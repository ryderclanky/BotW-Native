// Portable checks for meta.xml, iconTex.tga and mods.ini handling. Not part of the game.
#include "game_info.h"
#include "mods.h"
#include "setup_wizard.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fails = 0;

void expect(bool cond, const std::string& msg) {
    if (cond) return;
    std::cerr << "FAIL " << msg << "\n";
    g_fails++;
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 2x2 uncompressed 32-bit TGA, bottom-up rows.
std::vector<uint8_t> tiny_tga() {
    std::vector<uint8_t> f(18, 0);
    f[2] = 2;
    f[12] = 2;
    f[14] = 2;
    f[16] = 32;
    f[17] = 8;
    // bottom row: blue, green; top row: red, white (BGRA)
    const uint8_t px[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    f.insert(f.end(), px, px + sizeof px);
    return f;
}

void test_meta() {
    gameinfo::Info info;
    std::string xml =
        "<?xml version=\"1.0\"?><menu>"
        "<title_id type=\"hexBinary\" length=\"8\">00050000101c9400</title_id>"
        "<title_version type=\"unsignedInt\" length=\"4\">208</title_version>"
        "<longname_en type=\"string\" length=\"512\">Line one&#xA;Line &amp; two</longname_en>"
        "</menu>";
    expect(gameinfo::parse_meta(xml, info), "meta parses");
    expect(info.title_id == "00050000101C9400", "title id upper-case: " + info.title_id);
    expect(info.has_title_version && info.title_version == 208, "title version 208");
    expect(info.long_name == "Line one\nLine & two", "long name entities: " + info.long_name);
    expect(gameinfo::version_name(208) == "v1.5.0", "208 is v1.5.0");
    expect(gameinfo::region_name(info.title_id) == "USA", "region USA");
    gameinfo::Info bad;
    expect(!gameinfo::parse_meta("not xml", bad), "garbage rejected");
}

void test_tga() {
    gameinfo::Icon icon;
    expect(gameinfo::decode_tga(tiny_tga(), icon), "tga decodes");
    expect(icon.width == 2 && icon.height == 2 && icon.bgra.size() == 16, "tga size");
    if (icon.bgra.size() == 16) {
        expect(icon.bgra[2] == 255 && icon.bgra[0] == 0, "top-left is red after flip");
        expect(icon.bgra[8] == 255 && icon.bgra[9] == 0, "bottom-left is blue");
    }
    gameinfo::Icon none;
    expect(!gameinfo::decode_tga(std::vector<uint8_t>(10, 0), none), "short tga rejected");
}

void test_mods(const fs::path& root) {
    fs::remove_all(root);
    write_text(root / "mods" / "Beta" / "rules.txt",
               "[Definition]\nname = Beta Mod\ndescription = Second one\nversion = 2\n"
               "[Preset]\nname = Default Look\ncategory = Style\ndefault = 1\n"
               "[Preset]\nname = Dark Look\ncategory = Style\n");
    write_text(root / "mods" / "Beta" / "content" / "Actor" / "Pack" / "Link.sbactorpack", "beta-link");
    write_text(root / "mods" / "Alpha" / "rules.txt", "[Definition]\nname = \"Alpha\"\n");
    fs::create_directories(root / "mods" / "Alpha" / "aoc");
    // Direct Actor/ layout without content/ wrapper:
    write_text(root / "mods" / "Gamma" / "Actor" / "Pack" / "Link.sbactorpack", "gamma-link");
    write_text(root / "mods.ini", "; keep me\n[Enabled]\nBeta=1\nGone=1\n[Other]\nx=y\n");

    auto st = mods::load(root);
    expect(st.list.size() == 3, "three mods");
    if (st.list.size() == 3) {
        expect(st.list[0].folder == "Alpha" && st.list[1].folder == "Beta", "alphabetical without [Order]");
        expect(st.list[0].name == "Alpha", "quoted name: " + st.list[0].name);
        expect(st.list[1].name == "Beta Mod" && st.list[1].version == "2", "rules name and version");
        expect(st.list[1].presets.size() == 1 && st.list[1].presets[0].active == "Default Look", "default preset parsed");
        expect(st.list[2].name == "Gamma" && !st.list[2].has_rules, "no rules uses folder name");
        expect(!st.list[0].enabled && st.list[1].enabled && !st.list[2].enabled, "missing key is off");
        expect(st.list[0].has_aoc && st.list[1].has_content && st.list[2].has_content, "content and aoc detected");
    }
    expect(mods::enabled_count(st) == 1, "one enabled");
    expect(!st.has_order, "no order yet");

    // Enable Gamma, change Beta's preset, and move Gamma first.
    std::vector<mods::Mod> reordered;
    for (auto& m : st.list) {
        if (m.folder == "Gamma") {
            m.enabled = true;
            reordered.push_back(m);
        }
    }
    for (auto& m : st.list) {
        if (m.folder != "Gamma") {
            if (m.folder == "Beta" && !m.presets.empty()) {
                m.presets[0].active = "Dark Look";
            }
            reordered.push_back(m);
        }
    }
    st.list = reordered;
    std::string err;
    expect(mods::save(root, st, true, err), "save works: " + err);
    std::string ini = read_text(root / "mods.ini");
    expect(ini.find("order=Gamma,Alpha,Beta") != std::string::npos, "order written");
    expect(ini.find("Gone=1") != std::string::npos, "keys for missing folders kept");
    expect(ini.find("[Other]") != std::string::npos && ini.find("x=y") != std::string::npos, "other sections kept");
    expect(ini.find("Beta:Style=Dark Look") != std::string::npos, "preset persisted");

    auto again = mods::load(root);
    expect(again.has_order, "order read back");
    if (again.list.size() == 3) {
        expect(again.list[0].folder == "Gamma" && again.list[0].enabled, "Gamma first and on");
        expect(again.list[2].folder == "Beta" && again.list[2].enabled, "Beta last and on");
        expect(again.list[2].presets.size() == 1 && again.list[2].presets[0].active == "Dark Look", "preset read back");
        // Beta is after Gamma and both touch Actor/Pack/Link.sbactorpack -> Beta overrides Gamma
        expect(again.list[2].overrides_mods.size() == 1 && again.list[2].overrides_mods[0] == "Gamma", "Beta overrides Gamma");
        expect(again.list[0].overridden_by_mods.size() == 1 && again.list[0].overridden_by_mods[0] == "Beta Mod", "Gamma overridden by Beta Mod");
    }

    // Test installing a folder via mods::install_mod
    fs::path ext_mod = root / "external_source" / "CoolPack";
    write_text(ext_mod / "rules.txt", "[Definition]\nname = Cool Pack\nversion = 5\n");
    write_text(ext_mod / "Actor" / "Pack" / "Horse.sbactorpack", "horse-mod");
    std::string installed_folder;
    expect(mods::install_mod(root, ext_mod, installed_folder, err), "install_mod folder works: " + err);
    expect(installed_folder == "CoolPack", "installed folder name");
    auto after_install = mods::load(root);
    expect(after_install.list.size() == 4, "four mods after install");

    fs::remove_all(root);
}


void test_sha1_and_dumps(const fs::path& root) {
    expect(gameinfo::sha1_bytes("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 of abc");

    fs::remove_all(root);
    write_text(root / "app_only" / "code" / "app.xml",
               "<app><title_id>0005000E101C9400</title_id><title_version type=\"hexBinary\" length=\"2\">00D0</title_version></app>");
    auto app_info = gameinfo::read(root / "app_only");
    expect(app_info.title_id == "0005000E101C9400" && app_info.has_title_version && app_info.title_version == 208,
           "app.xml fallback parses hexadecimal v208");
    // Simulate a Dumpling / wiiuu parent folder with separate Base (v0), Update (v208), and DLC (v80)
    fs::path base = root / "wiiuu" / "BotW_Base";
    fs::path upd = root / "wiiuu" / "BotW_Update_v208";
    fs::path dlc = root / "wiiuu" / "BotW_DLC_v80";

    write_text(base / "meta" / "meta.xml",
               "<menu><title_id>00050000101C9400</title_id><title_version>0</title_version>"
               "<longname_en>The Legend of Zelda&#xA;Breath of the Wild</longname_en></menu>");
    write_text(base / "code" / "U-King.rpx", "base_v0_rpx");
    write_text(base / "content" / "Pack" / "Bootup.pack", "base_bootup");
    write_text(base / "content" / "Actor" / "Link.sbactorpack", "base_link");

    write_text(upd / "meta" / "meta.xml",
               "<menu><title_id>0005000E101C9400</title_id><title_version>208</title_version>"
               "<longname_en>The Legend of Zelda&#xA;Breath of the Wild</longname_en></menu>");
    write_text(upd / "code" / "U-King.rpx", "update_v208_rpx");
    write_text(upd / "content" / "Pack" / "Bootup.pack", "updated_bootup_v208");

    write_text(dlc / "meta" / "meta.xml",
               "<menu><title_id>0005000C101C9400</title_id><title_version>80</title_version></menu>");
    write_text(dlc / "content" / "0010" / "Pack" / "AocMainField.pack", "dlc_aoc_mainfield");

    auto layout = gameinfo::discover_dumps(root / "wiiuu");
    expect(layout.needs_merge, "separate dumps need merge");
    expect(layout.ready(), "separate base + update ready to merge");
    expect(layout.base_folder == base, "found base folder");
    expect(layout.update_folder == upd, "found update folder");
    expect(layout.dlc_folder == dlc, "found dlc folder");

    fs::path merged = root / "merged_game";
    std::string err;
    write_text(upd / "meta" / "meta.xml",
               "<menu><title_id>0005000E101C9500</title_id><title_version>208</title_version></menu>");
    expect(!gameinfo::discover_dumps(root / "wiiuu").ready(), "mixed regions are not ready");
    expect(!gameinfo::merge_dumps(base, upd, dlc, merged, err), "reject mismatched update region");
    expect(!fs::exists(merged), "invalid merge leaves destination untouched");
    write_text(upd / "meta" / "meta.xml",
               "<menu><title_id>0005000E101C9400</title_id><title_version>208</title_version></menu>");
    write_text(dlc / "meta" / "meta.xml",
               "<menu><title_id>0005000C101C9500</title_id><title_version>80</title_version></menu>");
    expect(!gameinfo::merge_dumps(base, upd, dlc, merged, err), "reject mismatched DLC region");
    write_text(dlc / "meta" / "meta.xml",
               "<menu><title_id>0005000C101C9400</title_id><title_version>80</title_version></menu>");
    write_text(upd / "meta" / "meta.xml",
               "<menu><title_id>0005000E101C9400</title_id><title_version>192</title_version></menu>");
    expect(!gameinfo::merge_dumps(base, upd, dlc, merged, err), "reject wrong update version");
    write_text(upd / "meta" / "meta.xml",
               "<menu><title_id>0005000E101C9400</title_id><title_version>208</title_version></menu>");
    expect(!gameinfo::merge_dumps(base, upd, dlc, base, err), "reject base as merge destination");
    expect(read_text(base / "code" / "U-King.rpx") == "base_v0_rpx", "rejected merge preserves base rpx");
    expect(!gameinfo::merge_dumps(base, upd, dlc, base / "nested", err), "reject destination inside base");
    expect(!gameinfo::merge_dumps(base, upd, dlc, root / "wiiuu", err), "reject destination containing sources");
#ifdef _WIN32
    std::wstring alternate_case = base.wstring();
    for (auto& ch : alternate_case) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    expect(!gameinfo::merge_dumps(base, upd, dlc, fs::path(alternate_case), err), "reject case alias of base");
    expect(read_text(base / "code" / "U-King.rpx") == "base_v0_rpx", "case alias preserves base rpx");
#endif
    err.clear();
    expect(gameinfo::merge_dumps(layout.base_folder, layout.update_folder, layout.dlc_folder, merged, err),
           "merge_dumps succeeds: " + err);

    // Base files must remain untouched while merged has Update overlay + Base files + DLC!
    expect(read_text(base / "code" / "U-King.rpx") == "base_v0_rpx", "base rpx untouched");
    expect(read_text(merged / "code" / "U-King.rpx") == "update_v208_rpx", "merged rpx from update");
    expect(read_text(merged / "content" / "Pack" / "Bootup.pack") == "updated_bootup_v208", "merged bootup from update");
    expect(read_text(merged / "content" / "Actor" / "Link.sbactorpack") == "base_link", "merged actor from base");
    expect(read_text(merged / "aoc" / "0010" / "Pack" / "AocMainField.pack") == "dlc_aoc_mainfield", "merged dlc aoc");

    auto minfo = gameinfo::read(merged);
    expect(minfo.has_rpx && minfo.has_content && minfo.has_meta && minfo.has_dlc, "merged game info complete");
    expect(minfo.title_version == 208, "merged version is 208");

    fs::path linked_base = root / "linked_base", linked_output = root / "linked_output";
    write_text(linked_base / "meta" / "meta.xml",
               "<menu><title_id>00050000101C9400</title_id><title_version>0</title_version></menu>");
    write_text(linked_output / "Pack" / "marker", "preserve");
    std::error_code symlink_ec;
    fs::create_directory_symlink(linked_output, linked_base / "content", symlink_ec);
    if (!symlink_ec) {
        expect(!gameinfo::merge_dumps(linked_base, upd, {}, linked_output, err), "reject source folder linked to target");
        expect(read_text(linked_output / "Pack" / "marker") == "preserve", "linked source remains untouched");
    }

    fs::remove_all(root);
}

void test_setup_wizard(const fs::path& root) {
    setupwizard::BuildProgress prog;
    expect(setupwizard::parse_progress_line("[recomp 1/4] Decompressing & parsing U-King.rpx...", prog), "parse recomp 1/4");
    expect(prog.stage == setupwizard::BuildStage::RecompileWiiUToCpp, "stage is RecompileWiiUToCpp");
    expect(prog.recompile_step == 1 && prog.recompile_total == 4, "step 1/4");

    expect(setupwizard::parse_progress_line("[recomp 3/4] Generating C++ shard 42/258 (16%) [fn_020c0000.cpp]...", prog), "parse recomp 3/4");
    expect(prog.sub_done == 42 && prog.sub_total == 258, "shard 42/258");
    expect(prog.current_item == "fn_020c0000.cpp", "current item fn_020c0000.cpp");
    expect(prog.phase_text.find("shard 42 / 258, 16%") != std::string::npos, "phase text has shard 42 / 258, 16%");

    expect(setupwizard::parse_progress_line("[recomp 4/4] Writing dispatch table & HLE stubs (100%)", prog), "parse recomp 4/4");
    expect(prog.stage_percent == 100, "recomp 4/4 stage 100%");

    expect(setupwizard::parse_progress_line("[412/1267] Building CXX object CMakeFiles/BotWRecomp.dir/generated/fn_02340000.cpp.obj", prog), "parse ninja [412/1267]");
    expect(prog.stage == setupwizard::BuildStage::CompileCppToExe, "stage is CompileCppToExe");
    expect(prog.sub_done == 412 && prog.sub_total == 1267, "412/1267");
    expect(prog.stage_percent == 32, "stage pct 32%");

    std::error_code ec;
    fs::remove_all(root, ec);
    write_text(root / "settings.ini", "# comment\nvolume=80\nscale=1\n");
    setupwizard::GamePrefs prefs;
    prefs.scale = 2;
    prefs.fps = 60;
    prefs.layout = 1;
    prefs.gamepad_view = true;
    prefs.gyro = false;
    expect(setupwizard::write_game_prefs(root, prefs), "write_game_prefs");
    auto r = setupwizard::read_game_prefs(root);
    expect(r.scale == 2 && r.fps == 60 && r.layout == 1 && r.gamepad_view && !r.gyro, "read_game_prefs round-trip");
    expect(read_text(root / "settings.ini").find("volume=80") != std::string::npos, "preserved volume=80");

    fs::path controller_repo = root / "controller_repo";
    fs::path runtime_dir = root / "runtime_install";
    write_text(controller_repo / "runtime" / "gamecontrollerdb.txt", "current mappings\n");
    write_text(runtime_dir / "gamecontrollerdb.txt", "stale mappings\n");
    setupwizard::deploy_runtime_files(runtime_dir, controller_repo);
    expect(read_text(runtime_dir / "gamecontrollerdb.txt") == "current mappings\n",
           "runtime install refreshes the controller database from runtime/");
    fs::remove_all(root, ec);
}

void test_codegen_cache(const fs::path& root) {
    fs::remove_all(root);
    write_text(root / "tools" / "recompile.py", "generator one");
    write_text(root / "cafe" / "codegen.py", "codegen one");
    write_text(root / "runtime" / "src" / "game" / "hooks.cpp", "BOTW_HOOK(0x10)");
    write_text(root / "generated" / "table.cpp", "table");
    std::string fingerprint = gameinfo::codegen_fingerprint(root, "");
    expect(!fingerprint.empty(), "fingerprint generation inputs");
    write_text(root / "generated" / ".codegen_complete", "rpxhash\n" + fingerprint + "\n");
    expect(gameinfo::codegen_cache_matches(root, "rpxhash", ""), "matching codegen cache reused");
    expect(!gameinfo::codegen_cache_matches(root, "other", ""), "changed RPX invalidates cache");
    expect(!gameinfo::codegen_cache_matches(root, "rpxhash", "1234"), "changed probes invalidate cache");
    write_text(root / "runtime" / "src" / "game" / "hooks.cpp", "BOTW_HOOK(0x20)");
    expect(!gameinfo::codegen_cache_matches(root, "rpxhash", ""), "changed hook invalidates cache");
    write_text(root / "runtime" / "src" / "game" / "hooks.cpp", "BOTW_HOOK(0x10)");
    write_text(root / "cafe" / "codegen.py", "codegen two");
    expect(!gameinfo::codegen_cache_matches(root, "rpxhash", ""), "changed generator invalidates cache");
    write_text(root / "cafe" / "codegen.py", "codegen one");
    write_text(root / "cafe" / "new_module.py", "new input");
    expect(!gameinfo::codegen_cache_matches(root, "rpxhash", ""), "new generator module invalidates cache");
    fs::remove_all(root);
}

}  // namespace

int main() {
    test_meta();
    test_tga();
    test_mods(fs::temp_directory_path() / "launcher_data_test");
    // Exercise dump discovery and merging under a path with spaces, an
    // ampersand, and non-ASCII text, as reported in Windows setup failures.
    test_sha1_and_dumps(fs::temp_directory_path() / fs::path(u8"launcher dump test λ &"));
    test_setup_wizard(fs::temp_directory_path() / "launcher_setup_test");
    test_codegen_cache(fs::temp_directory_path() / "launcher_codegen_test");
    if (g_fails) {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "launcher data tests passed\n";
    return 0;
}
