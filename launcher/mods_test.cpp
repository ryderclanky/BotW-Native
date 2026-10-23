// Portable checks for the mod overlay and the launcher's mod installer. No game, no GUI.
#include "coreinit/mods.h"
#include "mods.h"
#include "unzip.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

void write_text(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << body;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void w16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
void w32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 24));
}

std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::string>>& items) {
    std::vector<uint8_t> out;
    struct Cd {
        std::string name;
        uint32_t size;
        uint32_t local;
    };
    std::vector<Cd> cds;
    for (const auto& item : items) {
        Cd cd{item.first, static_cast<uint32_t>(item.second.size()), static_cast<uint32_t>(out.size())};
        w32(out, 0x04034b50);
        w16(out, 20);
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w32(out, 0);
        w32(out, cd.size);
        w32(out, cd.size);
        w16(out, static_cast<uint16_t>(item.first.size()));
        w16(out, 0);
        out.insert(out.end(), item.first.begin(), item.first.end());
        out.insert(out.end(), item.second.begin(), item.second.end());
        cds.push_back(cd);
    }
    uint32_t cd_off = static_cast<uint32_t>(out.size());
    for (const auto& cd : cds) {
        w32(out, 0x02014b50);
        w16(out, 20);
        w16(out, 20);
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w32(out, 0);
        w32(out, cd.size);
        w32(out, cd.size);
        w16(out, static_cast<uint16_t>(cd.name.size()));
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w32(out, 0);
        w32(out, cd.local);
        out.insert(out.end(), cd.name.begin(), cd.name.end());
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size() - cd_off);
    w32(out, 0x06054b50);
    w16(out, 0);
    w16(out, 0);
    w16(out, static_cast<uint16_t>(cds.size()));
    w16(out, static_cast<uint16_t>(cds.size()));
    w32(out, cd_size);
    w32(out, cd_off);
    w16(out, 0);
    return out;
}

fs::path scratch() {
    auto p = fs::temp_directory_path() / "botw-mod-test";
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void test_ini() {
    auto lines = mods::parse_ini("# comment\n\n+Alpha\n-Beta\nGamma\n+../etc\n+ \n");
    expect(lines.size() == 3, "ini keeps three legal lines");
    expect(lines[0].folder == "Alpha" && lines[0].enabled, "plus is on");
    expect(lines[1].folder == "Beta" && !lines[1].enabled, "minus is off");
    expect(lines[2].folder == "Gamma" && lines[2].enabled, "bare name is on");
    auto again = mods::parse_ini(mods::format_ini(lines));
    expect(again.size() == lines.size() && again[1].folder == "Beta" && !again[1].enabled, "ini round trip");
}

void test_overlay(const fs::path& root) {
    fs::path game = root / "game";
    fs::path user = root / "user";
    write_text(game / "content" / "Actor" / "A.txt", "BASEA");
    write_text(game / "content" / "Actor" / "B.txt", "BASEB");
    write_text(game / "content" / "Pack" / "P.txt", "P");
    write_text(game / "meta" / "meta.xml", "<title_id type=\"hex\">00050000101C9500</title_id>\n");
    write_text(game / "aoc" / "content" / "0010" / "Pack" / "AocMainField.pack", "GAMEPACK");
    write_text(user / "mods" / "First" / "content" / "Actor" / "A.txt", "MOD1A");
    write_text(user / "mods" / "First" / "content" / "Actor" / "C.txt", "MOD1C");
    write_text(user / "mods" / "First" / "mod.ini", "name=First Mod\n");
    write_text(user / "mods" / "Off" / "content" / "Actor" / "A.txt", "NO");
    write_text(user / "mods" / "Off" / "mod.ini", "name=Off Mod\n");
    write_text(user / "mods" / "Second" / "content" / "Actor" / "A.txt", "MOD2A");
    write_text(user / "mods" / "Second" / "content" / "New" / "N.txt", "N");
    write_text(user / "mods" / "Second" / "aoc" / "0010" / "Pack" / "AocMainField.pack", "MODPACK");
    write_text(user / "mods" / "Second" / "mod.ini", "name=Second Mod\nwarning=sample\n");
    write_text(user / "mods" / "mods.ini", "# test\n+First\n-Off\n+Second\n");

    mods::init(game.string(), user.string());
    expect(mods::aoc_title_id() == 0x0005000C101C9500ull, "EU aoc title id");
    expect(mods::aoc_mount_path() == "/vol/aoc0005000c101c9500", "mount path");
    expect(mods::dlc_ready(), "pack marks DLC installed");
    expect(mods::owns_content(0x0010), "content 0010 is owned");
    expect(!mods::owns_content(0x0001), "unknown content is not owned");

    auto a = mods::query("/vol/content/actor/a.txt");
    expect(a.kind == mods::Query::Kind::File && a.overlay && read_text(a.host) == "MOD2A", "last mod wins, case insensitive");
    auto b = mods::query("/vol/content/Actor/B.txt");
    expect(b.kind == mods::Query::Kind::File && !b.overlay && read_text(b.host) == "BASEB", "base file still used");
    auto c = mods::query("/vol/content/Actor/C.txt");
    expect(c.kind == mods::Query::Kind::File && c.overlay && read_text(c.host) == "MOD1C", "earlier mod fills gaps");
    auto n = mods::query("/vol/content/New/N.txt");
    expect(n.kind == mods::Query::Kind::File && read_text(n.host) == "N", "added file");
    expect(read_text(game / "content" / "Actor" / "A.txt") == "BASEA", "base dump was not modified");

    std::vector<mods::DirItem> top;
    expect(mods::list_dir("/vol/content", top) == mods::ListStatus::Ok, "list content");
    bool saw_actor = false, saw_pack = false, saw_new = false;
    for (const auto& e : top) {
        if (e.name == "Actor" && e.dir) saw_actor = true;
        if (e.name == "Pack" && e.dir) saw_pack = true;
        if (e.name == "New" && e.dir && e.overlay) saw_new = true;
    }
    expect(saw_actor && saw_pack && saw_new, "merged top directory");

    std::vector<mods::DirItem> actor;
    expect(mods::list_dir("/vol/content/Actor", actor) == mods::ListStatus::Ok, "list actor");
    bool a_ok = false, b_ok = false, c_ok = false;
    for (const auto& e : actor) {
        if (e.name == "A.txt" && !e.dir && e.overlay && e.size == 5) a_ok = true;
        if (e.name == "B.txt" && !e.overlay && e.size == 5) b_ok = true;
        if (e.name == "C.txt" && e.overlay && e.size == 5) c_ok = true;
    }
    expect(a_ok && b_ok && c_ok, "merged actor directory");

    auto pack = mods::query("/vol/aoc0005000C101C9500/0010/Pack/AocMainField.pack");
    expect(pack.kind == mods::Query::Kind::File && pack.overlay && read_text(pack.host) == "MODPACK", "mod aoc pack wins");
    expect(read_text(game / "aoc" / "content" / "0010" / "Pack" / "AocMainField.pack") == "GAMEPACK", "game aoc pack unchanged");

    expect(mods::read_only_volume("/vol/content/Actor/A.txt"), "content is read only");
    expect(mods::read_only_volume("/vol/code/U-King.rpx"), "code is read only");
    expect(mods::read_only_volume("/vol/aoc0005000c101c9500/0010"), "aoc is read only");
    expect(!mods::read_only_volume("/vol/save/80000001/option.sav"), "saves stay writable");

    mods::note_opened("/vol/content/Actor/A.txt");
    mods::note_opened("/vol/content/actor/a.txt");
    mods::note_opened("/vol/content/New/N.txt");
    expect(mods::opened_files() == 2, "opened paths are counted once per file");
    expect(mods::replacing_files() >= 1, "at least one base file is overridden");
}

void test_dlc_layouts(const fs::path& root) {
    fs::path game = root / "game2";
    fs::path user = root / "user2";
    write_text(game / "content" / "Pack" / "P.txt", "P");
    write_text(user / "aoc" / "0010" / "Pack" / "AocMainField.pack", "USERPACK");
    mods::init(game.string(), user.string());
    expect(mods::dlc_ready(), "user aoc folder enables DLC");
    expect(mods::aoc_title_id() == 0x0005000C101C9400ull, "missing meta assumes USA");
    auto pack = mods::query("/vol/aoc0005000c101c9400/0010/Pack/AocMainField.pack");
    expect(pack.overlay && read_text(pack.host) == "USERPACK", "UKMM aoc/0010 layout");

    fs::path game3 = root / "game3";
    fs::path user3 = root / "user3";
    write_text(game3 / "content" / "Pack" / "P.txt", "P");
    write_text(user3 / "mods" / "Tiny" / "aoc" / "0010" / "Map" / "x.txt", "tiny");
    write_text(user3 / "mods" / "Tiny" / "mod.ini", "name=Tiny\n");
    write_text(user3 / "mods" / "mods.ini", "+Tiny\n");
    mods::init(game3.string(), user3.string());
    expect(!mods::dlc_ready(), "a small aoc file does not mark DLC installed");
    expect(!mods::owns_content(0x0010), "purchase stays empty without the pack");
    auto tiny = mods::query("/vol/aoc/0010/Map/x.txt");
    expect(tiny.kind == mods::Query::Kind::File && read_text(tiny.host) == "tiny", "aoc file still resolves");
}

void test_install(const fs::path& root) {
    fs::path user = root / "launch";
    fs::path pack = root / "drop" / "Wrapped" / "CoolPack";
    write_text(pack / "rules.txt", "[Definition]\ntitleIds = 00050000101C9400\nname = \"Cool Sword\"\n\n[Shader]\nvertexShader = nope\n");
    write_text(pack / "content" / "Actor" / "Hello.txt", "hi");
    write_text(pack / "patch_fps.asm", "nop");
    write_text(pack / "aoc" / "0010" / "Pack" / "AocMainField.pack", "PACK");
    auto found = modman::inspect(root / "drop");
    expect(found.recognized && found.display_name == "Cool Sword", "rules.txt name");
    expect(found.patches && found.shaders && found.has_files, "patches and shaders detected");
    expect(found.warning.find("This mod needs code patches, which this version can't run") != std::string::npos, "code-patch warning");
    expect(found.warning.find("This mod replaces shaders, which this version can't run") != std::string::npos, "shader warning");
    auto added = modman::install_tree(user, root / "drop", "CoolPack.zip");
    expect(added.ok && added.enabled, "data pack installs on");
    expect(fs::is_regular_file(user / "mods" / "CoolPack.zip" / "content" / "Actor" / "Hello.txt"), "content copied");
    expect(fs::is_regular_file(user / "mods" / "CoolPack.zip" / "rules.txt"), "rules copied");
    expect(!fs::exists(user / "mods" / "CoolPack.zip" / "patch_fps.asm"), "asm patch is not copied");
    expect(read_text(user / "mods" / "mods.ini").find("+CoolPack.zip") != std::string::npos, "installed on in mods.ini");

    fs::path zip_dir = root / "zip-src";
    write_text(zip_dir / "Outer" / "Inner" / "rules.txt", "name = Plain Files\n[TextureRedefine]\nwidth = 1\n");
    write_text(zip_dir / "Outer" / "Inner" / "content" / "Actor" / "Z.txt", "z");
    std::string err;
    fs::path zip = root / "plain.zip";
    {
        auto bytes = make_zip({{"Outer/Inner/rules.txt", read_text(zip_dir / "Outer" / "Inner" / "rules.txt")},
                               {"Outer/Inner/content/Actor/Z.txt", "z"}});
        std::ofstream out(zip, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    fs::path extracted = root / "zip-out";
    expect(unzip_to(zip, extracted, err), err);
    auto zfound = modman::inspect(extracted);
    expect(zfound.recognized && zfound.display_name == "Plain Files" && zfound.textures, "nested zip content and texture rules");
    auto zadd = modman::install_tree(user, extracted, "plain");
    expect(zadd.ok && zadd.enabled, "nested pack installs on");
    expect(fs::is_regular_file(user / "mods" / "plain" / "content" / "Actor" / "Z.txt"), "zip content installed");

    fs::path sw = root / "switch" / "atmosphere" / "contents";
    write_text(sw / "romfs" / "pack", "nope");
    write_text(root / "switch" / "exefs" / "main", "nso");
    auto swf = modman::inspect(root / "switch");
    expect(swf.recognized && swf.switch_mod, "switch layout recognized");
    expect(swf.warning.find("This mod is for the Switch version") != std::string::npos, "switch warning");
    auto swadd = modman::install_tree(user, root / "switch", "SwitchPack");
    expect(swadd.ok && !swadd.enabled, "switch mod is added turned off");
    auto rows = modman::list_mods(user);
    bool saw_off = false;
    for (const auto& row : rows)
        if (row.folder == "SwitchPack" && !row.enabled && row.warning.find("Switch version") != std::string::npos) saw_off = true;
    expect(saw_off, "switch mod listed off");

    auto off = modman::disable_all(user);
    expect(off.ok, off.message);
    rows = modman::list_mods(user);
    bool any_on = false;
    for (const auto& row : rows)
        if (row.enabled) any_on = true;
    expect(!any_on && !rows.empty(), "turn all mods off");
    auto removed = modman::remove_mod(user, "plain");
    expect(removed.ok && !fs::exists(user / "mods" / "plain"), "remove deletes the mod folder");
    expect(fs::is_regular_file(user / "mods" / "CoolPack.zip" / "content" / "Actor" / "Hello.txt"), "other mods stay");
}

}  // namespace

int main() {
    auto root = scratch();
    test_ini();
    test_overlay(root);
    test_dlc_layouts(root);
    test_install(root);
    fs::remove_all(root);
    if (g_fails) {
        std::cerr << g_fails << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
