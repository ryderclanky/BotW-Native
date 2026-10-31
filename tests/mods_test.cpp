// Checks for the mod index and graphic-pack engine (runtime/src/coreinit/mods.cpp):
// mods.ini and rules.txt parsing, [Preset] categories/conditions, expression evaluation,
// [TextureRedefine], patches.txt data/code overrides, custom shader lookup, flexible
// layouts (direct Actor/ and nested wrapper unwrapping), UTF-8 filenames, move_order,
// reload, and .zip / folder installation.
#include "coreinit/mods.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) g_pass++;                                             \
        else {                                                          \
            g_fail++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

static std::string slurp(const std::string& p) {
    FILE* f = mods::fopen_utf8(p, "rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), f)) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

// Helper to build a minimal valid uncompressed .zip archive in memory.
static std::vector<uint8_t> make_stored_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    auto u16 = [](std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(static_cast<uint8_t>(x & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    };
    auto u32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(static_cast<uint8_t>(x & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
    };
    struct Entry {
        std::string name;
        std::string data;
        uint32_t offset;
    };
    std::vector<Entry> entries;
    std::vector<uint8_t> out;
    for (const auto& [name, data] : files) {
        uint32_t off = static_cast<uint32_t>(out.size());
        u32(out, 0x04034b50u);
        u16(out, 20);
        u16(out, 0);
        u16(out, 0);  // stored
        u16(out, 0);
        u16(out, 0);
        u32(out, 0);
        u32(out, static_cast<uint32_t>(data.size()));
        u32(out, static_cast<uint32_t>(data.size()));
        u16(out, static_cast<uint16_t>(name.size()));
        u16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        entries.push_back({name, data, off});
    }
    uint32_t cd_off = static_cast<uint32_t>(out.size());
    for (const auto& e : entries) {
        u32(out, 0x02014b50u);
        u16(out, 20);
        u16(out, 20);
        u16(out, 0);
        u16(out, 0);
        u16(out, 0);
        u16(out, 0);
        u32(out, 0);
        u32(out, static_cast<uint32_t>(e.data.size()));
        u32(out, static_cast<uint32_t>(e.data.size()));
        u16(out, static_cast<uint16_t>(e.name.size()));
        u16(out, 0);
        u16(out, 0);
        u16(out, 0);
        u16(out, 0);
        u32(out, 0);
        u32(out, e.offset);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_off;
    u32(out, 0x06054b50u);
    u16(out, 0);
    u16(out, 0);
    u16(out, static_cast<uint16_t>(entries.size()));
    u16(out, static_cast<uint16_t>(entries.size()));
    u32(out, cd_size);
    u32(out, cd_off);
    u16(out, 0);
    return out;
}

static void test_config() {
    auto c = mods::parse_config(
        "\xEF\xBB\xBF; comment\n[Enabled]\nAlpha = 1\r\nbeta=0\n  Gamma =1 \n# x\n[Order]\norder = Gamma, alpha , Gamma,,beta\n"
        "[Presets]\nAlpha = High\nGamma:Shadows = Ultra (4x)\n");
    CHECK(c.is_enabled("Alpha"));
    CHECK(c.is_enabled("ALPHA"));
    CHECK(!c.is_enabled("Beta"));
    CHECK(c.is_enabled("gamma"));
    CHECK(!c.is_enabled("Missing"));
    CHECK(c.has_order);
    CHECK((c.order == std::vector<std::string>{"Gamma", "alpha", "beta"}));
    CHECK(c.get_preset("alpha") == "High");
    CHECK(c.get_preset("GAMMA", "shadows") == "Ultra (4x)");

    auto none = mods::parse_config("");
    CHECK(!none.has_order && none.enabled.empty());

    // Keys outside [Enabled] do not enable anything; a repeated key uses the last value.
    auto odd = mods::parse_config("Alpha = 1\n[Other]\nBeta = 1\n[enabled]\nGamma = 1\nGamma = 0\n");
    CHECK(!odd.is_enabled("Alpha") && !odd.is_enabled("Beta") && !odd.is_enabled("Gamma"));
}

static void test_order() {
    std::vector<std::string> present = {"zeta", "Alpha", "mid", "Off"};
    auto cfg = mods::parse_config("[Enabled]\nzeta=1\nalpha=1\nMID=1\noff=0\n");
    CHECK((mods::load_order(cfg, present) == std::vector<std::string>{"Alpha", "mid", "zeta"}));
    // Listed ones in the given order; enabled but unlisted ones first, alphabetically.
    cfg = mods::parse_config("[Enabled]\nzeta=1\nalpha=1\nMID=1\noff=0\n[Order]\norder=zeta,off,Alpha,NotThere\n");
    CHECK((mods::load_order(cfg, present) == std::vector<std::string>{"mid", "zeta", "Alpha"}));
}

static void test_expressions() {
    std::vector<std::pair<std::string, double>> vars = {
        {"$width", 1920.0},
        {"$height", 1080.0},
        {"$scale", 1.5},
        {"fps", 60.0},
    };
    double out = 0.0;
    CHECK(mods::eval_expr("($width * 16) / 9", vars, &out) && std::fabs(out - 3413.333333) < 0.01);
    CHECK(mods::eval_expr("floor($scale * 1280)", vars, &out) && out == 1920.0);
    CHECK(mods::eval_expr("ceil(1.2) + round(2.6) + abs(-4)", vars, &out) && out == 9.0);
    CHECK(mods::eval_expr("min($width, $height) == 1080 ? 1 : 0", vars, &out) && out == 1.0);
    CHECK(mods::eval_expr("max(10, 20) > 15 && $fps >= 60", vars, &out) && out == 1.0);
    CHECK(mods::eval_expr("($width == 1280 || $height == 720) ? 10 : 20", vars, &out) && out == 20.0);
    CHECK(mods::eval_expr("0x100 + (17 % 5)", vars, &out) && out == 258.0);
}

static void test_rules() {
    auto r = mods::parse_rules(
        "[Definition]\ntitleIds = 00050000101C9300,00050000101C9400\nname = \"Better Things\"\npath = \"The Legend of Zelda: Breath "
        "of the Wild/Mods/Better\"\ndescription = Line one\\nline two\nversion = 7\n\n"
        "[Default]\n$base = 2\n\n"
        "[Preset]\nname = Standard\ncategory = Quality\ndefault = 1\n$scale = 1.0\n\n"
        "[Preset]\nname = Ultra\ncategory = Quality\n$scale = 2.0\n\n"
        "[Preset]\nname = Extra Sharp\ncategory = Detail\ncondition = ($scale >= 2.0)\n$sharp = 1\n\n"
        "[Preset]\nname = Normal Detail\ncategory = Detail\ndefault = 1\n$sharp = 0\n\n"
        "[TextureRedefine]\nwidth = 1280\nheight = 720\nformats = 0x001, 0x01a\nformatsExcluded = 0x031\n"
        "tileModes = 4\ntileModesExcluded = 1\noverwriteWidth = floor($scale * $base * 640)\n"
        "overwriteHeight = floor($scale * 720)\noverwriteLodBias = -0.5 * $scale\noverwriteAnisotropy = 8\n");
    CHECK(r.has_definition);
    CHECK(r.name == "Better Things");
    CHECK(r.description == "Line one line two");
    CHECK(r.version == "7");
    CHECK(r.title_ids.size() == 2 && r.title_ids[1] == "00050000101c9400");
    CHECK(r.presets.size() == 4);
    CHECK(r.texture_rules.size() == 1);
    auto empty = mods::parse_rules("name = x\n");
    CHECK(!empty.has_definition && empty.name.empty());
}

static void test_index(const fs::path& root) {
    fs::path game = root / "game", user = root / "user";
    put(game / "content" / "Actor" / "Pack" / "Link.sbactorpack", "base-link");
    put(game / "content" / "Actor" / "Pack" / "Horse.sbactorpack", "base-horse");
    put(game / "content" / "System" / "Version.txt", "base-version");
    put(game / "aoc" / "0010" / "Pack" / "AocMainField.pack", "dlc-pack");
    put(game / "aoc" / "0010" / "Map" / "Field.txt", "dlc-field");

    // ModA: lower case paths, replaces Link and adds a file.
    put(user / "mods" / "ModA" / "rules.txt", "[Definition]\nname = Mod A\ndescription = first\nversion = 2\n");
    put(user / "mods" / "ModA" / "content" / "actor" / "pack" / "link.sbactorpack", "A-link");
    put(user / "mods" / "ModA" / "content" / "Actor" / "Pack" / "NewThing.sbactorpack", "A-new");
    put(user / "mods" / "ModA" / "patch_fps.txt", "[x]\n");
    // ModB: also replaces Link, plus the DLC folder.
    put(user / "mods" / "ModB" / "Content" / "Actor" / "Pack" / "Link.sbactorpack", "B-link");
    put(user / "mods" / "ModB" / "aoc" / "0010" / "Map" / "Field.txt", "B-field");
    // ModC: present but disabled.
    put(user / "mods" / "ModC" / "content" / "System" / "Version.txt", "C-version");

    // Alphabetical: ModB loads after ModA and wins.
    put(user / "mods.ini", "[Enabled]\nModA = 1\nModB = 1\nModC = 0\n");
    mods::init(game.string(), user.string());
    CHECK(mods::loaded().size() == 2);
    CHECK(mods::loaded().size() == 2 && mods::loaded()[0].name == "Mod A" && mods::loaded()[0].version == "2");
    CHECK(mods::loaded().size() == 2 && mods::loaded()[1].name == "ModB");  // no rules.txt: folder name
    CHECK(mods::loaded().size() == 2 && mods::loaded()[0].content_files == 2 && mods::loaded()[0].replaces == 1);
    CHECK(mods::loaded().size() == 2 && mods::loaded()[1].aoc_files == 1 && mods::loaded()[1].replaces == 2);
    CHECK(mods::conflicts() == 1);

    auto q = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && q.overlay && slurp(q.host) == "B-link");
    q = mods::query("/vol/content/ACTOR/pack/LINK.SBACTORPACK");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "B-link");
    q = mods::query("/vol/content//Actor/./Pack/../Pack/newthing.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "A-new");
    q = mods::query("/vol/content/Actor/Pack/Horse.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && !q.overlay && slurp(q.host) == "base-horse");
    q = mods::query("/vol/content/actor/pack/horse.sbactorpack");  // base lookups ignore case too
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "base-horse");
    q = mods::query("/vol/content/System/Version.txt");  // ModC is off
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "base-version");
    q = mods::query("/vol/content/Actor/Pack/Nope.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::Missing);
    q = mods::query("/vol/content/Actor");
    CHECK(q.kind == mods::Query::Kind::Dir);
    q = mods::query("/vol/save/80000001/game_data.sav");
    CHECK(q.kind == mods::Query::Kind::Other);

    // DLC through the title-suffixed mount the game uses.
    CHECK(mods::dlc_ready());
    CHECK(mods::owns_content(0x0010));
    std::string aoc = mods::aoc_mount_path();
    q = mods::query(aoc + "/0010/Map/Field.txt");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "B-field");
    q = mods::query(aoc + "/0010/Pack/AocMainField.pack");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "dlc-pack");

    std::vector<mods::DirItem> items;
    CHECK(mods::list_dir("/vol/content/actor/Pack", items) == mods::ListStatus::Ok);
    CHECK(items.size() == 3);
    if (items.size() == 3) {
        CHECK(items[0].name == "Horse.sbactorpack" && !items[0].overlay);
        CHECK(items[1].name == "Link.sbactorpack" && items[1].overlay && items[1].size == 6);
        CHECK(items[2].name == "NewThing.sbactorpack" && items[2].overlay);
    }
    CHECK(mods::list_dir("/vol/content/Actor/Pack/Link.sbactorpack", items) == mods::ListStatus::NotDir);
    CHECK(mods::list_dir("/vol/content/NoSuchDir", items) == mods::ListStatus::Missing);
    CHECK(mods::list_dir("/vol/save", items) == mods::ListStatus::NotVolume);
    CHECK(mods::read_only_volume("/vol/content/x") && mods::read_only_volume("/vol/code/x") && !mods::read_only_volume("/vol/save/x"));
    CHECK(mods::summary() == "2: ModB, Mod A");

    // [Order] reverses priority: ModA now wins.
    put(user / "mods.ini", "[Enabled]\nmoda = 1\nmodb = 1\n[Order]\norder = ModB, ModA\n");
    mods::init(game.string(), user.string());
    q = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::File && slurp(q.host) == "A-link");
    CHECK(mods::loaded().size() == 2 && mods::loaded()[1].folder == "ModA" && mods::loaded()[0].lost == 1);

    // --no-mods: content goes straight to the host path again.
    mods::init(game.string(), user.string(), false);
    CHECK(mods::disabled_by_flag() && mods::loaded().empty());
    q = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q.kind == mods::Query::Kind::Other);
    CHECK(mods::list_dir("/vol/content/Actor", items) == mods::ListStatus::NotVolume);
    CHECK(mods::summary() == "Off for this run (--no-mods)");

    mods::init(game.string(), user.string(), true, true, 0x00050000101c9500ull);
    CHECK(mods::aoc_title_id() == 0x0005000c101c9500ull);
    mods::init(game.string(), user.string(), true, false);
    CHECK(!mods::dlc_ready() && !mods::owns_content(0x0010));
    CHECK(mods::query(aoc + "/0010/Pack/AocMainField.pack").kind == mods::Query::Kind::Missing);
    CHECK(mods::list_dir(aoc + "/0010", items) == mods::ListStatus::Missing);
    CHECK(mods::query("/vol/content/Actor/Pack/Link.sbactorpack").kind == mods::Query::Kind::File);
    mods::reload();
    CHECK(!mods::dlc_ready() && mods::query(aoc + "/0010/Pack/AocMainField.pack").kind == mods::Query::Kind::Missing);

    // No mods.ini: nothing is enabled, but status() still lists discovered mods and set_enabled() updates mods.ini.
    fs::remove(user / "mods.ini");
    mods::init(game.string(), user.string());
    CHECK(mods::loaded().empty() && mods::index_files() == 0);
    CHECK(mods::query("/vol/content/System/Version.txt").kind == mods::Query::Kind::Other);
    CHECK(mods::summary() == "None loaded");
    auto st = mods::status();
    CHECK(st.size() == 3 && !st[0].enabled && !st[0].loaded_now);
    CHECK(!mods::pending_restart());
    CHECK(mods::set_enabled("ModA", true));
    CHECK(mods::pending_restart());
    st = mods::status();
    CHECK(st.size() == 3 && st[0].folder == "ModA" && st[0].enabled && !st[0].loaded_now);
    CHECK(mods::set_enabled("ModA", false));
    CHECK(!mods::pending_restart());
}

static void test_presets_textures_shaders_and_patches(const fs::path& root) {
    fs::path game = root / "game2", user = root / "user2";
    put(game / "content" / "System" / "Version.txt", "1.5.0");

    // Mod with multi-category presets, TextureRedefine, custom shader, and patches.txt
    put(user / "mods" / "GraphicsMod" / "rules.txt",
        "[Definition]\n"
        "titleIds = 00050000101C9400\n"
        "name = Graphics & Gameplay Pack\n"
        "version = 4\n\n"
        "[Default]\n"
        "$baseScale = 1.0\n\n"
        "[Preset]\n"
        "name = 1080p\n"
        "category = Resolution\n"
        "default = 1\n"
        "$resScale = 1.5\n\n"
        "[Preset]\n"
        "name = 4K\n"
        "category = Resolution\n"
        "$resScale = 3.0\n\n"
        "[Preset]\n"
        "name = Normal Speed\n"
        "category = Speed\n"
        "default = 1\n"
        "$speedMult = 1.0\n\n"
        "[Preset]\n"
        "name = Fast Speed\n"
        "category = Speed\n"
        "$speedMult = 2.5\n\n"
        "[TextureRedefine]\n"
        "width = 1280\n"
        "height = 720\n"
        "formats = 0x001, 0x01a\n"
        "overwriteWidth = floor($resScale * 1280)\n"
        "overwriteHeight = floor($resScale * 720)\n"
        "overwriteLodBias = -0.5\n");

    put(user / "mods" / "GraphicsMod" / "shaders" / "0x3904895d1e2b3c4d_ps.txt",
        "#version 450\n#define $resScale 1\nvoid main() { float s = $speedMult; }\n");

    put(user / "mods" / "GraphicsMod" / "patches.txt",
        "[BotW_GameplayPatch]\n"
        "moduleMatches = 0x6fd41a61\n"
        "_speedAddr = 0x10500100\n"
        "0x10500100 = .float ($speedMult * 4.0)\n"
        "0x10500104 = .int 123456\n"
        "0x10500108 = .short 0x1234\n"
        "0x1050010A = .byte 0x7F\n"
        "0x10500110 = .string \"BOTW\"\n"
        "0x02500100 = blr\n"
        "0x02500200 = li r3, 42\n"
        "0x02500204 = blr\n"
        "0x02500300 = b 0x02500400\n"
        "0x02500508 = nop\n");

    // Set up simulated guest memory and dispatch hooks
    static std::map<uint32_t, uint8_t> sim_mem;
    static std::map<uint32_t, std::string> sim_hooks;
    static std::map<uint32_t, uint32_t> sim_ret_vals;
    static std::map<uint32_t, uint32_t> sim_redirects;
    sim_mem.clear();
    sim_hooks.clear();
    sim_ret_vals.clear();
    sim_redirects.clear();

    // Seed original memory bytes so we can verify reload() restores them
    sim_mem[0x10500100] = 0xAA;
    sim_mem[0x02500100] = 0x7C;

    mods::PatchBackend backend;
    backend.read_u8 = [](uint32_t addr) -> uint8_t {
        return sim_mem[addr];
    };
    backend.write_u8 = [](uint32_t addr, uint8_t val) {
        sim_mem[addr] = val;
    };
    backend.is_fn_entry = [](uint32_t addr) -> bool {
        return addr == 0x02500100 || addr == 0x02500200 || addr == 0x02500300 || addr == 0x02500400;
    };
    backend.override_nop = [](uint32_t addr) {
        sim_hooks[addr] = "nop";
    };
    backend.override_ret_imm = [](uint32_t addr, uint32_t imm) {
        sim_hooks[addr] = "ret";
        sim_ret_vals[addr] = imm;
    };
    backend.redirect_fn = [](uint32_t addr, uint32_t target) -> bool {
        sim_hooks[addr] = "redirect";
        sim_redirects[addr] = target;
        return true;
    };
    backend.restore_fn_overrides = []() {
        sim_hooks.clear();
        sim_ret_vals.clear();
        sim_redirects.clear();
    };
    mods::set_patch_backend(backend);

    put(user / "mods.ini", "[Enabled]\nGraphicsMod = 1\n");
    mods::init(game.string(), user.string());

    CHECK(mods::loaded().size() == 1);
    const auto& tex = mods::loaded()[0].texture_rules;
    CHECK(tex.size() == 1);
    if (tex.size() == 1) {
        CHECK(tex[0].width.size() == 1 && tex[0].width[0] == 1280);
        CHECK(tex[0].height.size() == 1 && tex[0].height[0] == 720);
        CHECK(tex[0].overwrite_width == 1920 && tex[0].overwrite_height == 1080);
        CHECK(tex[0].formats.size() == 2);
    }

    // Shader lookup with default presets ($resScale = 1.5, $speedMult = 1.0)
    auto shader_opt = mods::find_custom_shader(0x3904895d1e2b3c4dULL, 0, mods::ShaderStage::Pixel);
    CHECK(shader_opt.has_value());
    if (shader_opt) {
        CHECK(shader_opt->find("1.5") != std::string::npos);
        CHECK(shader_opt->find("float s = 1;") != std::string::npos);
    }

    // Apply patches and inspect simulated guest memory + function entry hooks
    mods::apply_patches();
    auto read_be32 = [&](uint32_t a) -> uint32_t {
        return (uint32_t(sim_mem[a]) << 24) | (uint32_t(sim_mem[a + 1]) << 16) |
               (uint32_t(sim_mem[a + 2]) << 8) | uint32_t(sim_mem[a + 3]);
    };
    float fval = 0.0f;
    uint32_t fbits = read_be32(0x10500100);
    std::memcpy(&fval, &fbits, 4);
    CHECK(std::fabs(fval - 4.0f) < 0.001f);
    CHECK(read_be32(0x10500104) == 123456u);
    CHECK(sim_mem[0x10500108] == 0x12 && sim_mem[0x10500109] == 0x34);
    CHECK(sim_mem[0x1050010A] == 0x7F);
    CHECK(sim_mem[0x10500110] == 'B' && sim_mem[0x10500113] == 'W' && sim_mem[0x10500114] == 0);
    CHECK(sim_hooks[0x02500100] == "nop");
    CHECK(sim_hooks[0x02500200] == "ret" && sim_ret_vals[0x02500200] == 42u);
    CHECK(sim_hooks[0x02500300] == "redirect" && sim_redirects[0x02500300] == 0x02500400u);
    CHECK(mods::loaded()[0].code_patches_applied == 3);
    CHECK(mods::loaded()[0].mid_fn_patches_skipped == 1);

    // Switch presets to 4K + Fast Speed and hot-reload!
    CHECK(mods::set_preset("GraphicsMod", "Resolution", "4K"));
    CHECK(mods::set_preset("GraphicsMod", "Speed", "Fast Speed"));
    mods::reload();

    const auto& tex2 = mods::loaded()[0].texture_rules;
    CHECK(tex2.size() == 1 && tex2[0].overwrite_width == 3840 && tex2[0].overwrite_height == 2160);
    fbits = read_be32(0x10500100);
    std::memcpy(&fval, &fbits, 4);
    CHECK(std::fabs(fval - 10.0f) < 0.001f);

    // Disable mod and reload: original memory byte and function hooks are restored!
    CHECK(mods::set_enabled("GraphicsMod", false));
    mods::reload();
    CHECK(sim_mem[0x10500100] == 0xAA);
    CHECK(sim_hooks.empty());
}

static void test_flexible_layouts_unicode_reorder_and_install(const fs::path& root) {
    fs::path game = root / "game3", user = root / "user3";
    put(game / "content" / "Actor" / "Pack" / "Link.sbactorpack", "base-link");

    // 1. Direct BotW content layout (<Mod>/Actor/Pack/... without content/)
    put(user / "mods" / "DirectLayoutMod" / "Actor" / "Pack" / "Link.sbactorpack", "direct-link");

    // 2. Single-child nested wrapper folder (<Mod>/InnerWrapper/content/...) with UTF-8 filename
    fs::path unicode_dir = user / "mods" / "NestedMod" / "InnerWrapper_v1" / "content" / "Actor" / "Pack";
    fs::create_directories(unicode_dir);
    put(unicode_dir / fs::path(u8"Épée_日本語.sbactorpack"), "unicode-sword");

    put(user / "mods.ini", "[Enabled]\nDirectLayoutMod = 1\nNestedMod = 1\n[Order]\norder = DirectLayoutMod, NestedMod\n");
    mods::init(game.string(), user.string());

    auto q1 = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q1.kind == mods::Query::Kind::File && q1.overlay && slurp(q1.host) == "direct-link");

    auto q2 = mods::query(std::string("/vol/content/Actor/Pack/\xC3\x89p\xC3\xA9" "e_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.sbactorpack"));
    CHECK(q2.kind == mods::Query::Kind::File && q2.overlay && slurp(q2.host) == "unicode-sword");

    // 3. Install a mod from a .zip archive using mods::install_from_path
    auto zip_bytes = make_stored_zip({
        {"ZipModWrapper/rules.txt", "[Definition]\nname = Zip Installed Mod\nversion = 1.0\n"},
        {"ZipModWrapper/content/Actor/Pack/Link.sbactorpack", "zip-link"},
    });
    fs::path zip_path = root / "ZipModArchive.zip";
    {
        std::ofstream out(zip_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(zip_bytes.data()), static_cast<std::streamsize>(zip_bytes.size()));
    }
    std::string installed_folder, err;
    CHECK(mods::install_from_path(zip_path.string(), &installed_folder, &err));
    CHECK(installed_folder == "ZipModArchive");

    mods::reload();
    CHECK(mods::loaded().size() == 3);
    auto q3 = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q3.kind == mods::Query::Kind::File && slurp(q3.host) == "zip-link");

    // 4. Test move_order: move ZipModArchive earlier (-1 twice) so DirectLayoutMod beats it
    CHECK(mods::move_order("ZipModArchive", -1));
    CHECK(mods::move_order("ZipModArchive", -1));
    mods::reload();
    q3 = mods::query("/vol/content/Actor/Pack/Link.sbactorpack");
    CHECK(q3.kind == mods::Query::Kind::File && slurp(q3.host) == "direct-link");
}

int main() {
    fs::path root = fs::temp_directory_path() / "botw_mods_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    test_config();
    test_order();
    test_expressions();
    test_rules();
    test_index(root);
    test_presets_textures_shaders_and_patches(root);
    test_flexible_layouts_unicode_reorder_and_install(root);
    fs::remove_all(root, ec);
    printf("mods_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
