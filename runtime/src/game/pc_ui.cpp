// UI state for the PC controls. Port of Zelda-on-PC's [BotWPCE_UI] patches (src/pcedition/PCGraphics.cpp) and of
// its UI model (src/pcedition/PCUI.cpp, PCInput.cpp HLE_InvJump / HLE_SlideSnap), MPL-2.0,
// https://github.com/mircea32000/Zelda-on-PC. This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Hooks (U-King.rpx 1.5.0; Zelda-on-PC patch address -> hook here):
//   0x03C4BA98 nw::lyt::TextBox::SetString epilogue -> 0x03C4BA94 (the store before it; r29 = text box)
//   0x03A3EDC8 ui screen layout load (r30 = screen, r24 = layout name)
//   0x02F033E0 world map screen update (r27 = map screen)
//   0x03A2E8E4 menu box cursor manager update (r31 = manager), only while the cursor takes input
//   0x02E816E8 list widget of the shop style screens (r30 = widget, r31 = its current cursor entry)
//   0x02FF64CC bl ease(carousel offset) -> 0x02FF64C8 (r4 = &offset, f1 = target): page change applied at once
//   0x02FF6768 / 0x02FF67EC bl carousel update -> 0x02FF6764 / 0x02FF67E8 (r3 = inventory screen): page jump
// HUD action ring: like Zelda-on-PC's RefreshDoCommand, the button pictures are hidden and the overlay draws keys.
// Not ported: minimap, tips pictures, region language, glide tick.
#include "pc_ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../input/input.h"
#include "../input/pc_controls.h"
#include "../input/pc_prompts.h"
#include "../log.h"
#include "hooks.h"

extern "C" void rt_call(PPCContext* c, uint32_t target);

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<int64_t> g_map_ms{-1000000};
}  // namespace

// The world map is on screen (its update ran in the last 150 ms): the wheel zooms it.
bool pce_map_screen() { return now_ms() - g_map_ms.load() < 150; }

namespace pceui {
namespace {
// ---- guest memory ---------------------------------------------------------------------------------------
// Pointers read from layouts can be stale; every read is checked. mem::readable is a system call on Windows, the
// walks make thousands of checks a frame: 4 KB pages found readable are remembered (guest memory stays committed).
std::vector<std::atomic<uint32_t>> g_page_ok(1u << 15);  // 2^20 pages, one bit each
bool page_ok(uint32_t page) {
    std::atomic<uint32_t>& w = g_page_ok[page >> 5];
    const uint32_t bit = 1u << (page & 31);
    if (w.load(std::memory_order_relaxed) & bit) return true;
    if (!mem::readable(page << 12, 4096)) return false;
    w.fetch_or(bit, std::memory_order_relaxed);
    return true;
}
bool ok(uint32_t a, uint32_t len = 0x100) {
    if (a < 0x10000000u || a >= 0xF0000000u) return false;
    const uint64_t end = (uint64_t)a + len;
    for (uint64_t p = a >> 12; p <= (end - 1) >> 12; p++)
        if (!page_ok((uint32_t)p)) return false;
    return true;
}
uint8_t r8(uint32_t a) { return mem::r8(a); }
uint16_t r16(uint32_t a) { return mem::r16(a); }
uint32_t r32(uint32_t a) { return mem::r32(a); }
float rf(uint32_t a) { return mem::rf32(a); }

// ---- nw::lyt::Pane layout (U-King v1.5.0) ----
constexpr uint32_t kNext = 0x00;       // sibling list node
constexpr uint32_t kParent = 0x0C;
constexpr uint32_t kChildHead = 0x14;  // child list head node (next / prev)
constexpr uint32_t kTranslate = 0x1C;  // vec3
constexpr uint32_t kScale = 0x34;      // vec2
constexpr uint32_t kSize = 0x3C;       // vec2
constexpr uint32_t kFlag = 0x44;       // u8 flags (bit0 visible), u8 alpha, u8 global alpha, u8 base position
constexpr uint32_t kName = 0x80;       // char[24]
constexpr uint32_t kTextBuf = 0xA4;    // TextBox: char16* buffer
constexpr uint32_t kTextLen = 0xCE;    // TextBox: u16 length
constexpr float kLayoutW = 1280.0f, kLayoutH = 720.0f;

bool pane_ok(uint32_t pane) { return ok(pane, 0x9C); }

std::string read_name(uint32_t pane) {
    char buf[25]{};
    for (int i = 0; i < 24; i++) {
        const char ch = (char)r8(pane + kName + i);
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7E) return {};
        buf[i] = ch;
    }
    return buf;
}
bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::string utf16_to_utf8(const std::u16string& s) {
    std::string o;
    for (char16_t ch : s) {
        if (ch < 0x80) o.push_back((char)ch);
        else if (ch < 0x800) o.push_back((char)(0xC0 | (ch >> 6))), o.push_back((char)(0x80 | (ch & 0x3F)));
        else {
            o.push_back((char)(0xE0 | (ch >> 12)));
            o.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (ch & 0x3F)));
        }
    }
    return o;
}

// ---- reported texts and screens ----
struct TextInfo {
    std::string name;
    std::string text;  // UTF-8, inline tags stripped
};
std::mutex g_mutex;
std::unordered_map<uint32_t, TextInfo> g_texts;
std::unordered_map<std::string, uint32_t> g_screens;
std::unordered_map<uint32_t, uint32_t> g_screen_sig;  // screen object -> vtable at registration (memory is reused)
std::atomic<uint32_t> g_frame{0};

// prompts of the menus: text boxes with button codes (U+E040..U+E08F) and what they were, re-targeted per frame
struct PromptBox {
    std::string name;
    uint32_t buf = 0;
    std::vector<std::pair<uint32_t, uint16_t>> codes;  // index in the buffer, original code
    std::vector<uint16_t> written;                      // the glyph last written at each (0: none yet)
};
std::unordered_map<uint32_t, PromptBox> g_prompt_boxes;

void on_text_prompts(uint32_t pane, const std::string& name, uint32_t buf, uint32_t len) {
    std::vector<std::pair<uint32_t, uint16_t>> codes;
    for (uint32_t i = 0; i < len; i++) {
        const uint16_t ch = r16(buf + i * 2);
        if (ch == 0) break;
        if (ch == 0x0E) {
            if (i + 3 < len) i += 3 + (r16(buf + (i + 3) * 2) + 1) / 2;
            continue;
        }
        if (ch >= 0xE040 && ch <= 0xE08F) codes.emplace_back(i, ch);
    }
    if (codes.empty()) {
        g_prompt_boxes.erase(pane);
        return;
    }
    if (g_prompt_boxes.size() > 4000) g_prompt_boxes.clear();
    PromptBox& box = g_prompt_boxes[pane];
    box.name = name;
    box.buf = buf;
    box.written.assign(codes.size(), 0);
    box.codes = std::move(codes);
}

// ---- menu box cursor (U-King v1.5.0) ----
// One manager (it belongs to the BoxCursorTV screen) owns the selection box of all menus. Its update takes a
// requested entry (+0x28), runs the game's own "can be selected" test, focuses it and clears the request: the entry
// under the mouse is requested and the box is on it in the next frame.
//   manager  +0x0C control ([+0x4C] = BoxCursorTV screen), +0x24 focused entry, +0x28 requested entry
//   screen   +0x14 screen list ([+0x14] count, [+0x18] screens, [+0x20] one byte per slot, bit 7 = free)
//            +0x1C control state ([+0x20] bit 1 = takes input), +0x50 entry list (first node +0x54, node offset
//            +0x5C), +0x95 / +0x96 state (shown: +0x96 == 2 and +0x95 >= 0)
//   entry    +0x04 next node, +0x08 owner control, +0x0C screen
//   owner    +0x1E flags (0x10 = can be selected), +0x38 pane (global matrix: translation at +0x54 / +0x64)
// Screens that fill the request slot themselves every frame keep their own row: shop lists are told the wanted
// index (list widget), the others get d-pad steps.
struct FocusRequest {
    uint32_t part = 0;
    Rect area;
    uint32_t frame = 0xF0000000;
    int result = 0;  // 0 not looked at, 1 requested, 2 no selectable entry, 3 already focused, 4 screen has its own row
};
std::mutex g_focus_mutex;
FocusRequest g_focus;
std::atomic<uint32_t> g_cursor_mgr{0}, g_cursor_frame{0xF0000000}, g_cursor_owned_frame{0xF0000000};
std::atomic<uint32_t> g_list_frame{0xF0000000};

int levels_below(uint32_t pane, uint32_t part) {
    for (int up = 0; up < 4 && pane_ok(pane); up++) {
        if (pane == part) return up;
        pane = r32(pane + kParent);
    }
    return -1;
}

uint32_t cursor_focus_pane() {
    const uint32_t mgr = g_cursor_mgr.load();
    if (g_frame.load() - g_cursor_frame.load() > 3 || !ok(mgr, 0x30)) return 0;
    const uint32_t focus = r32(mgr + 0x24);
    if (!ok(focus, 0x30)) return 0;
    const uint32_t owner = r32(focus + 0x08);
    if (!ok(owner, 0x3C)) return 0;
    const uint32_t pane = r32(owner + 0x38);
    return pane_ok(pane) ? pane : 0;
}

uint32_t find_cursor_entry(uint32_t mgr, uint32_t part, const Rect& area) {
    const uint32_t control = r32(mgr + 0x0C);
    if (!ok(control, 0x50)) return 0;
    const uint32_t box_screen = r32(control + 0x4C);
    if (!ok(box_screen, 0x18)) return 0;
    const uint32_t list = r32(box_screen + 0x14);
    if (!ok(list, 0x24)) return 0;
    const uint32_t count = std::min<uint32_t>(r32(list + 0x14), 256);
    const uint32_t screens = r32(list + 0x18), free_flags = r32(list + 0x20);
    if (!count || !ok(screens, count * 4) || !ok(free_flags, count)) return 0;
    uint32_t best = 0, best_own = 0;
    float best_dist = 1e9f;
    int best_levels = 99;
    for (uint32_t i = 0; i < count; i++) {
        if (r8(free_flags + i) & 0x80) continue;
        const uint32_t screen = r32(screens + i * 4);
        if (!ok(screen, 0xA0) || (int8_t)r8(screen + 0x96) != 2 || (int8_t)r8(screen + 0x95) < 0) continue;
        const uint32_t st = r32(screen + 0x1C);
        if (!ok(st, 0x24) || !(r32(st + 0x20) & 2)) continue;
        const uint32_t offset = r32(screen + 0x5C), end = screen + 0x50;
        uint32_t node = r32(screen + 0x54);
        for (int n = 0; n < 512 && node != end && ok(node, 8); n++, node = r32(node + 4)) {
            const uint32_t entry = node - offset;
            if (!ok(entry, 0x30)) break;
            const uint32_t owner = r32(entry + 0x08);
            if (!ok(owner, 0x3C) || !(r16(owner + 0x1E) & 0x10)) continue;
            const uint32_t pane = r32(owner + 0x38);
            if (!pane_ok(pane)) continue;
            const int levels = part ? levels_below(pane, part) : -1;
            if (levels >= 0 && levels < best_levels) best_levels = levels, best_own = entry;
            const float x = 0.5f + rf(pane + 0x54) / kLayoutW, y = 0.5f - rf(pane + 0x64) / kLayoutH;
            if (!area.contains(x, y)) continue;
            const float dx = x - area.cx(), dy = y - area.cy();
            if (dx * dx + dy * dy < best_dist) best_dist = dx * dx + dy * dy, best = entry;
        }
    }
    return best_own ? best_own : best;
}

// ---- list widget of the shop style screens (update 0x02E810CC) ----
//   widget    +0x21C / +0x224 categories (count / pointers), +0x298 current index, +0x29C wanted index
//   category  +0x04 pages {+0x00 count, +0x08 pointers}, +0x230 entries, +0x234 * +0x238 entries per page
//   page      +0x04 category, +0x0C / +0x14 slots (count / pointers), +0x1C state (+0x0C page number, < 0 unused)
//   slot      +0x08 page, +0x10 cursor entry, +0x14 flags (2 = in use), +0x20 position on its page
int32_t list_index_of(uint32_t widget, uint32_t entry) {
    const uint32_t cat_count = std::min<uint32_t>(r32(widget + 0x21C), 32), cats = r32(widget + 0x224);
    if (!cat_count || !ok(cats, cat_count * 4)) return -1;
    uint32_t found = 0;
    for (uint32_t ci = 0; ci < cat_count && !found; ci++) {
        const uint32_t cat = r32(cats + ci * 4);
        const uint32_t pages = ok(cat, 0x280) ? r32(cat + 0x04) : 0;
        const uint32_t page_count = ok(pages, 0x0C) ? std::min<uint32_t>(r32(pages), 16) : 0;
        const uint32_t page_arr = page_count ? r32(pages + 0x08) : 0;
        for (uint32_t p = 0; p < page_count && ok(page_arr, page_count * 4) && !found; p++) {
            const uint32_t page = r32(page_arr + p * 4);
            if (!ok(page, 0x20)) continue;
            const uint32_t slot_count = std::min<uint32_t>(r32(page + 0x0C), 64), slots = r32(page + 0x14);
            for (uint32_t k = 0; k < slot_count && ok(slots, slot_count * 4); k++) {
                const uint32_t slot = r32(slots + k * 4);
                if (ok(slot, 0x30) && (r32(slot + 0x14) & 2) && r32(slot + 0x10) == entry) {
                    found = slot;
                    break;
                }
            }
        }
    }
    if (!found) return -1;
    const uint32_t page = r32(found + 0x08);
    if (!ok(page, 0x20)) return -1;
    const uint32_t cat = r32(page + 0x04), st = r32(page + 0x1C);
    if (!ok(cat, 0x280) || !ok(st, 0x10) || (int32_t)r32(st + 0x0C) < 0) return -1;
    int32_t index = (int32_t)(r32(st + 0x0C) * r32(cat + 0x234) * r32(cat + 0x238) + r32(found + 0x20));
    for (uint32_t ci = 0; ci < cat_count; ci++) {
        const uint32_t other = r32(cats + ci * 4);
        if (other == cat) return index;
        if (ok(other, 0x280)) index += (int32_t)r32(other + 0x230);
    }
    return -1;
}

// ---- screens ----
std::unordered_map<uint32_t, bool> g_root_shown;  // root pane -> its screen is shown on the TV
std::unordered_map<uint32_t, std::string> g_root_screen;

// The state of a screen (+0x94): 0x100 opening, 0x200 open, 0x300 closing, 0 closed.
bool screen_shown_obj(const std::string& name, uint32_t obj) {
    if (!ok(obj, 0x200)) return false;
    if (name.find("DRC") != std::string::npos || name == "GamePadBG_00" || name == "SeekPadMenuBG_00") return false;
    if (auto sg = g_screen_sig.find(obj); sg != g_screen_sig.end() && r32(obj + 0x0C) != sg->second) return false;
    return (r32(obj + 0x94) & 0x200) != 0;
}

bool screen_visible(const std::string& name) {
    auto it = g_screens.find(name);
    if (it == g_screens.end() || !ok(it->second, 0x200)) return false;
    if (auto sg = g_screen_sig.find(it->second); sg != g_screen_sig.end() && r32(it->second + 0x0C) != sg->second)
        return false;
    return (r32(it->second + 0x94) & 0x200) != 0;
}

void rebuild_roots() {
    g_root_shown.clear();
    g_root_screen.clear();
    bool map_on_tv = false;
    if (auto it = g_screens.find("AppHome_00"); it != g_screens.end()) map_on_tv = screen_shown_obj(it->first, it->second);
    for (auto& [name, obj] : g_screens) {
        if (!ok(obj, 0x40)) continue;
        const uint32_t lyt = r32(obj + 0x18);
        if (!ok(lyt, 0x10)) continue;
        const uint32_t root = r32(lyt + 0x0C);
        bool shown = screen_shown_obj(name, obj);
        if (name == "AppMap_00") shown = shown && map_on_tv && !screen_visible("AppMapDungeon_00");
        g_root_shown[root] = shown;
        g_root_screen[root] = name;
    }
}

// this pane and all ancestors visible with non-zero alpha, and its screen shown
bool visible(uint32_t pane, std::string* screen_out = nullptr) {
    uint32_t last = pane;
    for (int depth = 0; depth < 32 && pane; depth++) {
        if (!pane_ok(pane)) return false;
        if (!(r8(pane + kFlag) & 1) || r8(pane + kFlag + 1) == 0) return false;
        last = pane;
        pane = r32(pane + kParent);
    }
    auto it = g_root_shown.find(last);
    if (it == g_root_shown.end()) return false;
    if (screen_out) *screen_out = g_root_screen[last];
    return it->second;
}

std::string screen_of(uint32_t pane) {
    uint32_t last = pane;
    for (int depth = 0; depth < 40 && pane; depth++) {
        if (!pane_ok(pane)) break;
        last = pane;
        pane = r32(pane + kParent);
    }
    auto it = g_root_screen.find(last);
    return it != g_root_screen.end() ? it->second : std::string();
}

bool has_ancestor(uint32_t pane, std::initializer_list<const char*> prefixes) {
    for (int depth = 0; depth < 40 && pane_ok(pane); depth++) {
        const std::string n = read_name(pane);
        for (const char* p : prefixes)
            if (starts(n, p)) return true;
        pane = r32(pane + kParent);
    }
    return false;
}

bool is_menu_screen(const std::string& s) {
    return starts(s, "Pause") || starts(s, "App") || starts(s, "SystemWindow") || starts(s, "Title") ||
           starts(s, "GameOver") || starts(s, "Shop") || starts(s, "Option") || starts(s, "End_");
}

// ---- menu prompts: in the game's menus the confirm prompt is a left click, Back the Esc key, the inventory's
// category hint and the map's zoom the wheel; everywhere else the code stays (the font shows the bound key)
uint16_t prompt_glyph(uint32_t pane, uint16_t code, const std::string& screen) {
    using pcprompts::MenuGlyph;
    if (starts(screen, "PauseMenu") && screen != "PauseMenuInfo_00") {
        if (has_ancestor(pane, {"N_PauseTips", "Pa_AllTips"})) return code;  // the controls help: gameplay keys
        if (has_ancestor(pane, {"Pa_Rstick", "N_RGuide"}))
            if (uint16_t g = pcprompts::menu_glyph(MenuGlyph::Wheel)) return g;
    }
    if (!is_menu_screen(screen)) return code;
    uint16_t g = 0;
    switch (code) {
        case 0xE040: g = pcprompts::menu_glyph(MenuGlyph::Click); break;
        case 0xE041: g = pcprompts::menu_glyph(MenuGlyph::Back); break;
        case 0xE043: g = pcprompts::menu_glyph(MenuGlyph::Sort); break;  // Y (sort): the Crouch key
        case 0xE05F:
            if (starts(screen, "AppHome") || starts(screen, "AppMap")) g = pcprompts::menu_glyph(MenuGlyph::Wheel);
            break;
        default: break;
    }
    return g ? g : code;
}

// false: the text box is not the one we know any more (freed layouts are reused by other objects: write only
// while name, buffer and the codes we know are still there)
bool apply_prompts(uint32_t pane, PromptBox& box) {
    if (!pane_ok(pane) || r32(pane + kTextBuf) != box.buf || read_name(pane) != box.name) return false;
    const uint32_t len = r16(pane + kTextLen);
    for (size_t i = 0; i < box.codes.size(); i++) {
        const uint32_t idx = box.codes[i].first;
        if (idx >= len || !ok(box.buf + idx * 2, 2)) return false;
        const uint16_t cur = r16(box.buf + idx * 2);
        if (cur != box.codes[i].second && cur != box.written[i]) return false;
    }
    const std::string screen = screen_of(pane);
    bool changed = false;
    for (size_t i = 0; i < box.codes.size(); i++) {
        const auto [idx, code] = box.codes[i];
        const uint16_t g = prompt_glyph(pane, code, screen);
        if (r16(box.buf + idx * 2) != g) {
            mem::w16(box.buf + idx * 2, g);
            box.written[i] = g;
            changed = true;
        }
    }
    if (changed) mem::w8(pane + 0xD0, r8(pane + 0xD0) | 0x20);  // text box: rebuild its glyphs
    return true;
}

void refresh_prompts() {
    if (g_prompt_boxes.empty() || !pcprompts::active()) return;
    for (auto it = g_prompt_boxes.begin(); it != g_prompt_boxes.end();) {
        if (!pane_ok(it->first) || (visible(it->first) && !apply_prompts(it->first, it->second))) {
            it = g_prompt_boxes.erase(it);
            continue;
        }
        ++it;
    }
}

// ---- HUD action ring (DoCommand_00) ----
std::mutex g_hud_mutex;
std::vector<HudKey> g_hud;

// a screen is drawn while it opens, is open or closes (+0x94 & 0x300)
bool screen_drawn(const std::string& name) {
    auto it = g_screens.find(name);
    if (it == g_screens.end() || !ok(it->second, 0x200)) return false;
    if (auto sg = g_screen_sig.find(it->second); sg != g_screen_sig.end() && r32(it->second + 0x0C) != sg->second)
        return false;
    return (r32(it->second + 0x94) & 0x300) != 0;
}

uint32_t screen_root(const std::string& name) {
    auto it = g_screens.find(name);
    if (it == g_screens.end() || !ok(it->second, 0x40)) return 0;
    const uint32_t lyt = r32(it->second + 0x18);
    if (!ok(lyt, 0x10)) return 0;
    const uint32_t root = r32(lyt + 0x0C);
    return pane_ok(root) ? root : 0;
}

float alpha_chain(uint32_t pane, bool skip_own_visibility) {
    float a = 1.0f;
    for (int depth = 0; depth < 40 && pane_ok(pane); depth++) {
        if (!(r8(pane + kFlag) & 1) && !(skip_own_visibility && depth == 0)) return 0.0f;
        a *= r8(pane + kFlag + 1) / 255.0f;
        pane = r32(pane + kParent);
    }
    return a;
}

uint32_t child_by_prefix(uint32_t parent, const std::string& prefix) {
    if (!pane_ok(parent)) return 0;
    const uint32_t head = parent + kChildHead;
    uint32_t node = r32(head);
    for (int i = 0; i < 64 && node && node != head; i++) {
        if (!pane_ok(node)) break;
        if (starts(read_name(node), prefix.c_str())) return node;
        node = r32(node + kNext);
    }
    return 0;
}

template <class F>
void walk_panes(uint32_t root, int limit, F&& f) {
    std::vector<uint32_t> stack{root};
    for (int guard = 0; !stack.empty() && guard < limit; guard++) {
        const uint32_t pane = stack.back();
        stack.pop_back();
        if (!pane_ok(pane)) continue;
        if (!f(pane)) continue;
        const uint32_t head = pane + kChildHead;
        uint32_t node = r32(head);
        for (int i = 0; i < 512 && node && node != head; i++) {
            if (!pane_ok(node)) break;
            stack.push_back(node);
            node = r32(node + kNext);
        }
    }
}

// how far the picture is faded to black (the fade screens lie over the HUD; the keys fade with it)
float fade_amount() {
    float fade = 0.0f;
    for (auto& [name, obj] : g_screens) {
        if (!starts(name, "Fade") || !screen_drawn(name)) continue;
        if (uint32_t root = screen_root(name))
            walk_panes(root, 400, [&](uint32_t pane) {
                if (!starts(read_name(pane), "P_Black")) return true;
                fade = std::max(fade, alpha_chain(pane, false));
                return false;
            });
    }
    return std::clamp(fade, 0.0f, 1.0f);
}

// screen rectangle of a pane from its accumulated translate / scale (the HUD does not rotate)
bool pane_rect(uint32_t pane, Rect& r) {
    uint32_t chain[32];
    int n = 0;
    for (uint32_t p = pane; p && n < 32; p = r32(p + kParent)) {
        if (!pane_ok(p)) return false;
        chain[n++] = p;
    }
    float px = 0, py = 0, sx = 1, sy = 1;
    for (int i = n - 1; i >= 0; i--) {
        px += sx * rf(chain[i] + kTranslate);
        py += sy * rf(chain[i] + kTranslate + 4);
        sx *= rf(chain[i] + kScale);
        sy *= rf(chain[i] + kScale + 4);
    }
    const float w = rf(pane + kSize) * std::fabs(sx), h = rf(pane + kSize + 4) * std::fabs(sy);
    const uint8_t bp = r8(pane + kFlag + 3);
    const int hpos = bp % 3, vpos = bp / 3;
    const float x0 = hpos == 0 ? px - w * 0.5f : (hpos == 1 ? px : px - w);
    const float y1 = vpos == 0 ? py + h * 0.5f : (vpos == 1 ? py : py + h);
    r = {0.5f + x0 / kLayoutW, 0.5f - y1 / kLayoutH, 0.5f + (x0 + w) / kLayoutW, 0.5f - (y1 - h) / kLayoutH};
    return w > 0 && h > 0 && std::isfinite(r.x0) && std::isfinite(r.y0);
}

// the ring's buttons: picture "Nt_KeyTex<B>_nn" beside label "T_Guide<B>_nn", letters "T_<B>_00"
void refresh_do_command() {
    std::vector<HudKey> out;
    const uint32_t root = pcprompts::active() && screen_drawn("DoCommand_00") ? screen_root("DoCommand_00") : 0;
    if (root) {
        const float fade = fade_amount();
        walk_panes(root, 4000, [&](uint32_t pane) {
            const std::string name = read_name(pane);
            const bool letter = name.size() == 6 && name[0] == 'T' && name[1] == '_' && name[3] == '_' &&
                                std::string("ABXY").find(name[2]) != std::string::npos;
            const bool key_tex = starts(name, "Nt_KeyTex") && name.size() >= 10;
            if (key_tex || letter) {
                const uint8_t f = r8(pane + kFlag);
                if (f & 1) mem::w8(pane + kFlag, f & ~1);  // controller artwork off
            }
            if (key_tex) {
                static const char* kAction[] = {"Interact", "Sprint", "Jump", "Attack", "UseRune"};  // A B X Y L
                const size_t bi = std::string("ABXYL").find(name[9]);
                const uint32_t lp = child_by_prefix(r32(pane + kParent), std::string("T_Guide") + name[9]);
                const float alpha = (lp ? alpha_chain(lp, false) : alpha_chain(pane, true)) * (1.0f - fade);
                Rect r;
                if (alpha > 0.01f && bi != std::string::npos && pane_rect(pane, r)) {
                    std::string label = pccontrols::key_label(kAction[bi]);
                    if (!label.empty()) out.push_back({r, alpha, label});
                }
            }
            return true;
        });
    }
    std::lock_guard<std::mutex> l(g_hud_mutex);
    g_hud = std::move(out);
}

// ---- state ----
State g_state;
uint32_t g_state_frame = 0xFFFFFFFF;

uint32_t sibling(uint32_t pane, const std::string& name) {
    const uint32_t parent = r32(pane + kParent);
    if (!pane_ok(parent)) return 0;
    const uint32_t head = parent + kChildHead;
    uint32_t node = r32(head);
    for (int i = 0; i < 256 && node && node != head; i++) {
        if (!pane_ok(node)) return 0;
        if (read_name(node) == name) return node;
        node = r32(node + kNext);
    }
    return 0;
}

// first character of a UTF-8 string, as a code point (the button pictures are U+E040..)
uint32_t first_code(const std::string& t) {
    const unsigned char* u = (const unsigned char*)t.data();
    if (t.size() >= 3 && (u[0] & 0xF0) == 0xE0) return ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    return t.empty() ? 0 : u[0];
}

uint32_t glyph_button(uint32_t g) {
    switch (g) {
        case 0xE040: return input::BTN_A;
        case 0xE041: return input::BTN_B;
        case 0xE042: return input::BTN_X;
        case 0xE043: return input::BTN_Y;
        case 0xE044: return input::BTN_L;
        case 0xE045: return input::BTN_R;
        case 0xE046: return input::BTN_ZL;
        case 0xE047: return input::BTN_ZR;
        default: return 0;
    }
}

// one shown action guide text box (Zelda-on-PC PCUI.cpp State computation); g_mutex held
void add_guide(State& st, uint32_t pane, const std::string& n, const std::string& text) {
    const bool placeholder = text.size() >= 5 && text.compare(text.size() - 5, 5, "Guide") == 0;  // "YGuide"
    if (starts(n, "T_GuideA_")) {
        if (!placeholder) {
            st.action_a = text;
            st.guides.push_back({text, input::BTN_A});
        }
    } else if (n.size() == 11 && starts(n, "T_Guide") && n[8] == '_' && std::string("BXYL").find(n[7]) != std::string::npos) {
        // the action ring's other buttons (Y: Flurry Rush, Sneakstrike, ...)
        if (!placeholder)
            st.guides.push_back({text, n[7] == 'B' ? input::BTN_B : n[7] == 'X' ? input::BTN_X : n[7] == 'Y' ? input::BTN_Y : input::BTN_L});
    } else if (starts(n, "T_Guide_")) {
        // guide list entry: the button is the picture in the sibling icon text box with the same suffix
        uint32_t b = 0;
        if (uint32_t icon = sibling(pane, "T_Icon_" + n.substr(8))) {
            auto it = g_texts.find(icon);
            if (it != g_texts.end()) b = glyph_button(first_code(it->second.text));
        }
        st.guides.push_back({text, b});
    } else if (starts(n, "T_A_") && text.size() > 1) {
        st.guides.push_back({text, input::BTN_A});
    } else if (starts(n, "T_B_") && text.size() > 1) {
        st.guides.push_back({text, input::BTN_B});
    } else if (starts(n, "T_BX_") && text.size() > 1) {
        st.guides.push_back({text, input::BTN_X});
    }
}

void compute(State& st) {
    rebuild_roots();
    refresh_prompts();
    refresh_do_command();
    st = State{};
    st.valid = true;
    st.inventory = screen_visible("PauseMenu_00");
    st.map = screen_visible("AppHome_00");
    st.title = screen_visible("Title_00");
    st.hud = screen_visible("MainScreen_00");
    st.camera_rune = screen_visible("AppCamera_00");
    if (auto it = g_screens.find("MainScreen_00"); it != g_screens.end() && ok(it->second, 0x200))
        st.any_menu = (r32(it->second + 0x180) & 0x1A) != 0x1A;
    for (auto& [name, obj] : g_screens)
        if (starts(name, "SystemWindow") || starts(name, "GameOver") || starts(name, "ShopBtnList") ||
            name == "AppSystemWindow_00" || name == "OptionWindow_00" || name == "End_00" ||
            name == "ControllerWindow_00" || name == "DLCWindow_00")
            if (screen_shown_obj(name, obj)) st.dialog = true;
    bool buttons = false;
    for (auto& [pane, ti] : g_texts) {
        const std::string& n = ti.name;
        if (n.size() < 3 || n[0] != 'T' || n[1] != '_') continue;
        const bool pin = ti.text == "Place pin";
        const bool guide = starts(n, "T_Guide") || starts(n, "T_A_") || starts(n, "T_B_") || starts(n, "T_BX_");
        if (!pin && !guide && !starts(n, "T_Btn_")) continue;
        if (!pane_ok(pane) || read_name(pane) != n || !visible(pane)) continue;
        if (pin) {
            st.scope = true;
            continue;
        }
        if (guide) {
            add_guide(st, pane, n, ti.text);
            continue;
        }
        if (ti.text.empty()) continue;
        buttons = true;
        // answers of a conversation ("MessageDialog": Pa_Btn_nn parts with a T_Btn_00 label)
        if (!st.choice && starts(screen_of(pane), "Message")) st.choice = true;
    }
    if (st.camera_rune && buttons) {  // the camera asks about the picture ("Save in album?"): a menu then
        st.camera_rune = false;
        st.camera_dialog = true;
    }
}

// ---- pointer: one depth-first walk of the shown layouts with inherited visibility and transform ----
Pointer g_pointer;
uint32_t g_pointer_frame = 0xFFFFFFFF;
std::atomic<uint32_t> g_map_obj{0}, g_map_obj_frame{0};

struct PartInfo {
    bool has_hit = false;
    Rect hit;
    bool has_text = false;
    Rect text;
    std::string label;
    uint8_t label_alpha = 0;
    int order = 0;
    int level = 0;  // what covers what (modal_level of the screen)
    uint32_t group = 0;
    bool is_tab = false, quest_tab = false, tab_current = false;
    int arrow = 0;  // page arrow part: 1 left, 2 right
    bool arrow_shown = false;
    Rect arrow_rect;
    std::vector<Rect> opt_arrows;  // arrow-like panes inside an entry (options: choice and slider rows)
    std::string names;             // child pane names (debug log)
};

bool is_modal_screen(const std::string& name) {
    return starts(name, "SystemWindow") || starts(name, "GameOver") || starts(name, "ShopBtnList") ||
           name == "AppSystemWindow_00" || name == "OptionWindow_00" || name == "End_00";
}
// Only the topmost level is clickable: conversation answers lie over everything, then dialogs and shop lists.
int modal_level(const std::string& name) {
    if (starts(name, "MessageDialog")) return 2;
    return is_modal_screen(name) ? 1 : 0;
}

void walk_layout(uint32_t root, bool box_cursor, Pointer& ps, std::vector<std::pair<float, float>>& corners,
                 std::unordered_map<uint32_t, PartInfo>& parts, int level) {
    struct Item {
        uint32_t pane;
        float px, py, sx, sy;
        uint32_t part;
        int depth;
        uint32_t group;
    };
    std::vector<Item> stack;
    stack.push_back({root, 0, 0, 1, 1, 0, 0, 0});
    int guard = 0;
    while (!stack.empty() && guard++ < 20000) {
        const Item it = stack.back();
        stack.pop_back();
        const uint32_t pane = it.pane;
        if (!pane_ok(pane) || it.depth > 30) continue;
        const uint8_t flags = r8(pane + kFlag), alpha = r8(pane + kFlag + 1);
        if (!(flags & 1) || alpha == 0) continue;
        const float px = it.px + it.sx * rf(pane + kTranslate);
        const float py = it.py + it.sy * rf(pane + kTranslate + 4);
        const float sx = it.sx * rf(pane + kScale);
        const float sy = it.sy * rf(pane + kScale + 4);
        const std::string name = read_name(pane);
        uint32_t part = it.part, group = it.group;
        if (starts(name, "Pa_PagePorch")) {
            // the pause menu keeps three pages side by side (x = -500, 0, +500): the neighbours are not selectable
            if (std::fabs(rf(pane + kTranslate)) > 200.0f) continue;
            group = pane;
        } else if (starts(name, "Pa_") && !starts(name, "Pa_Rstick")) {
            part = pane;
            PartInfo& pi = parts[pane];
            pi.group = group;
            // category tabs: inventory ("Pa_CategoryS_nn") and adventure log ("Pa_ChallengeCategory_nn")
            pi.quest_tab = starts(name, "Pa_ChallengeCategory");
            pi.is_tab = pi.quest_tab || starts(name, "Pa_Category");
        }
        if (name == "N_Check_00" && part) {
            auto pit = parts.find(part);
            if (pit != parts.end() && pit->second.is_tab) pit->second.tab_current = true;  // the highlighted category
        }
        auto rect_of = [&](Rect& r) {
            const float w = rf(pane + kSize) * std::fabs(sx), h = rf(pane + kSize + 4) * std::fabs(sy);
            const uint8_t bp = r8(pane + kFlag + 3);
            const int hpos = bp % 3, vpos = bp / 3;  // 0 center, 1 left/top, 2 right/bottom
            const float x0 = hpos == 0 ? px - w * 0.5f : (hpos == 1 ? px : px - w);
            const float y1 = vpos == 0 ? py + h * 0.5f : (vpos == 1 ? py : py + h);  // y up
            r = {0.5f + x0 / kLayoutW, 0.5f - y1 / kLayoutH, 0.5f + (x0 + w) / kLayoutW, 0.5f - (y1 - h) / kLayoutH};
            return w > 0 && h > 0;
        };
        if (name == "Pa_ArrowL_00" || name == "Pa_ArrowR_00") {
            PartInfo& pi = parts[pane];
            pi.arrow = name[8] == 'L' ? 1 : 2;
            rect_of(pi.arrow_rect);
        } else if ((name == "N_ArrowL_00" || name == "N_ArrowR_00") && part) {
            auto pit = parts.find(part);
            if (pit != parts.end() && pit->second.arrow == (name[7] == 'L' ? 1 : 2)) pit->second.arrow_shown = true;
        }
        if (part && it.depth > 0) {
            PartInfo& pi = parts[part];
            if (pi.names.size() < 600) pi.names += name + ",";  // logged on click (INPUT_LOG)
            // any arrow-like pane inside an entry ("<" / ">" of option rows): the side is decided later from its
            // position relative to the entry's hit area
            if (!starts(name, "Pa_Arrow") && name.find("Arrow") != std::string::npos) {
                Rect ar;
                if (rect_of(ar) && pi.opt_arrows.size() < 8) pi.opt_arrows.push_back(ar);
            }
        }
        if (box_cursor) {
            if (name == "N_LT_00" || name == "N_RT_00" || name == "N_LB_00" || name == "N_RB_00")
                corners.emplace_back(0.5f + px / kLayoutW, 0.5f - py / kLayoutH);
        } else if (starts(name, "B_Hit") || starts(name, "B_Btn")) {
            Rect r;
            PartInfo& pi = parts[part ? part : pane];
            if (!pi.has_hit && rect_of(r)) {
                pi.has_hit = true;
                pi.hit = r;
                pi.order = (int)parts.size();
                pi.level = level;
            }
        } else if (name == "N_MapT_00") {
            ps.has_map = true;
            ps.map_x = 0.5f + px / kLayoutW;
            ps.map_y = 0.5f - py / kLayoutH;
            ps.map_scale_x = it.sx;
            ps.map_scale_y = it.sy;
            // map controller of the screen that owns this pane: [screen+0x478]+0x648
            const uint32_t obj = g_map_obj.load();
            if (obj && g_frame.load() - g_map_obj_frame.load() <= 4 && ok(obj, 0x480) && r32(obj + 0xA8) == pane) {
                const uint32_t ctrl = r32(obj + 0x478);
                if (ok(ctrl + 0x648, 0x40) && ok(ctrl + 0x3160, 0x10)) ps.map_ctrl = ctrl + 0x648;
            }
        } else if (name == "N_Sunaarashi_00" || name == "N_Dungeon_00") {
            if (alpha >= 16) ps.map_blocked = true;
        } else if (starts(name, "T_") && part && alpha >= 16) {
            auto ti = g_texts.find(pane);
            Rect r;
            if (ti != g_texts.end() && !ti->second.text.empty() && rect_of(r)) {
                PartInfo& pi = parts[part];
                if (!pi.has_text) pi.text = r;
                else
                    pi.text = {std::min(pi.text.x0, r.x0), std::min(pi.text.y0, r.y0), std::max(pi.text.x1, r.x1),
                               std::max(pi.text.y1, r.y1)};
                pi.has_text = true;
                if (starts(name, "T_Btn") && alpha >= pi.label_alpha) {
                    pi.label = ti->second.text;
                    pi.label_alpha = alpha;
                }
            }
        }
        const uint32_t head = pane + kChildHead;
        uint32_t node = r32(head);
        for (int i = 0; i < 512 && node && node != head; i++) {
            if (!pane_ok(node)) break;
            stack.push_back({node, px, py, sx, sy, part, it.depth + 1, group});
            node = r32(node + kNext);
        }
    }
}

void compute_pointer(Pointer& ps) {
    rebuild_roots();
    ps = Pointer{};
    ps.valid = true;
    if (screen_visible("AppMapDungeon_00")) ps.map_blocked = true;
    std::unordered_map<uint32_t, PartInfo> parts;
    for (auto& [root, shown] : g_root_shown) {
        if (!shown) continue;
        const std::string& scr = g_root_screen[root];
        if (scr == "BoxCursorTV") {
            std::vector<std::pair<float, float>> corners;
            Pointer dummy;
            walk_layout(root, true, dummy, corners, parts, 0);
            if (corners.size() >= 2) {
                float x = 0, y = 0;
                for (auto& c : corners) x += c.first, y += c.second;
                ps.has_box_cursor = true;
                ps.box_x = x / corners.size();
                ps.box_y = y / corners.size();
            }
            continue;
        }
        if (starts(scr, "MainScreen") || scr == "DoCommand_00" || scr == "MainShortCut_00" || scr == "PauseMenuBG_00")
            continue;  // HUD layers: no pointer targets
        std::vector<std::pair<float, float>> none;
        walk_layout(root, false, ps, none, parts, modal_level(scr));
    }
    auto on_screen = [](const Rect& r) { return r.cx() > 0.0f && r.cx() < 1.0f && r.cy() > 0.0f && r.cy() < 1.0f; };
    int top_level = 0;
    for (auto& [part, pi] : parts)
        if (pi.has_hit) top_level = std::max(top_level, pi.level);
    for (auto& [part, pi] : parts)
        if (pi.arrow && pi.arrow_shown && top_level == 0 && on_screen(pi.arrow_rect)) {
            ps.page_arrow[pi.arrow - 1] = true;
            ps.page_arrow_rect[pi.arrow - 1] = pi.arrow_rect;
        }
    std::vector<std::pair<int, Target>> ordered;
    for (auto& [part, pi] : parts) {
        if (!pi.has_hit || pi.level < top_level) continue;  // something lies on top
        Target t;
        t.part = part;
        t.hit = pi.hit;
        t.label = pi.label;
        t.text_alpha = pi.label_alpha;
        t.group = pi.group;
        t.tab = pi.is_tab ? 0 : -1;
        t.names = pi.names;
        for (const Rect& ar : pi.opt_arrows) {
            const int k = ar.cx() < pi.hit.cx() ? 0 : 1;
            if (!t.has_arrow[k]) t.has_arrow[k] = true, t.arrow[k] = ar;
        }
        const float hw = pi.hit.x1 - pi.hit.x0, hh = pi.hit.y1 - pi.hit.y0;
        if (on_screen(pi.hit) && hw > 0.02f && hh > 0.02f) t.rect = pi.hit;
        else if (pi.has_text && on_screen(pi.text)) {
            // the bounding pane is a tiny / off-screen anchor (title menu): the button's text is the click area
            const float pad_y = (pi.text.y1 - pi.text.y0) * 0.2f;
            t.rect = {pi.text.x0 - 0.01f, pi.text.y0 - pad_y, pi.text.x1 + 0.01f, pi.text.y1 + pad_y};
        } else
            continue;
        ordered.emplace_back(pi.order, t);
    }
    std::sort(ordered.begin(), ordered.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [o, t] : ordered) ps.targets.push_back(t);
    // category tabs: index them left to right
    std::vector<int> tabs;
    for (int i = 0; i < (int)ps.targets.size(); i++)
        if (ps.targets[i].tab >= 0) tabs.push_back(i);
    std::sort(tabs.begin(), tabs.end(), [&](int a, int b) { return ps.targets[a].rect.cx() < ps.targets[b].rect.cx(); });
    ps.tab_count = (int)tabs.size();
    for (int k = 0; k < (int)tabs.size(); k++) {
        ps.targets[tabs[k]].tab = k;
        const PartInfo& pi = parts[ps.targets[tabs[k]].part];
        if (pi.tab_current) ps.current_tab = k;
        ps.quest_tabs |= pi.quest_tab;
    }
    // the target the game's own cursor is on: its entry's hit pane lies below the target's part pane
    uint32_t pane = cursor_focus_pane();
    for (int up = 0; up < 4 && pane_ok(pane) && ps.game_sel < 0; up++) {
        for (int i = 0; i < (int)ps.targets.size(); i++)
            if (ps.targets[i].part == pane) {
                ps.game_sel = i;
                break;
            }
        pane = r32(pane + kParent);
    }
}

// ---- inventory carousel (pc_menus -> hooks) ----
std::atomic<int> g_inv_jump{-1}, g_inv_jump_pages{0}, g_inv_flick{0};
std::atomic<bool> g_inv_snap{false};
std::atomic<int64_t> g_slide_ms{-1000000};  // last frame with a carousel slide running

// The inventory screen (r3), before its carousel update (0x02FF644C): a category click, or several wheel notches,
// use the game's own page jump (0x02FF3AC0, no slide; to the page in +0x1B90) to the page next to the target, and
// the last step slides in like a normal page turn (flick). +0x1B94 = item of the page the cursor lands on.
// +0x1B78 page count, +0x1B8C current page; category table +0x1BC0, 7 x 0x48: page count +0x20, first page +0x40.
// Categories without pages have no tab, so the k-th category with pages is the k-th tab.
void inventory_jump(PPCContext* c) {
    const int tab = g_inv_jump.exchange(-1);
    const int wheel_pages = g_inv_jump_pages.exchange(0);
    const uint32_t inv = c->r[3];
    if ((tab < 0 && wheel_pages == 0) || !ok(inv + 0x1B78, 0x1BC0 + 7 * 0x48 - 0x1B78)) return;
    uint32_t target = 0xFFFFFFFF;
    const uint32_t pages = r32(inv + 0x1B78), cur = r32(inv + 0x1B8C);
    if (tab >= 0) {
        int k = 0;
        for (uint32_t ci = 0; ci < 7; ci++) {
            const uint32_t e = inv + 0x1BC0 + ci * 0x48;
            if (r32(e + 0x20) == 0 || k++ != tab) continue;
            target = r32(e + 0x40);
            break;
        }
    } else if (pages > 0) {
        target = (uint32_t)std::clamp((int)cur + wheel_pages, 0, (int)pages - 1);
    }
    if (target >= pages || target == cur) return;
    const int dir = target > cur ? 1 : -1;
    const uint32_t land = target - dir;  // the page before the target: the last step slides in
    if (land != cur) {
        mem::w32(inv + 0x1B90, land);
        mem::w32(inv + 0x1B94, 0);
        PPCContext saved = *c;
        c->r[3] = inv;
        c->lr = 0;
        rt_call(c, 0x02FF3AC0);
        *c = saved;
    }
    g_inv_flick = dir;
}
}  // namespace

bool State::has(std::string_view text) const {
    for (auto& g : guides)
        if (g.text == text) return true;
    return false;
}
bool State::has_button(uint32_t button) const {
    for (auto& g : guides)
        if (g.button == button) return true;
    return false;
}
uint32_t State::button_for(std::string_view text, uint32_t fallback) const {
    for (auto& g : guides)
        if (g.text == text && g.button) return g.button;
    return fallback;
}
bool State::held_items() const {
    return !any_menu && has("Drop") && !has("Throw") && (has("Put away") || has("Far") || has("Near"));
}

// ---- hooks ---------------------------------------------------------------------------------------------------
}  // namespace pceui

BOTW_HOOK(0x03c4ba94) {  // stb r0,0xd0(r29): TextBox::SetString end, r29 = text box (full string in its buffer)
    using namespace pceui;
    const uint32_t pane = c->r[29];
    if (!ok(pane, 0xD0)) return;
    TextInfo ti;
    ti.name = read_name(pane);
    const uint32_t buf = r32(pane + kTextBuf);
    const uint32_t len = std::min<uint32_t>(r16(pane + kTextLen), 256);
    if (ti.name.empty() || !ok(buf, len * 2 + 2)) return;
    std::u16string s;
    for (uint32_t i = 0; i < len; i++) {
        const char16_t ch = (char16_t)r16(buf + i * 2);
        if (ch == 0) break;
        if (ch == 0x0E) {  // inline tag: skip header + params
            if (i + 3 < len) i += 3 + (r16(buf + (i + 3) * 2) + 1) / 2;
            continue;
        }
        s.push_back(ch);
    }
    ti.text = utf16_to_utf8(s);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_texts.size() > 30000) g_texts.clear();
    const std::string name = ti.name;
    g_texts[pane] = std::move(ti);
    if (pcprompts::active()) on_text_prompts(pane, name, buf, len);
}

BOTW_HOOK(0x03a3edc8) {  // lwz r12,0xe4(r11): ui screen layout loaded, r30 = screen, r24 = layout name
    using namespace pceui;
    const uint32_t obj = c->r[30], name_ptr = c->r[24];
    if (!ok(name_ptr, 1) || !ok(obj, 0x10)) return;
    std::string name;
    for (uint32_t i = 0; i < 48; i++) {
        const char ch = (char)r8(name_ptr + i);
        if (ch == 0) break;
        name.push_back(ch);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_screens.begin(); it != g_screens.end();)  // an object address can be reused by another screen
        it = it->second == obj ? g_screens.erase(it) : std::next(it);
    g_screens[name] = obj;
    g_screen_sig[obj] = r32(obj + 0x0C);
}

BOTW_HOOK(0x02f033e0) {  // lwz r6,0xa8(r27): world map screen update, r27 = map screen
    g_map_ms = now_ms();
    pceui::g_map_obj = c->r[27];
    pceui::g_map_obj_frame = pceui::g_frame.load();
}

BOTW_HOOK(0x03a2e8e4) {  // stw r29,0x20(r1): box cursor manager update (r31), only while the cursor takes input
    using namespace pceui;
    const uint32_t mgr = c->r[31];
    const uint32_t f = g_frame.load();
    g_cursor_mgr = mgr;
    g_cursor_frame = f;
    if (!ok(mgr, 0x30)) return;
    // our own request was consumed by the update after the last hook, so a taken slot means the screen asked itself
    const uint32_t pending = r32(mgr + 0x28);
    if (pending != 0) g_cursor_owned_frame = f;
    const bool owned = f - g_cursor_owned_frame.load() <= 8;
    std::lock_guard<std::mutex> lock(g_focus_mutex);
    if (f - g_focus.frame > 2) return;
    const uint32_t entry = find_cursor_entry(mgr, g_focus.part, g_focus.area);
    if (!entry) g_focus.result = 2;
    else if (entry == r32(mgr + 0x24) && (pending == 0 || pending == entry)) g_focus.result = 3;
    else if (owned) g_focus.result = f - g_list_frame.load() <= 4 ? 1 : 4;  // its list widget has it / d-pad
    else {
        mem::w32(mgr + 0x28, entry);
        g_focus.result = 1;
    }
}

BOTW_HOOK(0x02e816e8) {  // stw r31,0x2a8(r30): shop list widget (r30) asked the cursor for its entry (r31)
    using namespace pceui;
    const uint32_t widget = c->r[30], current = c->r[31];
    const uint32_t f = g_frame.load();
    const uint32_t mgr = g_cursor_mgr.load();
    if (!ok(widget, 0x2C0) || !ok(mgr, 0x30) || f - g_cursor_frame.load() > 3) return;
    std::lock_guard<std::mutex> lock(g_focus_mutex);
    if (f - g_focus.frame > 2) return;
    const uint32_t entry = find_cursor_entry(mgr, g_focus.part, g_focus.area);
    const int32_t index = entry && entry != current ? list_index_of(widget, entry) : -1;
    if (index >= 0) {
        mem::w32(widget + 0x29C, (uint32_t)index);
        g_list_frame = f;
    }
}

BOTW_HOOK(0x02ff64c8) {  // mr r3,r31 before bl ease(r4 = &carousel offset, f1 = target): a burst snaps
    using namespace pceui;
    g_slide_ms = now_ms();  // (the ease runs only while the carousel is off its page)
    if (!g_inv_snap.load() || !ok(c->r[4], 4)) return;
    mem::wf32(c->r[4], (float)c->f[1]);  // the ease then has nothing left to do
}

BOTW_HOOK(0x02ff6764) { pceui::inventory_jump(c); }  // mr r3,r28 before bl 0x02FF644C (carousel update)
BOTW_HOOK(0x02ff67e8) { pceui::inventory_jump(c); }  // the same in the other inventory update path

namespace pceui {
void tick_frame() { g_frame++; }

std::vector<HudKey> hud_keys() {
    std::lock_guard<std::mutex> l(g_hud_mutex);
    return g_hud;
}
uint32_t frame() { return g_frame.load(); }

const State& state() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t f = g_frame.load();
    if (f != g_state_frame) {
        compute(g_state);
        g_state_frame = f;
    }
    return g_state;
}

const Pointer& pointer() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t f = g_frame.load();
    if (f != g_pointer_frame) {
        compute_pointer(g_pointer);
        g_pointer_frame = f;
    }
    return g_pointer;
}

bool pointer_screen(const State& st) {
    return st.title || st.inventory || st.map || st.dialog || st.choice || st.camera_dialog;
}

std::string shown_screens() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string r;
    for (auto& [name, obj] : g_screens) {
        if (!screen_shown_obj(name, obj) || starts(name, "MainScreen") || name == "DoCommand_00") continue;
        if (!r.empty()) r += ",";
        r += name;
    }
    return r;
}

Focus request_focus(uint32_t part, const Rect& area) {
    const uint32_t f = g_frame.load();
    std::lock_guard<std::mutex> lock(g_focus_mutex);
    if (f - g_focus.frame > 4 || g_focus.part != part) g_focus.result = 0;
    g_focus.part = part;
    g_focus.area = area;
    g_focus.frame = f;
    if (f - g_cursor_frame.load() > 3 || g_focus.result == 2 || g_focus.result == 4) return Focus::Unavailable;
    return g_focus.result == 3 ? Focus::Focused : Focus::Requested;
}

bool set_pause_start_page(int page) {
    // the pause menu data singleton (pointer at 0x10469978) keeps the page the menu was closed on at +0x37ED8
    // (setters 0x03076D0C / 0x03076D7C / 0x03076D2C: 0 inventory, 1 system, 2 adventure log)
    constexpr uint32_t kInstancePtr = 0x10469978, kLastPage = 0x37ED8;
    if (page < 0 || page > 2 || !mem::readable(kInstancePtr, 4)) return false;
    const uint32_t inst = r32(kInstancePtr);
    if (!ok(inst, 0x10) || !ok(inst + kLastPage, 4) || r32(inst + kLastPage) > 2) return false;
    mem::w32(inst + kLastPage, (uint32_t)page);
    return true;
}

// ---- world map: the controller keeps the scroll (layout units, +0x20 current / +0x30 previous) and the cursor
// (controller + 0x3160 - 0x648: position, +8 the target it eases to)
bool set_map_origin(const Pointer& ps, float nx, float ny) {
    if (!ps.map_ctrl || std::fabs(ps.map_scale_x) < 1e-4f || std::fabs(ps.map_scale_y) < 1e-4f) return false;
    const float dx = (nx - ps.map_x) * kLayoutW / ps.map_scale_x;
    const float dy = -(ny - ps.map_y) * kLayoutH / ps.map_scale_y;
    const float x = rf(ps.map_ctrl + 0x20) + dx, y = rf(ps.map_ctrl + 0x24) + dy;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    for (uint32_t off : {0x20u, 0x30u}) mem::wf32(ps.map_ctrl + off, x), mem::wf32(ps.map_ctrl + off + 4, y);
    const uint32_t cursor = ps.map_ctrl - 0x648 + 0x3160;
    if (ok(cursor, 0x10))
        for (uint32_t off : {0x0u, 0x8u}) mem::wf32(cursor + off, -x), mem::wf32(cursor + off + 4, -y);
    return true;
}

bool map_point_at(const Pointer& ps, float nx, float ny, float& x, float& y) {
    if (!ps.map_ctrl || std::fabs(ps.map_scale_x) < 1e-4f || std::fabs(ps.map_scale_y) < 1e-4f) return false;
    x = (nx - ps.map_x) * kLayoutW / ps.map_scale_x;
    y = (ps.map_y - ny) * kLayoutH / ps.map_scale_y;
    return std::isfinite(x) && std::isfinite(y);
}

bool set_map_cursor(const Pointer& ps, float x, float y) {
    const uint32_t cursor = ps.map_ctrl - 0x648 + 0x3160;
    if (!ps.map_ctrl || !std::isfinite(x) || !std::isfinite(y) || !ok(cursor, 0x10)) return false;
    // the cursor's limits (x0, y0, x1, y1) at controller + 0x60
    if (ok(ps.map_ctrl + 0x60, 0x10)) {
        const float x0 = rf(ps.map_ctrl + 0x60), y0 = rf(ps.map_ctrl + 0x64);
        const float x1 = rf(ps.map_ctrl + 0x68), y1 = rf(ps.map_ctrl + 0x6C);
        if (std::isfinite(x0) && std::isfinite(x1) && x0 <= x1) x = std::clamp(x, x0, x1);
        if (std::isfinite(y0) && std::isfinite(y1) && y0 <= y1) y = std::clamp(y, y0, y1);
    }
    for (uint32_t off : {0x0u, 0x8u}) mem::wf32(cursor + off, x), mem::wf32(cursor + off + 4, y);
    return true;
}

void queue_inventory_jump(int tab) {
    g_inv_jump = tab;
    g_inv_jump_pages = 0;
    g_inv_flick = 0;
}
void queue_inventory_jump_pages(int pages) { g_inv_jump_pages = pages; }
bool inventory_jump_pending() { return g_inv_jump.load() >= 0 || g_inv_jump_pages.load() != 0; }
int take_inventory_flick() { return g_inv_flick.exchange(0); }
void set_inventory_snap(bool on) { g_inv_snap = on; }
bool inventory_sliding() { return now_ms() - g_slide_ms.load() < 70; }

int Pointer::at(float x, float y, uint32_t only_group) const {
    int best = -1;
    float best_area = 1e9f;
    for (int i = 0; i < (int)targets.size(); i++) {
        const Rect& r = targets[i].rect;
        if (!r.contains(x, y)) continue;
        if (only_group && targets[i].group && targets[i].group != only_group) continue;
        const float area = (r.x1 - r.x0) * (r.y1 - r.y0);
        if (area < best_area) best_area = area, best = i;
    }
    return best;
}

int Pointer::selected() const {
    if (targets.empty()) return -1;
    if (game_sel >= 0) return game_sel;
    if (has_box_cursor) {
        // the box cursor snaps onto the selected entry's bounding pane
        int in = -1;
        float in_area = 1e9f;
        for (int i = 0; i < (int)targets.size(); i++) {
            const Rect& h = targets[i].hit;
            const float a = (h.x1 - h.x0) * (h.y1 - h.y0);
            if (h.contains(box_x, box_y) && a < in_area) in = i, in_area = a;
        }
        if (in < 0) in = at(box_x, box_y);
        if (in >= 0) return in;
        int best = -1;
        float bd = 1e9f;
        for (int i = 0; i < (int)targets.size(); i++) {
            const float dx = targets[i].hit.cx() - box_x, dy = targets[i].hit.cy() - box_y;
            if (dx * dx + dy * dy < bd) bd = dx * dx + dy * dy, best = i;
        }
        return bd < 0.01f ? best : -1;
    }
    // title-like menus: the selected entry's text is fully opaque, the others are dimmed
    int best = -1, count255 = 0;
    uint8_t max_a = 0;
    for (int i = 0; i < (int)targets.size(); i++) {
        if (targets[i].label.empty()) continue;
        if (targets[i].text_alpha >= 250) count255++;
        if (targets[i].text_alpha > max_a) max_a = targets[i].text_alpha, best = i;
    }
    return count255 == 1 ? best : -1;
}

int Pointer::find_label(std::string_view label) const {
    for (int i = 0; i < (int)targets.size(); i++)
        if (targets[i].label == label) return i;
    return -1;
}
}  // namespace pceui
