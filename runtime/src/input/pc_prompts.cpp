// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was
// not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Derived from Zelda-on-PC src/pcedition/PCPrompts.cpp (https://github.com/mircea32000/Zelda-on-PC): Yaz0, SARC,
// RSTB and prompt-font handling and the button-code -> action table. Adapted to BotW-Native's bindings
// (pc_controls.ini) and mod layer.
#include "pc_prompts.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "../coreinit/mods.h"
#include "../log.h"
#include "../utf8_path.h"
#include "pc_controls.h"

namespace pcprompts {
namespace {
namespace fs = std::filesystem;
using u8 = uint8_t;
using Bytes = std::vector<u8>;

bool g_active = false;
Bytes g_template;                            // prompt font
std::unordered_map<uint16_t, uint16_t> g_cmap;  // template: code -> glyph index

// ---- byte helpers (big endian) --------------------------------------------------------------------------
uint16_t RB16(const u8* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t RB32(const u8* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void WB16(u8* p, uint16_t v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }
void WB32(u8* p, uint32_t v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }

bool read_file(const std::string& path, Bytes& out) {
    FILE* f = botw::fopen_utf8(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool write_file_atomic(const fs::path& p, const Bytes& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    FILE* f = botw::fopen_utf8(botw::path_to_utf8(tmp), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) return false;
    fs::remove(p, ec);  // (MinGW's rename does not replace an existing file)
    fs::rename(tmp, p, ec);
    return !ec;
}

// ---- Yaz0 -----------------------------------------------------------------------------------------------
bool yaz0_decode(const Bytes& in, Bytes& out) {
    if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4) != 0) return false;
    const uint32_t size = RB32(in.data() + 4);
    out.assign(size, 0);
    size_t src = 16, dst = 0;
    while (dst < size && src < in.size()) {
        const u8 flags = in[src++];
        for (int bit = 7; bit >= 0 && dst < size; bit--) {
            if (flags & (1 << bit)) {
                if (src >= in.size()) return false;
                out[dst++] = in[src++];
            } else {
                if (src + 1 >= in.size()) return false;
                const u8 b1 = in[src++], b2 = in[src++];
                const size_t dist = ((size_t)(b1 & 0x0F) << 8 | b2) + 1;
                size_t len = b1 >> 4;
                if (len == 0) {
                    if (src >= in.size()) return false;
                    len = (size_t)in[src++] + 0x12;
                } else {
                    len += 2;
                }
                if (dist > dst) return false;
                for (size_t i = 0; i < len && dst < size; i++, dst++) out[dst] = out[dst - dist];
            }
        }
    }
    return dst == size;
}

// literal-only Yaz0 (valid for every decoder; the game decompresses it as usual)
Bytes yaz0_store(const Bytes& in, uint32_t alignment) {
    Bytes out(16, 0);
    out.reserve(16 + in.size() + in.size() / 8 + 8);
    memcpy(out.data(), "Yaz0", 4);
    WB32(out.data() + 4, (uint32_t)in.size());
    WB32(out.data() + 8, alignment);
    for (size_t i = 0; i < in.size(); i += 8) {
        out.push_back(0xFF);
        const size_t n = std::min<size_t>(8, in.size() - i);
        out.insert(out.end(), in.begin() + i, in.begin() + i + n);
    }
    return out;
}

// ---- SARC (big endian) ----------------------------------------------------------------------------------
struct SarcFile {
    uint32_t hash = 0, attr = 0;
    std::string name;
    Bytes data;
};

bool sarc_read(const Bytes& d, std::vector<SarcFile>& files, uint32_t& hash_key) {
    if (d.size() < 0x20 || memcmp(d.data(), "SARC", 4) != 0 || RB16(d.data() + 6) != 0xFEFF) return false;
    const uint32_t data_off = RB32(d.data() + 0x0C);
    const u8* sfat = d.data() + RB16(d.data() + 4);
    if (memcmp(sfat, "SFAT", 4) != 0) return false;
    const uint16_t count = RB16(sfat + 6);
    hash_key = RB32(sfat + 8);
    const u8* nodes = sfat + 0x0C;
    const u8* sfnt = nodes + count * 0x10;
    if (sfnt + 8 > d.data() + d.size() || memcmp(sfnt, "SFNT", 4) != 0) return false;
    const u8* names = sfnt + RB16(sfnt + 4);
    files.clear();
    for (uint16_t i = 0; i < count; i++) {
        const u8* n = nodes + i * 0x10;
        SarcFile f;
        f.hash = RB32(n);
        f.attr = RB32(n + 4);
        const uint32_t b = RB32(n + 8), e = RB32(n + 12);
        if (f.attr & 0x01000000) f.name = (const char*)(names + (f.attr & 0xFFFF) * 4);
        if (data_off + e > d.size() || b > e) return false;
        f.data.assign(d.begin() + data_off + b, d.begin() + data_off + e);
        files.push_back(std::move(f));
    }
    return true;
}

Bytes sarc_write(const std::vector<SarcFile>& files, uint32_t hash_key, uint32_t alignment) {
    Bytes names;
    std::vector<uint32_t> name_offs;
    for (auto& f : files) {
        name_offs.push_back((uint32_t)names.size());
        names.insert(names.end(), f.name.begin(), f.name.end());
        names.push_back(0);
        while (names.size() % 4) names.push_back(0);
    }
    const uint32_t header = 0x14 + 0x0C + (uint32_t)files.size() * 0x10 + 8 + (uint32_t)names.size();
    const uint32_t data_off = (header + alignment - 1) & ~(alignment - 1);
    Bytes out(data_off, 0);
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (auto& f : files) {
        size_t pos = (out.size() + alignment - 1) & ~(size_t)(alignment - 1);
        out.resize(pos, 0);
        ranges.emplace_back((uint32_t)(pos - data_off), (uint32_t)(pos - data_off + f.data.size()));
        out.insert(out.end(), f.data.begin(), f.data.end());
    }
    u8* p = out.data();
    memcpy(p, "SARC", 4);
    WB16(p + 4, 0x14);
    WB16(p + 6, 0xFEFF);
    WB32(p + 8, (uint32_t)out.size());
    WB32(p + 0x0C, data_off);
    WB16(p + 0x10, 0x0100);
    u8* sfat = p + 0x14;
    memcpy(sfat, "SFAT", 4);
    WB16(sfat + 4, 0x0C);
    WB16(sfat + 6, (uint16_t)files.size());
    WB32(sfat + 8, hash_key);
    for (size_t i = 0; i < files.size(); i++) {
        u8* n = sfat + 0x0C + i * 0x10;
        WB32(n, files[i].hash);
        WB32(n + 4, files[i].name.empty() ? 0 : (0x01000000 | (name_offs[i] / 4)));
        WB32(n + 8, ranges[i].first);
        WB32(n + 12, ranges[i].second);
    }
    u8* sfnt = sfat + 0x0C + files.size() * 0x10;
    memcpy(sfnt, "SFNT", 4);
    WB16(sfnt + 4, 8);
    memcpy(sfnt + 8, names.data(), names.size());
    return out;
}

// ---- CRC32 (RSTB keys, cache stamp) ---------------------------------------------------------------------
uint32_t crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const u8* p = (const u8*)data;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
uint32_t crc32(std::string_view s) { return crc32(s.data(), s.size()); }

// ---- prompt font ----------------------------------------------------------------------------------------
void parse_template_cmap() {
    g_cmap.clear();
    const Bytes& d = g_template;
    if (d.size() < 0x34 || memcmp(d.data(), "FFNT", 4) != 0) return;
    uint32_t cmap = RB32(d.data() + 0x14 + 0x1C);  // FINF ptrCMAP (points at block data)
    for (int guard = 0; cmap && cmap + 12 < d.size() && guard < 64; guard++) {
        const u8* c = d.data() + cmap;
        const uint16_t lo = RB16(c), hi = RB16(c + 2), method = RB16(c + 4);
        if (method == 1)
            for (uint32_t code = lo; code <= hi && c + 12 + (code - lo) * 2 + 2 <= d.data() + d.size(); code++) {
                const uint16_t g = RB16(c + 12 + (code - lo) * 2);
                if (g != 0xFFFF) g_cmap[(uint16_t)code] = g;
            }
        cmap = RB32(c + 8);
    }
}

// the font's CMAP table for U+E040..U+E08F (game button codes)
u8* find_base_table(Bytes& d) {
    uint32_t cmap = RB32(d.data() + 0x14 + 0x1C);
    for (int guard = 0; cmap && cmap + 12 < d.size() && guard < 64; guard++) {
        u8* c = d.data() + cmap;
        if (RB16(c) == 0xE040 && RB16(c + 4) == 1) return c + 12;
        cmap = RB32(c + 8);
    }
    return nullptr;
}

// composite glyphs of the template
constexpr uint16_t kGlyphWASD = 0xE0F0, kGlyphAD = 0xE0F1, kGlyphMouseMove = 0xE0F3, kGlyphMouseX = 0xE0F4,
                   kGlyphMouseY = 0xE0F5, kGlyphArrowKeys = 0xE0F6, kGlyphWheelUpDown = 0xE0F7,
                   kGlyphOneToFour = 0xE0F8, kGlyphMouseLeft = 0xE0F9, kGlyphMouseRight = 0xE0FA,
                   kGlyphMouseUp = 0xE0FB, kGlyphMouseDown = 0xE0FC;

// Windows virtual-key code of a binding (the template's glyphs are at U+E100 + VK), 0 if none.
int vk_of(const pccontrols::BoundKey& k) {
    switch (k.type) {
        case 2:
            switch (k.code) {
                case SDL_BUTTON_LEFT: return 0x01;
                case SDL_BUTTON_RIGHT: return 0x02;
                case SDL_BUTTON_MIDDLE: return 0x04;
                case SDL_BUTTON_X1: return 0x05;
                case SDL_BUTTON_X2: return 0x06;
            }
            return 0;
        case 3: return 0x0E;  // wheel up
        case 4: return 0x0F;  // wheel down
        case 1: break;
        default: return 0;
    }
    const int sc = k.code;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    static const std::pair<int, int> kMap[] = {
        {SDL_SCANCODE_BACKSPACE, 0x08}, {SDL_SCANCODE_TAB, 0x09},       {SDL_SCANCODE_RETURN, 0x0D},
        {SDL_SCANCODE_ESCAPE, 0x1B},    {SDL_SCANCODE_SPACE, 0x20},     {SDL_SCANCODE_PAGEUP, 0x21},
        {SDL_SCANCODE_PAGEDOWN, 0x22},  {SDL_SCANCODE_END, 0x23},       {SDL_SCANCODE_HOME, 0x24},
        {SDL_SCANCODE_LEFT, 0x25},      {SDL_SCANCODE_UP, 0x26},        {SDL_SCANCODE_RIGHT, 0x27},
        {SDL_SCANCODE_DOWN, 0x28},      {SDL_SCANCODE_INSERT, 0x2D},    {SDL_SCANCODE_DELETE, 0x2E},
        {SDL_SCANCODE_LSHIFT, 0xA0},    {SDL_SCANCODE_RSHIFT, 0xA1},    {SDL_SCANCODE_LCTRL, 0xA2},
        {SDL_SCANCODE_RCTRL, 0xA3},     {SDL_SCANCODE_LALT, 0xA4},      {SDL_SCANCODE_RALT, 0xA5},
        {SDL_SCANCODE_SEMICOLON, 0xBA}, {SDL_SCANCODE_EQUALS, 0xBB},    {SDL_SCANCODE_COMMA, 0xBC},
        {SDL_SCANCODE_MINUS, 0xBD},     {SDL_SCANCODE_PERIOD, 0xBE},    {SDL_SCANCODE_SLASH, 0xBF},
        {SDL_SCANCODE_GRAVE, 0xC0},     {SDL_SCANCODE_LEFTBRACKET, 0xDB}, {SDL_SCANCODE_BACKSLASH, 0xDC},
        {SDL_SCANCODE_RIGHTBRACKET, 0xDD}, {SDL_SCANCODE_APOSTROPHE, 0xDE},
    };
    for (auto& [s, v] : kMap)
        if (s == sc) return v;
    return 0;
}

uint16_t key_glyph(int vk) {
    if (!vk) return 0;
    const uint16_t code = (uint16_t)(0xE100 + vk);
    return g_cmap.count(code) ? code : 0;
}

uint16_t action_glyph(const char* action) {
    for (int slot = 0; slot < 2; slot++)
        if (uint16_t g = key_glyph(vk_of(pccontrols::bound_key(action, slot)))) return g;
    return 0;
}

bool first_key_is(const char* action, SDL_Scancode sc) {
    pccontrols::BoundKey k = pccontrols::bound_key(action, 0);
    return k.type == 1 && k.code == (int)sc;
}

// The PC glyph for one of the game's button codes (U+E040..), by its gameplay meaning (Zelda-on-PC's table).
uint16_t glyph_for(uint16_t code) {
    auto key = [&](const char* action) {
        uint16_t g = action_glyph(action);
        return g ? g : code;
    };
    const bool wasd = first_key_is("MoveForward", SDL_SCANCODE_W) && first_key_is("MoveLeft", SDL_SCANCODE_A) &&
                      first_key_is("MoveBack", SDL_SCANCODE_S) && first_key_is("MoveRight", SDL_SCANCODE_D);
    auto back = [&] {
        uint16_t g = action_glyph("Back");
        return g ? g : code;
    };
    switch (code) {
        case 0xE040: return key("Interact");   // A
        case 0xE041: return key("Sprint");     // B
        case 0xE042: return key("Jump");       // X
        case 0xE043: return key("Attack");     // Y
        case 0xE044: return key("UseRune");    // L
        case 0xE045: return key("Throw");      // R
        case 0xE046: return key("Target");     // ZL
        case 0xE047: return key("Bow");        // ZR
        case 0xE048: return back();            // (Zelda-on-PC: Escape, its menu key; here Back has Escape)
        case 0xE049: return kGlyphOneToFour;
        case 0xE04A: return back();
        case 0xE04B: return key("Inventory");  // +
        case 0xE04C: return key("Map");        // -
        case 0xE050: return key("MoveBack");
        case 0xE051: return kGlyphMouseDown;
        case 0xE052: return key("MoveForward");
        case 0xE053: return kGlyphMouseUp;
        case 0xE054: return wasd ? kGlyphAD : key("MoveRight");
        case 0xE055: return kGlyphMouseX;
        case 0xE056: return key("Crouch");     // left stick click
        case 0xE057: return key("Scope");      // right stick click
        case 0xE058: return key("MoveRight");
        case 0xE059: return kGlyphMouseRight;
        case 0xE05A: return key("MoveLeft");
        case 0xE05B: return kGlyphMouseLeft;
        case 0xE05D: return kGlyphMouseY;
        case 0xE05E: return wasd ? kGlyphWASD : key("MoveForward");
        case 0xE05F: return kGlyphMouseMove;
        case 0xE060: return key("RuneSelect");
        case 0xE061: return key("Whistle");
        case 0xE062: return key("ShieldSelect");
        case 0xE063: return key("WeaponSelect");
        case 0xE064: return kGlyphWheelUpDown;
        case 0xE065: return kGlyphArrowKeys;
        case 0xE066: return key("TiltMode");
        default: return code;
    }
}

std::string binding_stamp() {
    std::ostringstream o;
    for (uint16_t c = 0xE040; c <= 0xE067; c++) o << std::hex << glyph_for(c) << ",";
    return o.str();
}

// The file the game would get for /vol/content/<rel>: a mod's, else the game's own.
std::string source_of(const std::string& game_root, const std::string& rel) {
    mods::Query q = mods::query("/vol/content/" + rel);
    if (q.kind == mods::Query::Kind::File) return q.host;
    std::string p = game_root + "/content/" + rel;
    return fs::exists(botw::path_from_utf8(p)) ? p : "";
}

std::string file_stamp(const std::string& path) {
    std::error_code ec;
    const fs::path p = botw::path_from_utf8(path);
    auto size = fs::file_size(p, ec);
    if (ec) return "";
    auto t = fs::last_write_time(p, ec).time_since_epoch().count();
    std::ostringstream o;
    o << path << "#" << size << "#" << t;
    return o.str();
}
}  // namespace

bool active() { return g_active; }

void prepare(const std::string& game_root, const std::string& user_root) {
    g_active = false;
    if (!pccontrols::active() || user_root.empty() || game_root.empty()) return;
    if (const char* e = getenv("BOTW_PCE_NO_PROMPTS"); e && *e && *e != '0') {
        LOG("pc prompts: off (BOTW_PCE_NO_PROMPTS); GamePad prompts");
        return;
    }
    std::string font_name;
    std::string font_src;
    for (const char* n : {"Font_EU", "Font_US", "Font_JP"}) {
        font_src = source_of(game_root, std::string("Font/") + n + ".sbfarc");
        if (!font_src.empty()) {
            font_name = n;
            break;
        }
    }
    const std::string rstb_rel = "System/Resource/ResourceSizeTable.product.srsizetable";
    const std::string rstb_src = source_of(game_root, rstb_rel);
    const std::string font_rel = "Font/" + font_name + ".sbfarc";
    if (font_name.empty() || rstb_src.empty()) {
        LOG("pc prompts: game font or resource size table not found; keeping GamePad prompts");
        return;
    }

    const char* base = SDL_GetBasePath();
    const std::string tmpl = std::string(base ? base : "") + "resources/pcedition/External_00.bffnt";
    if (!read_file(tmpl, g_template)) {
        LOG("pc prompts: %s missing; keeping GamePad prompts", tmpl.c_str());
        return;
    }
    parse_template_cmap();

    const fs::path out_dir = botw::path_from_utf8(user_root) / "pc_prompts";
    const fs::path font_out = out_dir / "content" / "Font" / (font_name + ".sbfarc");
    const fs::path rstb_out = out_dir / "content" / "System" / "Resource" / "ResourceSizeTable.product.srsizetable";
    const fs::path stamp_path = out_dir / "prompts.stamp";
    auto install = [&] {
        mods::set_private_override(font_rel, botw::path_to_utf8(font_out));
        mods::set_private_override(rstb_rel, botw::path_to_utf8(rstb_out));
        g_active = true;
    };

    // cache: rebuild only when the bindings, the template or the source files changed
    std::ostringstream st;
    st << "v1|" << g_template.size() << "|" << crc32(g_template.data(), g_template.size()) << "|" << file_stamp(font_src)
       << "|" << file_stamp(rstb_src) << "|" << binding_stamp();
    const std::string stamp = st.str();
    {
        Bytes old;
        std::error_code ec;
        if (read_file(botw::path_to_utf8(stamp_path), old) && std::string(old.begin(), old.end()) == stamp &&
            fs::exists(font_out, ec) && fs::exists(rstb_out, ec)) {
            install();
            LOG("pc prompts: keyboard & mouse prompts (cached)");
            return;
        }
    }

    Bytes raw, sarc, rstb_raw, rstb;
    std::vector<SarcFile> files;
    uint32_t hash_key = 0x65;
    if (!read_file(font_src, raw) || !yaz0_decode(raw, sarc) || !sarc_read(sarc, files, hash_key) ||
        !read_file(rstb_src, rstb_raw) || !yaz0_decode(rstb_raw, rstb) || rstb.size() < 12 ||
        memcmp(rstb.data(), "RSTB", 4) != 0) {
        LOG("pc prompts: could not read %s / %s", font_src.c_str(), rstb_src.c_str());
        return;
    }
    const uint32_t yaz_align = RB32(raw.data() + 8);

    // prompt font with the button codes pointed at the bound keys
    Bytes font = g_template;
    if (u8* table = find_base_table(font)) {
        for (uint16_t code = 0xE040; code <= 0xE08F; code++) {
            const uint16_t target = glyph_for(code);
            if (target == code) continue;  // arrows etc. keep the template's own glyphs
            auto it = g_cmap.find(target);
            if (it != g_cmap.end()) WB16(table + (code - 0xE040) * 2, it->second);
        }
    }
    bool replaced = false;
    for (auto& f : files)
        if (f.name == "External_00.bffnt") f.data = font, replaced = true;
    if (!replaced) {
        LOG("pc prompts: External_00.bffnt not found in %s", font_src.c_str());
        return;
    }
    const Bytes new_sarc = sarc_write(files, hash_key, 0x2000);

    // resource size table: keep the game's overhead for this resource, grow it by our size difference
    const uint32_t crc_count = RB32(rstb.data() + 4);
    const uint32_t key = crc32("Font/" + font_name + ".bfarc");
    bool rstb_ok = false;
    for (uint32_t i = 0; i < crc_count && 12 + (size_t)(i + 1) * 8 <= rstb.size(); i++) {
        u8* e = rstb.data() + 12 + i * 8;
        if (RB32(e) == key) {
            const int64_t overhead = (int64_t)RB32(e + 4) - (int64_t)sarc.size();
            const uint32_t need = (uint32_t)((((int64_t)new_sarc.size() + std::max<int64_t>(overhead, 0x2008)) + 31) & ~31ll);
            if (need > RB32(e + 4)) WB32(e + 4, need);
            rstb_ok = true;
            break;
        }
    }
    if (!rstb_ok) LOG("pc prompts: %s.bfarc has no RSTB entry (left unchanged)", font_name.c_str());

    if (!write_file_atomic(font_out, yaz0_store(new_sarc, yaz_align)) ||
        !write_file_atomic(rstb_out, yaz0_store(rstb, RB32(rstb_raw.data() + 8)))) {
        LOG("pc prompts: writing %s failed", botw::path_to_utf8(out_dir).c_str());
        return;
    }
    write_file_atomic(stamp_path, Bytes(stamp.begin(), stamp.end()));
    install();
    LOG("pc prompts: installed keyboard & mouse prompts (%llu -> %llu bytes, %s)", (unsigned long long)sarc.size(),
        (unsigned long long)new_sarc.size(), botw::path_to_utf8(out_dir).c_str());
}
}  // namespace pcprompts
