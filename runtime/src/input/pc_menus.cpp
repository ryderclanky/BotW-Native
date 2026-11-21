// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was
// not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Derived from Zelda-on-PC src/pcedition/PCMenus.cpp and the pause menu page keys of PCInput.cpp
// (https://github.com/mircea32000/Zelda-on-PC). Not ported: the save / load macros of its PC settings menu.
#include "pc_menus.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "../game/pc_ui.h"
#include "../log.h"
#include "input.h"

namespace pcmenus {
namespace {
using namespace input;
const bool g_log = [] {
    const char* s = std::getenv("BOTW_PCE_INPUT_LOG");
    return s && *s && *s != '0';
}();

// button pulses (pressed for a few reads, then a gap so the game sees a new press)
struct Pulse {
    uint32_t buttons = 0;
    int on = 0, off = 0;
    bool busy() const { return on > 0 || off > 0; }
    void start(uint32_t b, int on_frames = 3, int off_frames = 3) { buttons = b, on = on_frames, off = off_frames; }
    uint32_t step() {
        if (on > 0) return on--, buttons;
        if (off > 0) off--;
        return 0;
    }
};
Pulse g_nav, g_confirm;

// pointer navigation
uint32_t g_frame = 0;
uint32_t g_pending_part = 0, g_pending_frame = 0;  // a click waits for the selection to reach this target
uint32_t g_pending_btn = BTN_A;                    // ... and presses this (d-pad left / right on an option's arrows)
uint32_t g_last_move_frame = 0;
float g_last_mx = -1, g_last_my = -1;
int g_last_selected = -2;
uint32_t g_nav_wait_until = 0;
uint32_t g_focus_part = 0, g_focus_since = 0;
bool g_hover_chase = false;      // the hovered entry is not selected yet
uint32_t g_hover_done_part = 0;  // selected by hovering once; not again while the mouse rests on it

// category tabs (the right stick switches them)
int g_tab_target = -1;
uint32_t g_tab_until = 0;
int g_tab_steps = 0, g_tab_phase = 0;
uint32_t g_tab_verify_at = 0;
int g_tab_retries = 0;
constexpr uint32_t kTabCheckFrames = 3;
int g_wheel_pages = 0;  // inventory: pages still to turn for the wheel
int g_pager_wheel = 0;  // shops and other screens with tabs
uint32_t g_wheel_next_at = 0, g_smooth_until = 0;
bool g_smooth = false;  // the page turns now under way slide (wheel, last step of a jump)

// inventory: rotate Link by dragging (middle button anywhere, left button on empty space)
bool g_rot_drag = false;
float g_rot_last_x = 0;

// map
bool g_map_dragging = false, g_map_click = false, g_map_target_active = false, g_map_confirm = false;
float g_drag_mx = 0, g_drag_my = 0, g_drag_ox = 0, g_drag_oy = 0;
float g_map_tx = 0, g_map_ty = 0;
uint32_t g_map_target_frame = 0;
int g_map_settle = 0, g_map_cursor_frames = 0;
float g_map_cx = 0, g_map_cy = 0;
int g_zoom_frames = 0;
float g_zoom_dir = 0;

// pause menu pages (L / R order)
enum class Page { None = -1, AdventureLog = 0, Inventory = 1, System = 2 };
int g_page_target = -1;
uint32_t g_page_start = 0, g_page_next = 0;
int g_page_steps = 0;
int g_close_frames = 0;

void request_tab(int target, int current) {
    const int from = g_tab_target >= 0 ? g_tab_target : current;
    g_tab_steps += target - from;
    g_tab_target = target;
    g_tab_retries = 0;
    g_tab_until = g_frame + 600;
    g_tab_verify_at = g_frame + kTabCheckFrames;
}

void jump_tab(int target) {
    pceui::queue_inventory_jump(target);
    g_tab_target = target;
    g_tab_steps = 0;
    g_tab_phase = 0;
    g_tab_retries = 0;
    g_tab_until = g_frame + 600;
    g_tab_verify_at = g_frame + 12;
}

Page detect_page(const pceui::State& st, const pceui::Pointer& ps) {
    if (!st.inventory) return Page::None;
    if (ps.tab_count > 0) return ps.quest_tabs ? Page::AdventureLog : Page::Inventory;
    if (ps.find_label("Save") >= 0 || ps.find_label("Load") >= 0) return Page::System;
    return Page::AdventureLog;
}
int start_page_code(Page p) { return p == Page::Inventory ? 0 : p == Page::System ? 1 : 2; }

void request_page(Page p, bool menu_open) {
    // closed menu: it opens directly on the page (the game reads the page it was closed on when it opens)
    if (!menu_open) pceui::set_pause_start_page(start_page_code(p));
    g_page_target = (int)p;
    g_page_start = g_frame;
    g_page_next = g_frame + 6;
    g_page_steps = 0;
}

void navigate(const pceui::Pointer& ps, int from, int to) {
    if (g_nav.busy() || g_frame < g_nav_wait_until) return;
    const pceui::Rect& a = ps.targets[from].rect;
    const pceui::Rect& b = ps.targets[to].rect;
    const float dx = b.cx() - a.cx(), dy = b.cy() - a.cy();
    const uint32_t dir = std::fabs(dx) > std::fabs(dy) ? (dx > 0 ? BTN_RIGHT : BTN_LEFT) : (dy > 0 ? BTN_DOWN : BTN_UP);
    g_nav.start(dir, 2, 2);
    g_nav_wait_until = g_frame + 8;  // give the menu time to move its cursor
}

int find_part(const pceui::Pointer& ps, uint32_t part) {
    for (int i = 0; i < (int)ps.targets.size(); i++)
        if (ps.targets[i].part == part) return i;
    return -1;
}

void update_map(const pceui::Pointer& ps, FrameIO& io) {
    const float mx = io.mouse_x, my = io.mouse_y;
    if (io.lmb_pressed && io.mouse_inside) {
        g_drag_mx = mx, g_drag_my = my;
        g_drag_ox = ps.map_x, g_drag_oy = ps.map_y;
        g_map_dragging = false;
        g_map_click = true;
    }
    if (io.lmb_down && g_map_click) {
        if (std::fabs(mx - g_drag_mx) + std::fabs(my - g_drag_my) > 0.01f) g_map_dragging = true;
        if (g_map_dragging) {  // grab and drag: the map follows the mouse 1:1
            g_map_tx = g_drag_ox + (mx - g_drag_mx);
            g_map_ty = g_drag_oy + (my - g_drag_my);
            g_map_target_active = true;
            g_map_confirm = false;
            g_map_target_frame = g_frame;
        }
    }
    if (io.lmb_released && g_map_click) {
        g_map_click = false;
        if (!g_map_dragging && io.mouse_inside) {
            if (pceui::map_point_at(ps, mx, my, g_map_cx, g_map_cy) && pceui::set_map_cursor(ps, g_map_cx, g_map_cy)) {
                // the map cursor jumps to the clicked spot (the map stays still), then confirm
                g_map_target_active = false;
                g_map_cursor_frames = 4;
            } else {
                // fallback: steer the clicked spot under the map cursor (screen centre) with the stick, then confirm
                g_map_tx = ps.map_x + (0.5f - mx);
                g_map_ty = ps.map_y + (0.5f - my);
                g_map_target_active = true;
                g_map_confirm = true;
                g_map_target_frame = g_frame;
            }
        } else if (g_map_dragging && ps.map_ctrl) {
            g_map_target_active = false;  // the drag wrote the scroll directly
        }
        g_map_dragging = false;
    }
    if (g_map_cursor_frames > 0) {
        // hold it there until the game lets go of the icon it was on and picks up what is under it, then confirm
        pceui::set_map_cursor(ps, g_map_cx, g_map_cy);
        if (--g_map_cursor_frames == 0 && !g_confirm.busy()) g_confirm.start(BTN_A);
    }
    if (g_map_target_active) {
        const float ex = g_map_tx - ps.map_x, ey = g_map_ty - ps.map_y;
        const bool reached = std::fabs(ex) < 0.004f && std::fabs(ey) < 0.004f;
        if (reached || g_frame - g_map_target_frame > 300) {
            if (g_map_settle > 0) g_map_settle--;
            else if (!io.lmb_down || !g_map_dragging) {
                g_map_target_active = false;
                if (g_map_confirm) g_confirm.start(BTN_A);
                g_map_confirm = false;
            }
        } else if (pceui::set_map_origin(ps, g_map_tx, g_map_ty)) {
            g_map_settle = g_map_confirm ? 4 : 0;  // instant: the map's scroll is written directly
        } else {
            constexpr float kGain = 22.0f;  // fallback: steer with the stick
            io.override_l = true;
            io.lx = std::clamp(-ex * kGain, -1.0f, 1.0f);
            io.ly = std::clamp(ey * kGain, -1.0f, 1.0f);
            g_map_settle = 0;
        }
    }
    if (io.rmb_pressed && !g_confirm.busy()) g_confirm.start(BTN_B);  // back / close the map
    if (io.wheel != 0) {
        g_zoom_dir = io.wheel > 0 ? 1.0f : -1.0f;
        g_zoom_frames = 5 * std::min(4, std::abs(io.wheel));
    }
    if (g_zoom_frames > 0) {
        g_zoom_frames--;
        io.override_r = true;
        io.rx = 0;
        io.ry = g_zoom_dir;
    }
}

void reset_pointer() {
    g_pending_part = 0;
    g_map_target_active = false;
    g_map_click = false;
    g_map_cursor_frames = 0;
    g_tab_target = -1;
    g_tab_steps = 0;
    g_tab_phase = 0;
    g_wheel_pages = 0;
    g_pager_wheel = 0;
    g_smooth = false;
    g_hover_chase = false;
    g_hover_done_part = 0;
    g_focus_part = 0;
    g_last_selected = -2;
    g_last_mx = g_last_my = -1.0f;
    g_rot_drag = false;
}

// save / load keys (Zelda-on-PC PCMenus UpdateMacro): open the pause menu on its System page, find the Save / Load
// button and confirm it through the pointer logic. The game then shows its own Save prompt / Load list.
enum class Macro { None, OpenMenu, FindButton, Select, Verify };
Macro g_macro = Macro::None;
const char* g_macro_label = "";
uint32_t g_macro_frame = 0;
int g_macro_tries = 0;

void update_macro(const pceui::State& st, FrameIO& io) {
    if ((io.save_key || io.load_key) && g_macro == Macro::None && !st.title && !st.dialog && !st.choice) {
        g_macro = Macro::OpenMenu;
        g_macro_label = io.save_key ? "Save" : "Load";
        g_macro_frame = g_frame;
        g_macro_tries = 0;
        if (!st.inventory) pceui::set_pause_start_page(start_page_code(Page::System));
        if (g_log) LOG("pc menus: %s key", g_macro_label);
    }
    switch (g_macro) {
        case Macro::None: return;
        case Macro::OpenMenu:
            if (st.inventory) {
                g_macro = Macro::FindButton;
                g_macro_frame = g_frame;
            } else if (g_frame - g_macro_frame == 1) {
                if (!g_confirm.busy()) g_confirm.start(BTN_PLUS);
            } else if (g_frame - g_macro_frame > 150) {
                g_macro = Macro::None;  // the pause menu is not available right now
            }
            break;
        case Macro::FindButton: {
            const pceui::Pointer& ps = pceui::pointer();
            const int t = ps.find_label(g_macro_label);
            if (t >= 0) {
                g_pending_part = ps.targets[t].part;
                g_pending_frame = g_frame;
                g_pending_btn = BTN_A;
                g_macro = Macro::Select;
            } else if (g_frame - g_macro_frame > 20) {
                // another page is shown (the menu was already open): step toward the System page
                if (++g_macro_tries > 6) g_macro = Macro::None;
                else {
                    if (!g_confirm.busy()) g_confirm.start(BTN_R);
                    g_macro_frame = g_frame;
                }
            }
            break;
        }
        case Macro::Select:
            if (g_pending_part == 0) {  // confirmed by the pointer logic (ignored while the menu is still opening)
                g_macro = Macro::Verify;
                g_macro_frame = g_frame;
            }
            break;
        case Macro::Verify:
            if (st.dialog || st.choice || !st.inventory) g_macro = Macro::None;  // the save / load window is up
            else if (g_frame - g_macro_frame > 14) {
                g_macro = ++g_macro_tries > 8 ? Macro::None : Macro::FindButton;
                g_macro_frame = g_frame;
            }
            break;
    }
}

// pause menu: I lands on the inventory, J on the adventure log (J again closes it)
void update_pages(const pceui::State& st, FrameIO& io) {
    const Page cur = st.inventory ? detect_page(st, pceui::pointer()) : Page::None;
    if (io.inventory_key && !st.inventory) pceui::set_pause_start_page(start_page_code(Page::Inventory));
    if (io.journal_key) {
        if (!st.inventory && !st.title) {
            request_page(Page::AdventureLog, false);
            g_close_frames = 0;
            if (!g_confirm.busy()) g_confirm.start(BTN_PLUS);
        } else if (cur == Page::AdventureLog) {
            g_close_frames = 4;
        } else {
            request_page(Page::AdventureLog, true);
        }
        if (g_log) LOG("pc menus: journal key (menu %s)", st.inventory ? "open" : "closed");
    }
    if (io.system_key) {
        // in play: pause menu on the System page; anywhere else (menus, map, dialogs, title): Back
        const bool open_system = io.in_play && !st.inventory && !st.title && !st.dialog && !st.choice && !st.map;
        if (open_system) {
            request_page(Page::System, false);
            g_close_frames = 0;
            if (!g_confirm.busy()) g_confirm.start(BTN_PLUS);
        } else if (!g_confirm.busy()) {
            g_confirm.start(BTN_B);
        }
        if (g_log) LOG("pc menus: system key (%s)", open_system ? "open system" : "back");
    }
    if (g_close_frames > 0) g_close_frames--, io.hold_add |= BTN_PLUS;
    if (g_page_target < 0) return;
    if (!st.inventory) {
        if (g_frame - g_page_start > 150) g_page_target = -1;  // the pause menu did not open
        return;
    }
    if ((int)cur == g_page_target || g_page_steps >= 4 || g_frame - g_page_start > 300) {
        g_page_target = -1;
        return;
    }
    if (g_frame >= g_page_next && !g_confirm.busy()) {
        g_confirm.start(g_page_target < (int)cur ? BTN_L : BTN_R);
        g_page_next = g_frame + 24;  // let the page slide in before checking again
        g_page_steps++;
    }
}
}  // namespace

void update(FrameIO& io) {
    pceui::tick_frame();
    g_frame++;
    const pceui::State& st = pceui::state();
    update_pages(st, io);
    update_macro(st, io);
    bool pointer_screen = pceui::pointer_screen(st) && !st.scope && !st.camera_rune;
    if (pointer_screen) {
        // only when the screen really offers something to point at (no cursor for a stale "shown" flag)
        const pceui::Pointer& probe = pceui::pointer();
        pointer_screen = probe.has_map || probe.has_box_cursor || !probe.targets.empty();
    }
    {
        static bool was = false;
        if (g_log && pointer_screen != was)
            LOG("pc menus: pointer %s (inv=%d map=%d title=%d dialog=%d choice=%d) screens=[%s]", pointer_screen ? "on" : "off",
                st.inventory, st.map, st.title, st.dialog, st.choice, pceui::shown_screens().c_str());
        was = pointer_screen;
    }
    if (!pointer_screen) {
        reset_pointer();
        pceui::set_inventory_snap(false);
        io.hold_add |= g_nav.step() | g_confirm.step();
        return;
    }
    const pceui::Pointer& ps = pceui::pointer();
    io.pointer_mode = true;

    const int sel = ps.selected();
    // world map: drag / click, unless a sub menu (stamp box, travel prompt, ...) has the box cursor
    const bool map_mode = st.map && ps.has_map && !(ps.has_box_cursor && sel >= 0);
    if (map_mode) update_map(ps, io);
    else g_map_cursor_frames = 0;
    const uint32_t sel_group = sel >= 0 ? ps.targets[sel].group : 0;
    if (!map_mode) {
        if (g_last_mx >= 0.0f && std::fabs(io.mouse_x - g_last_mx) + std::fabs(io.mouse_y - g_last_my) > 0.002f) {
            if (g_frame - g_last_move_frame >= 20) g_hover_done_part = 0;  // moves again after a rest: hover selects again
            g_last_move_frame = g_frame;
        }
        g_last_mx = io.mouse_x, g_last_my = io.mouse_y;
        const int hover = io.mouse_inside ? ps.at(io.mouse_x, io.mouse_y, sel_group) : -1;
        // inventory: the "<" / ">" next to the item grid turn the page (not while an item's action list is open)
        int arrow = 0;
        if (st.inventory && io.mouse_inside && (sel < 0 || sel_group != 0))
            for (int k = 0; k < 2; k++) {
                const pceui::Rect& r = ps.page_arrow_rect[k];
                if (ps.page_arrow[k] && io.mouse_x >= r.x0 - 0.012f && io.mouse_x <= r.x1 + 0.012f &&
                    io.mouse_y >= r.y0 - 0.02f && io.mouse_y <= r.y1 + 0.02f)
                    arrow = k == 0 ? -1 : 1;
            }
        if (io.lmb_pressed && arrow != 0) {
            if (g_tab_target < 0 && std::abs(g_tab_steps) < 3) g_tab_steps += arrow;
        } else if (io.lmb_pressed && hover >= 0) {
            if (ps.targets[hover].tab >= 0) {
                // category tab: the inventory jumps straight to it, the adventure log switches with the right stick
                if (ps.current_tab >= 0) {
                    g_wheel_pages = g_pager_wheel = 0;
                    g_smooth = false;
                    if (st.inventory && !ps.quest_tabs) jump_tab(ps.targets[hover].tab);
                    else request_tab(ps.targets[hover].tab, ps.current_tab);
                }
            } else {
                const pceui::Target& tg = ps.targets[hover];
                g_pending_part = tg.part;
                g_pending_frame = g_frame;
                // the "<" / ">" of an option row step the value (d-pad left / right) instead of confirming
                g_pending_btn = BTN_A;
                for (int k = 0; k < 2; k++)
                    if (tg.has_arrow[k] && io.mouse_x >= tg.arrow[k].x0 - 0.01f && io.mouse_x <= tg.arrow[k].x1 + 0.01f &&
                        io.mouse_y >= tg.arrow[k].y0 - 0.02f && io.mouse_y <= tg.arrow[k].y1 + 0.02f)
                        g_pending_btn = k == 0 ? BTN_LEFT : BTN_RIGHT;
                // option rows have no arrow panes of their own: a click on the outer ends of a value box (the
                // "<" / ">" spots) steps it. Plain menu buttons ignore d-pad left/right; the inventory/map grids don't.
                const float bw = tg.rect.x1 - tg.rect.x0;
                if (g_pending_btn == BTN_A && !st.inventory && !st.map && bw > 0.12f && bw < 0.4f) {
                    if (io.mouse_x < tg.rect.x0 + bw * 0.13f) g_pending_btn = BTN_LEFT;
                    else if (io.mouse_x > tg.rect.x1 - bw * 0.13f) g_pending_btn = BTN_RIGHT;
                }
                if (g_log)
                    LOG("pc menus: click part hit=[%.3f %.3f %.3f %.3f] mouse=(%.3f %.3f) arrows=%d%d -> %s", tg.rect.x0,
                        tg.rect.y0, tg.rect.x1, tg.rect.y1, io.mouse_x, io.mouse_y, tg.has_arrow[0], tg.has_arrow[1],
                        g_pending_btn == BTN_LEFT ? "left" : g_pending_btn == BTN_RIGHT ? "right" : "A");
                if (g_log) LOG("pc menus: click part panes=[%s]", tg.names.c_str());
            }
        }
        if (io.rmb_pressed && !g_confirm.busy()) g_confirm.start(BTN_B);
        if (st.inventory) {
            if (io.lmb_pressed && hover < 0 && arrow == 0) g_rot_drag = true;
            if (!io.lmb_down) g_rot_drag = false;
            if (g_rot_drag || io.mmb_down) {
                io.override_r = true;
                io.rx = std::clamp((io.mouse_x - g_rot_last_x) * 90.0f, -1.0f, 1.0f);
                io.ry = 0;
            }
        } else {
            g_rot_drag = false;
        }
        g_rot_last_x = io.mouse_x;
        if (io.wheel != 0) {
            // adventure log: the wheel scrolls the quest list, and switches categories only over their icons
            bool over_tabs = !ps.quest_tabs;
            for (auto& t : ps.targets)
                over_tabs |= t.tab >= 0 && io.mouse_y >= t.rect.y0 - 0.03f && io.mouse_y <= t.rect.y1 + 0.05f;
            const int n = std::abs(io.wheel), dir = io.wheel > 0 ? -1 : 1;
            if (ps.tab_count > 0 && ps.current_tab >= 0 && over_tabs && st.inventory && !ps.quest_tabs) {
                if (g_tab_target < 0) g_wheel_pages = std::clamp(g_wheel_pages + dir * n, -12, 12);
            } else if (ps.tab_count > 0 && ps.current_tab >= 0 && over_tabs && ps.quest_tabs) {
                const int base = g_tab_target >= 0 ? g_tab_target : ps.current_tab;
                request_tab(std::clamp(base + dir * std::min(3, n), 0, ps.tab_count - 1), ps.current_tab);
            } else if (ps.tab_count > 0 && ps.current_tab >= 0 && over_tabs) {
                if (g_tab_target < 0) g_pager_wheel = std::clamp(g_pager_wheel + dir * n, -12, 12);
            } else if (!g_nav.busy()) {
                g_nav.start(io.wheel > 0 ? BTN_UP : BTN_DOWN, 2, 3);
            }
        }
    }
    // the last step of a category jump slides in
    if (const int d = pceui::take_inventory_flick(); d != 0) {
        g_smooth = true;
        g_tab_steps = d;
        g_tab_phase = 0;
        g_smooth_until = g_frame + 30;
    }
    // shops and other screens with category tabs: one flick per queued wheel notch
    if (g_pager_wheel != 0 && g_tab_steps == 0 && g_tab_target < 0) {
        const int dir = g_pager_wheel > 0 ? 1 : -1;
        g_tab_steps = dir;
        g_pager_wheel -= dir;
    }
    // inventory wheel pages, once the previous slide has ended: one notch slides one page; several notches jump over
    // the pages in between and only the last one slides in
    if (g_wheel_pages != 0 && g_tab_steps == 0 && g_tab_target < 0 && g_frame >= g_wheel_next_at &&
        !pceui::inventory_sliding() && !pceui::inventory_jump_pending()) {
        g_smooth = true;
        g_wheel_next_at = g_frame + 4;
        g_smooth_until = g_frame + 30;
        if (std::abs(g_wheel_pages) >= 2) pceui::queue_inventory_jump_pages(g_wheel_pages);
        else g_tab_steps = g_wheel_pages;
        g_wheel_pages = 0;
    } else if (g_smooth && g_wheel_pages == 0 && g_tab_steps == 0 && g_frame > g_smooth_until && !pceui::inventory_sliding()) {
        g_smooth = false;
    }
    // a category burst (adventure log, a jump that missed) applies its page changes at once; wheel steps slide
    pceui::set_inventory_snap(g_tab_target >= 0 && !g_smooth);
    // one right-stick flick per page; a category request is checked afterwards and continued until reached
    if (g_tab_steps != 0) {
        const int dir = g_tab_steps > 0 ? 1 : -1;
        if (g_tab_phase == 0) {
            io.override_r = true;
            io.rx = (float)dir;
            io.ry = 0;
        }
        if (++g_tab_phase >= 2) {
            g_tab_phase = 0;
            g_tab_steps -= dir;
            g_tab_verify_at = g_frame + (g_smooth ? 30 : (st.inventory ? kTabCheckFrames : 1));
        }
    } else if (g_tab_target >= 0 && g_frame >= g_tab_verify_at && !pceui::inventory_sliding() &&
               !pceui::inventory_jump_pending()) {
        if (ps.current_tab < 0 || ps.current_tab == g_tab_target || g_frame > g_tab_until || g_tab_retries >= 40)
            g_tab_target = -1;
        else {
            g_tab_steps = g_tab_target - ps.current_tab;  // categories with several pages: go on until reached
            g_tab_retries++;
        }
    }

    // move the selection to the pending click target, or to the hovered entry while the mouse moves
    if (sel != g_last_selected) {
        g_nav_wait_until = std::min(g_nav_wait_until, g_frame + 2);
        g_last_selected = sel;
    }
    int desired = -1;
    if (g_pending_part) {
        desired = find_part(ps, g_pending_part);
        if (desired < 0 || g_frame - g_pending_frame > 240) g_pending_part = 0;
    } else if (!map_mode && io.mouse_inside && g_tab_target < 0 && g_tab_steps == 0 &&
               (g_frame - g_last_move_frame < 20 || (g_hover_chase && g_frame - g_last_move_frame < 90))) {
        // the selection follows the mouse while it moves, until it arrived on the entry the mouse stopped on
        desired = ps.at(io.mouse_x, io.mouse_y, sel_group);
        if (desired >= 0 && ps.targets[desired].tab >= 0) desired = -1;  // hovering a tab does not switch categories
        if (desired < 0 || ps.targets[desired].part != g_hover_done_part) g_hover_done_part = 0;
        else desired = -1;
    }
    bool reached = desired >= 0 && desired == sel;
    if (desired >= 0 && !reached) {
        const uint32_t part = ps.targets[desired].part;
        if (part != g_focus_part) g_focus_part = part, g_focus_since = g_frame;
        // the game's cursor jumps straight onto the entry; d-pad steps where that is not possible or has no effect
        const pceui::Focus direct = pceui::request_focus(part, ps.targets[desired].hit);
        if (direct == pceui::Focus::Focused) reached = true;
        else if (sel >= 0 && (direct == pceui::Focus::Unavailable || g_frame - g_focus_since > 6))
            navigate(ps, sel, desired);
    } else {
        g_focus_part = 0;
    }
    if (reached) {
        if (!g_pending_part) g_hover_done_part = ps.targets[desired].part;
        else if (!g_confirm.busy() && !g_nav.busy()) {
            g_confirm.start(g_pending_btn);
            g_pending_part = 0;
        }
    }
    g_hover_chase = !g_pending_part && desired >= 0 && !reached;
    io.hold_add |= g_nav.step() | g_confirm.step();
}
}  // namespace pcmenus
