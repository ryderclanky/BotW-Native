// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was
// not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Derived from Zelda-on-PC src/pcedition/PCUI.{h,cpp} (https://github.com/mircea32000/Zelda-on-PC).
//
// Live model of the game's own UI (nn::ui2d / nw::lyt layouts of U-King.rpx v1.5.0). Hooks report screen layouts
// (ui screen load) and every text box string change; from those the shown screens and their panes are walked, so
// the mouse can point at menu entries. Called from the VPADRead thread (pc_menus.cpp); the hooks are in pc_ui.cpp.
#pragma once
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace pceui {
struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // normalized picture coordinates (0..1, origin top-left)
    bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

struct State {
    bool valid = false;
    bool inventory = false;     // pause menu (inventory / system / adventure log)
    bool map = false;           // Sheikah Slate map (AppHome_00)
    bool title = false;         // title screen menu
    bool dialog = false;        // system window / game over / shop list / other modal screen
    bool choice = false;        // a conversation shows its answer buttons
    bool scope = false;         // scope view ("Place pin")
    bool camera_rune = false;   // the camera rune looking
    bool camera_dialog = false; // ... asking about the picture just taken
    bool any_menu = false;      // the gameplay HUD is suspended by a menu
    bool hud = false;           // the gameplay HUD is shown
    // the game's own action guides on screen ("Throw", "Let go", "Cancel", ...) with their GamePad button bit
    // (input::BTN_*; 0 = unknown), English labels. Drive the context controls (pc_controls.cpp).
    struct Guide {
        std::string text;
        uint32_t button = 0;
    };
    std::vector<Guide> guides;
    std::string action_a;       // the A action of the action ring ("Take", "Open", ...), empty if none
    bool has(std::string_view text) const;
    bool has_button(uint32_t button) const;
    uint32_t button_for(std::string_view text, uint32_t fallback) const;
    // materials held in the hands (taken with "Hold" in the inventory): "Drop" + "Put away", no "Throw"
    bool held_items() const;
};

struct Target {
    uint32_t part = 0;   // owning part pane (identity)
    Rect rect;           // click area
    Rect hit;            // raw bounding pane
    std::string label;   // button text if any
    uint8_t text_alpha = 0;
    uint32_t group = 0;  // inventory page the entry belongs to, 0 = none
    int tab = -1;        // category tab index, -1 = not a tab
    bool has_arrow[2] = {false, false};  // "<" / ">" panes inside the entry (options choice rows), if any
    Rect arrow[2];
    std::string names;  // child pane names (debug log)
};

struct Pointer {
    bool valid = false;
    std::vector<Target> targets;
    bool has_box_cursor = false;
    float box_x = 0, box_y = 0;
    bool has_map = false;
    bool map_blocked = false;
    float map_x = 0, map_y = 0;
    uint32_t map_ctrl = 0;
    float map_scale_x = 1, map_scale_y = 1;
    int tab_count = 0;
    int current_tab = -1;
    bool quest_tabs = false;
    int game_sel = -1;
    bool page_arrow[2] = {false, false};
    Rect page_arrow_rect[2];
    int selected() const;
    int at(float x, float y, uint32_t only_group = 0) const;
    int find_label(std::string_view label) const;
};

// HUD action ring (DoCommand_00): the GamePad button pictures are hidden while the PC prompts are on, and the
// overlay draws the bound key at their place (runtime/cemu_shims/overlay_menu.cpp). Any thread.
struct HudKey {
    Rect rect;          // normalized picture coordinates
    float alpha = 1;    // follows the label's fade
    std::string label;  // "E", "Space", "LMB"
};
std::vector<HudKey> hud_keys();

// The UI frame counter (advanced once per game read of the GamePad by pc_menus).
void tick_frame();
uint32_t frame();

// Recomputed at most once per frame. Only the VPADRead thread calls these.
const State& state();
const Pointer& pointer();
bool pointer_screen(const State& st);
std::string shown_screens();

enum class Focus { Unavailable, Requested, Focused };
Focus request_focus(uint32_t part, const Rect& area);

bool set_map_origin(const Pointer& ps, float x, float y);
bool map_point_at(const Pointer& ps, float nx, float ny, float& x, float& y);
bool set_map_cursor(const Pointer& ps, float x, float y);
// Page the pause menu opens on next time (0 inventory, 1 system, 2 adventure log).
bool set_pause_start_page(int page);

// Inventory carousel, applied by the hooks in the next inventory update: jump to a category tab (its first page) or
// by several pages (wheel), with the game's own page jump to the page before; the last step is a flick in
// take_inventory_flick()'s direction, so it slides in. While snap is on (a burst of flicks), page changes apply at once.
void queue_inventory_jump(int tab);
void queue_inventory_jump_pages(int pages);
bool inventory_jump_pending();
int take_inventory_flick();
void set_inventory_snap(bool on);
bool inventory_sliding();  // a page slide ran in the last ~2 frames
}  // namespace pceui
