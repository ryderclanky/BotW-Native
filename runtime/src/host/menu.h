// In-game settings menu: opened with the DualSense touchpad, L3+R3 or F1. Navigation runs on the host
// main thread (window.cpp); runtime/cemu_shims/overlay_menu.cpp draws a snapshot on the GPU thread.
// The settings are grouped in tabs (Graphics, Controls, Keys, Audio, Mods, About). Every tab ends with a Close row.
#pragma once
#include <string>
#include <vector>

namespace menu {
bool is_open();
void toggle();
void close();
void move(int dy);      // previous / next row
void tab(int dir);      // previous / next tab (controller L1 / R1, keyboard Tab and PageUp / PageDown)
void select_tab(int i); // touch / mouse: jump to a tab
void change(int dir);   // left / right on the selected row
void activate();        // confirm button
void reorder_mod(int delta);  // U/D or controller X/Y in Mods tab: move selected mod up (-1) or down (+1)
void reload_mods();           // R in Mods tab: apply & reload mod overlay immediately
// Keys tab: Enter on an action waits for the next key / mouse button / wheel notch (window.cpp sends it here).
bool listening();
void cancel_listen();
void bind(int type, int code);  // type as pccontrols::BoundKey: 0 none, 1 scancode, 2 mouse button, 3/4 wheel up/down

struct Row {
    std::string label, value;
    bool action = false;    // a button, not a setting
    bool readonly = false;  // a display-only value row, no left/right arrows
};
struct View {
    bool open = false;
    int tab = 0;                     // index into tabs
    int cursor = 0;                  // selected index into rows (the current tab's rows, Close is last)
    std::vector<std::string> tabs;   // tab titles
    std::vector<Row> rows;           // rows of the current tab
    std::vector<std::string> text;   // read-only lines of the current tab (About)
    std::string note;                // short remark under the rows
};
View view();                    // snapshot for drawing
void click(int row, int dir);   // touch / mouse: dir -1/+1 on a row's arrows, 0 on the row itself
// Pointer (mouse / touch) in drawable pixels, from the host event loop, for the overlay's ImGui.
void pointer(float x, float y, bool down);
void pointer_state(float* x, float* y, bool* down);
// Mouse wheel notches while the menu is open (scrolls the long tabs); the overlay takes them each frame.
void wheel(float y);
float take_wheel();
// Touch: the overlay records where it drew the panel and its X button (fractions of the screen, x0 y0 x1 y1).
// A tap outside the panel or on the X closes the menu; touch_close() returns true when it did.
void set_rects(const float panel[4], const float close_button[4]);
void rects(float panel[4], float close_button[4]);
bool touch_close(float x, float y);
// Screen insets that overlays must keep clear of (rounded corners, Dynamic Island, home indicator), pixels.
void set_safe_area(float left, float top, float right, float bottom);
void safe_area(float* left, float* top, float* right, float* bottom);
}  // namespace menu
