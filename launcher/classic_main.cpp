// Small Win32 launcher for BotWRecomp. It is not linked into the game. SDL3 is only used to
// name keys and controller buttons the same way the game reads controls.ini.
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include "game_info.h"
#include "input/controls_file.h"
#include "mods.h"
#include "save_import.h"
#include "setup_wizard.h"
#include "ui.h"
#include "unzip.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#ifndef BIF_NONEWFOLDERBUTTON
#define BIF_NONEWFOLDERBUTTON 0x00000200
#endif
#ifndef BIF_BROWSEINCLUDEFILES
#define BIF_BROWSEINCLUDEFILES 0x00004000
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

using ui::S;

namespace {

const wchar_t* kAppTitle = L"BotW Recomp Launcher";

// Child ids start at 100 so IDOK and IDCANCEL stay free for Enter and Esc.
enum {
    ID_PLAY = 100,
    ID_SETTINGS,
    ID_CONTROLS_OPEN,
    ID_SAVES_OPEN,
    ID_MODS_OPEN,
    ID_USER_OPEN,
    ID_LOG_OPEN,
    ID_GAME,
    ID_GAME_BROWSE,
    ID_EXE,
    ID_EXE_BROWSE,
    ID_USER,
    ID_USER_BROWSE,
    ID_SECONDS,
    ID_FULLSCREEN,
    ID_GAMEPAD_VIEW,
    ID_SAVE_SETTINGS,
    ID_CANCEL_SETTINGS,
    ID_CONTROLS,
    ID_SET_KEY,
    ID_SET_PAD,
    ID_RESET,
    ID_CLOSE,
    ID_SAVES,
    ID_IMPORT,
    ID_SWITCH,
    ID_DELETE,
    ID_OPEN,
    ID_REFRESH,
    ID_MIGRATE,
    ID_MODS,
    ID_MOD_INSTALL,
    ID_MOD_TOGGLE,
    ID_MOD_UP,
    ID_MOD_DOWN,
    ID_MODS_FOLDER,
    ID_MODS_REFRESH,
    ID_NO_MODS,
    ID_NO_DLC,
    ID_MOD_PRESET_BASE = 200,
    ID_MOD_PRESET_MAX = 231,
    ID_LOG_TEXT,
    ID_LOG_FILE,
    ID_SETUP_OPEN,
    // Setup Assistant controls
    ID_WIZ_BACK,
    ID_WIZ_NEXT,
    ID_WIZ_CANCEL,
    ID_WIZ_USER,
    ID_WIZ_USER_BROWSE,
    ID_WIZ_TOOLCHAIN_REFRESH,
    ID_WIZ_TOOLCHAIN_INSTALL,
    ID_WIZ_MODE_MERGED,
    ID_WIZ_MODE_SEPARATE,
    ID_WIZ_GAME,
    ID_WIZ_GAME_BROWSE,
    ID_WIZ_AUTODETECT,
    ID_WIZ_BASE,
    ID_WIZ_BASE_BROWSE,
    ID_WIZ_UPD,
    ID_WIZ_UPD_BROWSE,
    ID_WIZ_DLC,
    ID_WIZ_DLC_BROWSE,
    ID_WIZ_MERGE,
    ID_WIZ_BUILD_START,
    ID_WIZ_BUILD_CANCEL,
    ID_WIZ_EXE_BROWSE,
    ID_WIZ_LOG_EDIT,
    ID_WIZ_SCALE_1X,
    ID_WIZ_SCALE_2X,
    ID_WIZ_SCALE_3X,
    ID_WIZ_FPS_30,
    ID_WIZ_FPS_60,
    ID_WIZ_LAYOUT_AUTO,
    ID_WIZ_LAYOUT_XBOX,
    ID_WIZ_LAYOUT_NINTENDO,
    ID_WIZ_FULLSCREEN,
    ID_WIZ_GAMEPAD_VIEW,
    ID_WIZ_GYRO,
    ID_WIZ_IMPORT_SAVE,
};

struct Settings {
    std::wstring game, exe, user, seconds = L"0", theme = L"auto", language;
    bool fullscreen = false;
    int window_mode = 0;
    bool gamepad_view = false;
    bool no_mods = false;
    bool no_dlc = false;
    bool setup_done = false;
    std::vector<std::string> other;  // lines this version does not know, written back as they were
};

enum class Tone { Normal, Good, Warn, Error };

struct Window;
struct SavesWindow;

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    Settings cfg;
    gameinfo::Info info;
    mods::State mods;
    std::wstring status;
    Tone tone = Tone::Normal;
    bool sdl = false;
    bool in_capture = false;
    bool was_running = false;
    std::vector<SDL_Gamepad*> pads;
    std::vector<controlsfile::Binding> rows;
    std::vector<saveimport::SetInfo> sets;
    SavesWindow* saves_win = nullptr;
};

App g;

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(wcslen(s.c_str()));
    return s;
}

void set_text(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

HWND alert_owner() {
    HWND a = GetActiveWindow();
    return a ? a : g.hwnd;
}

void alert(const std::wstring& text, UINT flags = MB_OK | MB_ICONWARNING) { MessageBoxW(alert_owner(), text.c_str(), kAppTitle, flags); }

int ask(const std::wstring& text, UINT flags) { return MessageBoxW(alert_owner(), text.c_str(), kAppTitle, flags); }

std::filesystem::path exe_dir() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (!n || n >= 32768) return {};
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path launcher_ini() { return exe_dir() / L"BotWLauncher.ini"; }

std::wstring appdata_root() {
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf) != S_OK) return L"BotWRecomp";
    return std::wstring(buf) + L"\\BotWRecomp";
}

std::filesystem::path user_root() {
    if (g.cfg.user.empty()) return {};
    return std::filesystem::path(g.cfg.user);
}

bool has_quote(const std::wstring& s) { return s.find(L'"') != std::wstring::npos; }

std::wstring pick_browse(HWND owner, const wchar_t* title, bool files_ok) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON | BIF_EDITBOX;
    if (files_ok) bi.ulFlags |= BIF_BROWSEINCLUDEFILES;
    else bi.ulFlags |= BIF_RETURNONLYFSDIRS;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t path[32768];
    path[0] = 0;
    using ExFn = BOOL(WINAPI*)(PCIDLIST_ABSOLUTE, PWSTR, DWORD, DWORD);
    FARPROC raw = GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetPathFromIDListEx");
    ExFn ex = nullptr;
    memcpy(&ex, &raw, sizeof ex);
    BOOL ok = ex ? ex(pidl, path, 32768, 0) : SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? std::wstring(path) : std::wstring();
}

std::wstring pick_exe(HWND owner, const std::wstring& cur) {
    wchar_t buf[32768];
    buf[0] = 0;
    if (cur.size() < 32767) wcsncpy(buf, cur.c_str(), 32767);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Programs\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"BotWRecomp.exe";
    if (!GetOpenFileNameW(&ofn)) return {};
    return buf;
}

bool process_running(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool game_running() { return process_running(L"BotWRecomp.exe"); }

bool read_user_gamepad_view(const std::filesystem::path& root, bool fallback) {
    if (root.empty()) return fallback;
    std::ifstream in(root / "settings.ini");
    if (!in) return fallback;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("gamepad_view=", 0) == 0) return line.substr(13) != "0";
    }
    return fallback;
}

void write_user_gamepad_view(const std::filesystem::path& root, bool on) {
    if (root.empty()) return;
    std::filesystem::path p = root / "settings.ini";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return;
    std::ifstream in(p);
    if (!in) return;
    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("gamepad_view=", 0) == 0) {
            lines.push_back(std::string("gamepad_view=") + (on ? "1" : "0"));
            found = true;
        } else {
            lines.push_back(line);
        }
    }
    in.close();
    if (!found) lines.push_back(std::string("gamepad_view=") + (on ? "1" : "0"));
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& l : lines) out << l << "\n";
}

void save_launcher_ini() {
    std::ofstream out(launcher_ini(), std::ios::binary | std::ios::trunc);
    if (!out) return;
    auto line = [&](const char* k, const std::wstring& v) { out << k << "=" << narrow(v) << "\n"; };
    line("game", g.cfg.game);
    line("exe", g.cfg.exe);
    line("user", g.cfg.user);
    line("seconds", g.cfg.seconds);
    line("fullscreen", g.cfg.fullscreen ? L"1" : L"0");
    line("window_mode", std::to_wstring(g.cfg.window_mode));
    line("gamepad_view", g.cfg.gamepad_view ? L"1" : L"0");
    line("no_mods", g.cfg.no_mods ? L"1" : L"0");
    line("no_dlc", g.cfg.no_dlc ? L"1" : L"0");
    line("language", g.cfg.language);
    line("setup_done", g.cfg.setup_done ? L"1" : L"0");
    line("theme", g.cfg.theme);
    for (auto& l : g.cfg.other) out << l << "\n";
}

void load_launcher_ini() {
    std::ifstream in(launcher_ini());
    Settings s;
    s.user = appdata_root();
    std::string line;
    bool saw_gpv = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "game") s.game = widen(v);
        else if (k == "exe") s.exe = widen(v);
        else if (k == "user") {
            if (!v.empty()) s.user = widen(v);
        } else if (k == "seconds") s.seconds = widen(v);
        else if (k == "fullscreen") { s.fullscreen = v == "1"; s.window_mode = s.fullscreen ? 1 : 0; }
        else if (k == "window_mode") { s.window_mode = std::clamp(atoi(v.c_str()), 0, 2); s.fullscreen = s.window_mode != 0; }
        else if (k == "gamepad_view") { s.gamepad_view = v == "1"; saw_gpv = true; }
        else if (k == "no_mods") s.no_mods = v == "1";
        else if (k == "no_dlc") s.no_dlc = v == "1";
        else if (k == "language") s.language = widen(v);
        else if (k == "setup_done") s.setup_done = v == "1";
        else if (k == "theme") s.theme = widen(v);
        else s.other.push_back(line);
    }
    s.gamepad_view = read_user_gamepad_view(std::filesystem::path(s.user), saw_gpv ? s.gamepad_view : false);
    if (s.exe.empty()) {
        auto guess = exe_dir() / L"BotWRecomp.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(guess, ec)) s.exe = guess.wstring();
    }
    if (s.seconds.empty()) s.seconds = L"0";
    g.cfg = s;
}

ui::Theme theme_pref() {
    if (_wcsicmp(g.cfg.theme.c_str(), L"dark") == 0) return ui::Theme::Dark;
    if (_wcsicmp(g.cfg.theme.c_str(), L"light") == 0) return ui::Theme::Light;
    return ui::Theme::Auto;
}

// Every window here is one Window object behind one window procedure. Dialogs are modal.
struct Window {
    HWND hwnd = nullptr;
    bool is_main = false;
    int bar = 0;  // bottom button bar height at 96 dpi, 0 for none
    int min_w = 0, min_h = 0;
    virtual ~Window() = default;
    virtual void create() = 0;
    virtual void layout(int w, int h) = 0;
    virtual void paint(HDC, const RECT&) {}
    virtual void command(int, int, HWND) {}
    virtual LRESULT notify(NMHDR*) { return 0; }
    virtual void ok() {}
    virtual void cancel() { close(); }
    virtual void on_close() { cancel(); }
    virtual void dropped(const std::wstring&) {}
    virtual void timer() {}
    void close() {
        if (is_main) DestroyWindow(hwnd);
        else ui::end_modal(hwnd);
    }
    void relayout() {
        if (!hwnd) return;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        layout(rc.right, rc.bottom);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
};

void on_theme_change() {
    if (ui::refresh_theme()) ui::retheme_all();
}

void apply_dpi_everywhere(int dpi) {
    if (dpi == ui::dpi()) return;
    ui::set_dpi(dpi);
    ui::rebuild_fonts();
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND w, LPARAM) -> BOOL {
            ui::set_fonts(w);
            return TRUE;
        },
        0);
}

enum class SetupPreset {
    None,
    Step1,
    Step2,
    Step3Idle,
    Step3Recomp,
    Step3Compile,
    Step4,
};
void show_setup_wizard(SetupPreset preset = SetupPreset::None);
void show_settings();
void show_controls();
void show_saves();
void show_mods();
void show_log();

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        w = static_cast<Window*>(cs->lpCreateParams);
        w->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
    }
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CREATE:
            ui::apply_frame(hwnd);
            w->create();
            ui::set_fonts(hwnd);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            ui::PaintBuffer pb(hwnd);
            ui::fill(pb.dc, pb.rc, ui::pal.window);
            if (w->bar) {
                RECT b = pb.rc;
                b.top = b.bottom - S(w->bar);
                ui::fill(pb.dc, b, ui::pal.panel);
                ui::hline(pb.dc, 0, b.right, b.top, ui::pal.line);
            }
            ui::paint_frames(hwnd, pb.dc);
            w->paint(pb.dc, pb.rc);
            return 0;
        }
        case WM_SIZE:
            w->layout(LOWORD(lp), HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            if (!w->min_w) break;
            RECT r{0, 0, S(w->min_w), S(w->min_h)};
            AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_DPICHANGED: {
            apply_dpi_everywhere(HIWORD(wp));
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            w->relayout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) on_theme_change();
            break;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE: on_theme_change(); break;
        case WM_DRAWITEM:
            if (ui::draw_item(reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSTATIC: return ui::ctl_color(msg, wp, lp);
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
            if (id == IDOK && !ctl) {
                HWND f = GetFocus();
                if (f && GetParent(f) == hwnd && ui::is_button(f)) SendMessageW(f, BM_CLICK, 0, 0);
                else w->ok();
                return 0;
            }
            if (id == IDCANCEL && !ctl) {
                w->cancel();
                return 0;
            }
            if (ctl && code == BN_CLICKED && ui::is_check(ctl)) ui::set_checked(ctl, !ui::checked(ctl));
            w->command(id, code, ctl);
            return 0;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (LRESULT r = ui::list_custom_draw(hdr)) return r;
            return w->notify(hdr);
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t path[32768];
            std::wstring p;
            if (DragQueryFileW(drop, 0, path, 32768)) p = path;
            DragFinish(drop);
            if (!p.empty()) w->dropped(p);
            return 0;
        }
        case WM_TIMER: w->timer(); return 0;
        case WM_APP + 10: {
            char* s = reinterpret_cast<char*>(lp);
            std::string arg = s ? s : "";
            free(s);
            if (arg == "setup" || arg == "setup1") show_setup_wizard(SetupPreset::Step1);
            else if (arg == "setup2") show_setup_wizard(SetupPreset::Step2);
            else if (arg == "setup3") show_setup_wizard(SetupPreset::Step3Idle);
            else if (arg == "setup3-recomp") show_setup_wizard(SetupPreset::Step3Recomp);
            else if (arg == "setup3-compile") show_setup_wizard(SetupPreset::Step3Compile);
            else if (arg == "setup4") show_setup_wizard(SetupPreset::Step4);
            else if (arg == "settings") show_settings();
            else if (arg == "controls") show_controls();
            else if (arg == "saves") show_saves();
            else if (arg == "mods") show_mods();
            else if (arg == "log") show_log();
            return 0;
        }
        case WM_CLOSE: w->on_close(); return 0;
        case WM_DESTROY:
            if (w->is_main) {
                KillTimer(hwnd, 1);
                PostQuitMessage(0);
            }
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            w->hwnd = nullptr;
            break;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

const wchar_t* kMainClass = L"BotWLauncher";
const wchar_t* kDialogClass = L"BotWLauncherDialog";

void register_classes() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kDialogClass;
    RegisterClassExW(&wc);
}

HWND make_window(Window* w, const wchar_t* cls, const wchar_t* title, int cw, int ch, bool resizable, HWND owner) {
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    if (resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    if (!owner) style |= WS_MINIMIZEBOX;
    DWORD ex = WS_EX_CONTROLPARENT | (owner ? WS_EX_DLGMODALFRAME : 0);
    RECT r{0, 0, S(cw), S(ch)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    HWND h = CreateWindowExW(ex, cls, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, owner, nullptr, g.inst, w);
    if (h && owner) ui::center_on(h, owner);
    return h;
}

void run_dialog(Window* w, const wchar_t* title, int cw, int ch, bool resizable) {
    HWND owner = GetActiveWindow();
    if (!owner) owner = g.hwnd;
    HWND h = make_window(w, kDialogClass, title, cw, ch, resizable, owner);
    if (!h) return;
    ui::run_modal(h, owner);
}

void add_columns(HWND list, const wchar_t** names, int n) {
    for (int i = 0; i < n; i++) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(names[i]);
        col.cx = 120;
        col.iSubItem = i;
        ListView_InsertColumn(list, i, &col);
    }
}

// Buttons left to right from x.
int place_row(std::initializer_list<HWND> buttons, int x, int y, int h, int gap) {
    for (HWND b : buttons) {
        int bw = ui::button_width(b);
        MoveWindow(b, x, y, bw, h, TRUE);
        x += bw + gap;
    }
    return x;
}

// Buttons right to left from right; the first one ends up rightmost.
int place_row_right(std::initializer_list<HWND> buttons, int right, int y, int h, int gap) {
    for (HWND b : buttons) {
        int bw = ui::button_width(b);
        MoveWindow(b, right - bw, y, bw, h, TRUE);
        right -= bw + gap;
    }
    return right;
}

int text_height(HFONT f, const std::wstring& text, int width) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return r.bottom;
}

void paint_lines(HDC dc, const std::wstring& text, RECT r, HFONT f, COLORREF c) {
    ui::draw_text(dc, text, r, f, c, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

void set_status(const std::wstring& s, Tone t = Tone::Normal) {
    g.status = s;
    g.tone = t;
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

void update_play_button();

void refresh_game() {
    g.info = gameinfo::read(std::filesystem::path(g.cfg.game));
    g.mods = mods::load(user_root());
    const auto& i = g.info;
    std::error_code ec;
    bool exe_ok = !g.cfg.exe.empty() && std::filesystem::is_regular_file(std::filesystem::path(g.cfg.exe), ec);
    if (g.cfg.game.empty()) set_status(L"Choose the folder with your game files (code, content and meta) to get started.");
    else if (!i.folder_exists) set_status(L"Game folder not found. Choose it again in Settings.", Tone::Error);
    else if (!i.has_rpx) set_status(L"No code\\U-King.rpx in the game folder. Pick the folder that has code, content and meta.", Tone::Error);
    else if (!exe_ok) set_status(L"BotWRecomp.exe not found. Set it in Settings.", Tone::Error);
    else if (game_running()) set_status(L"BotWRecomp.exe is running. Save import, switch, delete, and migrate are blocked until it closes.");
    else if (saveimport::decimal_legacy_present(user_root()))
        set_status(L"A save from an older build is in save\\2147483649. Open Saves and use Migrate old save.", Tone::Warn);
    else if (!i.has_meta || !i.has_title_version) set_status(L"Game folder found. No meta\\meta.xml, so the update version was not checked.", Tone::Good);
    else if (i.title_version == gameinfo::kExpectedTitleVersion) set_status(L"Game folder found. Update v1.5.0 detected.", Tone::Good);
    else
        set_status(L"Game folder found, but it has update " + widen(gameinfo::version_name(i.title_version)) +
                       L". This build needs v1.5.0.",
                   Tone::Warn);
    update_play_button();
}

// Lists live in their dialogs. These keep g.rows and g.sets current and refresh any open list.
void fill_saves();
int selected_set();
void note_legacy_save();

// Controls capture: a small SDL window takes the next key or button.
const char* token_for_button(SDL_GamepadButton b) {
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
        case SDL_GAMEPAD_BUTTON_EAST: return "east";
        case SDL_GAMEPAD_BUTTON_WEST: return "west";
        case SDL_GAMEPAD_BUTTON_NORTH: return "north";
        case SDL_GAMEPAD_BUTTON_BACK: return "back";
        case SDL_GAMEPAD_BUTTON_START: return "start";
        case SDL_GAMEPAD_BUTTON_GUIDE: return "guide";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "leftstick";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "rightstick";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "leftshoulder";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "rightshoulder";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpup";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpdown";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpleft";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpright";
        case SDL_GAMEPAD_BUTTON_MISC1: return "misc1";
        case SDL_GAMEPAD_BUTTON_MISC2: return "misc2";
        case SDL_GAMEPAD_BUTTON_MISC3: return "misc3";
        case SDL_GAMEPAD_BUTTON_MISC4: return "misc4";
        case SDL_GAMEPAD_BUTTON_MISC5: return "misc5";
        case SDL_GAMEPAD_BUTTON_MISC6: return "misc6";
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "touchpad";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "paddle1";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "paddle2";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "paddle3";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "paddle4";
        default: return nullptr;
    }
}

const char* token_for_axis(int axis, Sint16 value) {
    switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: return value > 0 ? "leftx+" : "leftx-";
        case SDL_GAMEPAD_AXIS_LEFTY: return value < 0 ? "lefty+" : "lefty-";
        case SDL_GAMEPAD_AXIS_RIGHTX: return value > 0 ? "rightx+" : "rightx-";
        case SDL_GAMEPAD_AXIS_RIGHTY: return value < 0 ? "righty+" : "righty-";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "lefttrigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "righttrigger";
        default: return nullptr;
    }
}

void remember_pad(SDL_JoystickID id) {
    for (auto* p : g.pads)
        if (SDL_GetGamepadID(p) == id) return;
    if (SDL_Gamepad* p = SDL_OpenGamepad(id)) g.pads.push_back(p);
}

void forget_pad(SDL_JoystickID id) {
    for (auto it = g.pads.begin(); it != g.pads.end(); ++it) {
        if (SDL_GetGamepadID(*it) == id) {
            SDL_CloseGamepad(*it);
            g.pads.erase(it);
            return;
        }
    }
}

void pump_sdl() {
    if (!g.sdl || g.in_capture) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) remember_pad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) forget_pad(e.gdevice.which);
    }
}

enum class CaptureSide { Key, Pad };

bool capture_binding(HWND owner, int row, CaptureSide side) {
    if (!g.sdl) {
        alert(L"SDL did not start, so keys and controller buttons cannot be captured.");
        return false;
    }
    const auto* action = controlsfile::find_action(g.rows[static_cast<size_t>(row)].id);
    std::string label = action ? action->label : g.rows[static_cast<size_t>(row)].id;
    std::string title = std::string(side == CaptureSide::Key ? "Key for " : "Controller for ") + label + "  -  Esc cancels, Backspace clears";
    g.in_capture = true;
    EnableWindow(owner, FALSE);
    SDL_Window* sw = SDL_CreateWindow(title.c_str(), 640, 180, SDL_WINDOW_UTILITY | SDL_WINDOW_ALWAYS_ON_TOP);
    SDL_Renderer* renderer = sw ? SDL_CreateRenderer(sw, nullptr) : nullptr;
    if (sw) SDL_SetWindowPosition(sw, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    const char* line1 = side == CaptureSide::Key ? "Press a key. Esc cancels. Backspace clears."
                                                  : "Press a button, trigger, or stick. Esc cancels. Backspace clears.";
    const char* line2 = "Release it first if you are already holding it.";
    bool done = false, accept = false, clear = false;
    std::string key, pad;
    struct Arm {
        SDL_Gamepad* pad = nullptr;
        bool axis_ready[8]{};
        bool button_was[32]{};
        bool primed = false;
    };
    std::vector<Arm> arms;
    while (!done) {
        if (renderer) {
            SDL_SetRenderDrawColor(renderer, 24, 26, 32, 255);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawColor(renderer, 236, 236, 236, 255);
            SDL_RenderDebugText(renderer, 16, 24, line1);
            SDL_RenderDebugText(renderer, 16, 48, line2);
            SDL_RenderPresent(renderer);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED) remember_pad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) forget_pad(e.gdevice.which);
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) done = true;
                else if (e.key.scancode == SDL_SCANCODE_BACKSPACE) {
                    clear = true;
                    done = true;
                } else if (side == CaptureSide::Key) {
                    const char* name = SDL_GetScancodeName(e.key.scancode);
                    if (name && name[0]) {
                        key = name;
                        accept = true;
                        done = true;
                    }
                }
            }
        }
        if (side == CaptureSide::Pad && !done) {
            for (auto* padp : g.pads) {
                Arm* arm = nullptr;
                for (auto& a : arms)
                    if (a.pad == padp) arm = &a;
                if (!arm) {
                    arms.push_back({});
                    arm = &arms.back();
                    arm->pad = padp;
                }
                if (!arm->primed) {
                    arm->primed = true;
                    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++)
                        arm->button_was[b] = SDL_GetGamepadButton(padp, static_cast<SDL_GamepadButton>(b));
                    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && a < 8; a++) {
                        int v = SDL_GetGamepadAxis(padp, static_cast<SDL_GamepadAxis>(a));
                        if (v < 0) v = -v;
                        int center = (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ? 4000 : 8000;
                        arm->axis_ready[a] = v < center;
                    }
                    continue;
                }
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) {
                    bool down = SDL_GetGamepadButton(padp, static_cast<SDL_GamepadButton>(b));
                    if (down && !arm->button_was[b]) {
                        if (const char* tok = token_for_button(static_cast<SDL_GamepadButton>(b))) {
                            pad = tok;
                            accept = true;
                            done = true;
                        }
                    }
                    arm->button_was[b] = down;
                }
                if (done) break;
                for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && a < 8; a++) {
                    Sint16 v = SDL_GetGamepadAxis(padp, static_cast<SDL_GamepadAxis>(a));
                    int mag = v < 0 ? -v : v;
                    int center = (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ? 4000 : 8000;
                    if (mag < center) arm->axis_ready[a] = true;
                    else if (arm->axis_ready[a] && mag > 20000) {
                        if (const char* tok = token_for_axis(a, v)) {
                            pad = tok;
                            accept = true;
                            done = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!done) SDL_Delay(10);
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (sw) SDL_DestroyWindow(sw);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g.in_capture = false;
    if (clear) {
        if (side == CaptureSide::Key) {
            g.rows[static_cast<size_t>(row)].key.clear();
            g.rows[static_cast<size_t>(row)].key2.clear();
        } else {
            g.rows[static_cast<size_t>(row)].pad.clear();
        }
        return true;
    }
    if (!accept) return false;
    if (side == CaptureSide::Key) {
        g.rows[static_cast<size_t>(row)].key = key;
        g.rows[static_cast<size_t>(row)].key2.clear();
    } else {
        g.rows[static_cast<size_t>(row)].pad = pad;
    }
    return true;
}

// Save sets.
bool ensure_stopped() {
    if (!game_running()) return true;
    alert(L"BotWRecomp.exe is running. Close the game before changing saves.");
    return false;
}

bool have_user() {
    if (!user_root().empty()) return true;
    alert(L"Set the user data folder first. Saves and controls.ini go there.");
    return false;
}

void cleanup_temp(const std::filesystem::path& tmp) {
    if (tmp.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

std::filesystem::path make_temp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"botw-import-%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, GetCurrentProcessId());
    return std::filesystem::temp_directory_path() / name;
}

bool is_zip_path(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    auto ext = p.extension().wstring();
    for (wchar_t& c : ext)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return ext == L".zip";
}

void import_path(const std::wstring& picked) {
    if (picked.empty()) return;
    if (!have_user() || !ensure_stopped()) return;
    std::filesystem::path path(picked);
    std::filesystem::path tmp, root = path;
    std::string err;
    if (is_zip_path(path)) {
        tmp = make_temp();
        if (!unzip_to(path, tmp, err)) {
            alert(widen(err));
            cleanup_temp(tmp);
            return;
        }
        root = tmp;
    }
    saveimport::Located loc;
    if (!saveimport::locate(root, loc, err)) {
        alert(widen(err));
        cleanup_temp(tmp);
        return;
    }
    if (loc.dangerous || loc.caution || !loc.note.empty()) {
        std::wstring text;
        if (!loc.warnings.empty()) text += widen(loc.warnings);
        if (!loc.note.empty()) {
            if (!text.empty()) text += L"\n\n";
            text += widen(loc.note);
        }
        text += L"\n\nImport anyway? The current save is backed up first.";
        UINT def = loc.dangerous ? MB_DEFBUTTON2 : MB_DEFBUTTON1;
        if (MessageBoxW(alert_owner(), text.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING | def) != IDYES) {
            cleanup_temp(tmp);
            set_status(L"Import cancelled.");
            return;
        }
    }
    auto result = saveimport::import_account(user_root(), loc, narrow(picked));
    cleanup_temp(tmp);
    fill_saves();
    set_status(widen(result.message));
    alert(widen(result.message), result.ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

void on_migrate() {
    if (!have_user() || !ensure_stopped()) return;
    saveimport::Located loc;
    if (!saveimport::peek_decimal(user_root(), loc)) {
        alert(L"No save in save\\2147483649. That is the folder an older build used for account slot 1.");
        return;
    }
    std::wstring text =
        L"Copy save\\2147483649 to save\\80000001?\nThe old folder stays where it is.\nIf save\\80000001 already has files, it is backed up first.";
    if (!loc.warnings.empty()) text += L"\n\n" + widen(loc.warnings);
    UINT def = loc.dangerous ? MB_DEFBUTTON2 : MB_DEFBUTTON1;
    if (MessageBoxW(alert_owner(), text.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING | def) != IDYES) return;
    auto result = saveimport::migrate_decimal(user_root());
    fill_saves();
    note_legacy_save();
    set_status(widen(result.message));
    alert(widen(result.message), result.ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

void on_import() {
    std::wstring picked = pick_browse(alert_owner(), L"Select a save folder, a .zip, or option.sav / game_data.sav inside the save.", true);
    import_path(picked);
}

void on_switch() {
    int i = selected_set();
    if (i < 0) {
        alert(L"Select a save set first.");
        return;
    }
    if (!have_user() || !ensure_stopped()) return;
    std::wstring title = widen(g.sets[static_cast<size_t>(i)].title);
    std::wstring text = L"Switch to \"" + title + L"\"?\nThe current save is backed up first.";
    if (!g.sets[static_cast<size_t>(i)].warnings.empty()) text += L"\n\n" + widen(g.sets[static_cast<size_t>(i)].warnings);
    if (MessageBoxW(alert_owner(), text.c_str(), kAppTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    auto result = saveimport::switch_to(user_root(), g.sets[static_cast<size_t>(i)].dir);
    fill_saves();
    set_status(widen(result.message));
    alert(widen(result.message), result.ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

void on_delete() {
    int i = selected_set();
    if (i < 0) {
        alert(L"Select a save set first.");
        return;
    }
    if (!have_user() || !ensure_stopped()) return;
    std::wstring title = widen(g.sets[static_cast<size_t>(i)].title);
    if (MessageBoxW(alert_owner(), (L"Delete \"" + title + L"\"? This does not change the live save.").c_str(), kAppTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    auto result = saveimport::delete_set(user_root(), g.sets[static_cast<size_t>(i)].dir);
    fill_saves();
    set_status(widen(result.message));
    if (!result.ok) alert(widen(result.message));
}

void on_open_saves() {
    auto root = user_root();
    if (root.empty()) {
        alert(L"Set the user data folder first.");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(root / "save" / "80000001", ec);
    std::filesystem::create_directories(root / "save" / "common", ec);
    ShellExecuteW(alert_owner(), L"open", (root / "save").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring tail_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return L"No runtime.log yet. Launch the game once.";
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    const std::streamoff cap = 256 * 1024;
    if (sz > cap) in.seekg(sz - cap);
    else in.seekg(0);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (sz > cap) {
        auto nl = data.find('\n');
        if (nl != std::string::npos) data.erase(0, nl + 1);
    }
    std::vector<size_t> lines;
    lines.push_back(0);
    for (size_t i = 0; i < data.size(); i++)
        if (data[i] == '\n') lines.push_back(i + 1);
    size_t begin = lines.size() > 250 ? lines[lines.size() - 250] : 0;
    std::string body = data.substr(begin), crlf;
    crlf.reserve(body.size() + body.size() / 32);
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '\n' && (i == 0 || body[i - 1] != '\r')) crlf.push_back('\r');
        crlf.push_back(body[i]);
    }
    return widen(crlf);
}

// Launch.
std::wstring join_path_env(const std::wstring& extra, const std::wstring& existing) {
    if (extra.empty()) return existing;
    if (existing.empty()) return extra;
    if (!extra.empty() && extra.back() == L';') return extra + existing;
    return extra + L";" + existing;
}

std::wstring msys_bin() {
    if (GetFileAttributesW(L"C:\\msys64\\ucrt64\\bin") != INVALID_FILE_ATTRIBUTES) return L"C:\\msys64\\ucrt64\\bin";
    return {};
}

std::wstring build_env(const std::filesystem::path& root, const std::filesystem::path& exe) {
    std::wstring extra;
    std::wstring msys = msys_bin();
    if (!msys.empty()) extra += msys + L";";
    extra += exe.parent_path().wstring() + L";";
    wchar_t* cur = GetEnvironmentStringsW();
    std::wstring out;
    bool saw_path = false, saw_user = false;
    if (cur) {
        for (wchar_t* p = cur; *p;) {
            std::wstring e = p;
            p += e.size() + 1;
            if (e.rfind(L"PATH=", 0) == 0 || e.rfind(L"Path=", 0) == 0) {
                e = L"PATH=" + join_path_env(extra, e.substr(5));
                saw_path = true;
            } else if (e.rfind(L"BOTW_USER_ROOT=", 0) == 0) {
                e = L"BOTW_USER_ROOT=" + root.wstring();
                saw_user = true;
            }
            out += e;
            out.push_back(L'\0');
        }
        FreeEnvironmentStringsW(cur);
    }
    if (!saw_path) {
        out += L"PATH=" + extra;
        out.push_back(L'\0');
    }
    if (!saw_user) {
        out += L"BOTW_USER_ROOT=" + root.wstring();
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

void update_play_button();

std::wstring nice_name() {
    std::wstring n = widen(g.info.long_name);
    if (n.empty()) {
        std::filesystem::path p(g.cfg.game);
        n = g.cfg.game.empty() ? L"No game folder" : p.filename().wstring();
    }
    return n;
}

std::wstring info_line() {
    const wchar_t* dot = L"  \u00B7  ";
    std::wstring s;
    if (g.info.has_title_version) s += L"Update " + widen(gameinfo::version_name(g.info.title_version));
    else s += L"Update unknown";
    std::string region = gameinfo::region_name(g.info.title_id);
    if (!region.empty()) s += dot + widen(region);
    s += dot;
    s += L"Vulkan";
    if (g.cfg.fullscreen) s += dot + std::wstring(L"Fullscreen");
    int on = mods::enabled_count(g.mods);
    if (g.cfg.no_mods) s += dot + std::wstring(L"Mods off");
    else if (on > 0) s += dot + std::to_wstring(on) + (on == 1 ? L" mod on" : L" mods on");
    return s;
}

// Controls rows and capture.
struct ControlsWindow;
ControlsWindow* g_controls_win = nullptr;
void fill_controls();

void load_controls() {
    auto root = user_root();
    std::vector<controlsfile::Binding> file_rows;
    std::string err;
    auto st = controlsfile::load(root / "controls.ini", file_rows, err);
    bool present = st == controlsfile::LoadStatus::Ok && !file_rows.empty();
    if (st == controlsfile::LoadStatus::Error) alert(L"Could not read controls.ini:\n" + widen(err));
    g.rows = controlsfile::editor_rows(present, file_rows);
    fill_controls();
}

bool save_controls() {
    auto root = user_root();
    if (root.empty()) {
        alert(L"Set the saves and logs folder in Settings before saving controls.");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::string err;
    if (!controlsfile::save(root / "controls.ini", g.rows, err)) {
        alert(widen(err));
        return false;
    }
    return true;
}

void on_capture(HWND owner, int item, int sub) {
    if (item < 0 || item >= static_cast<int>(g.rows.size())) return;
    if (sub != 1 && sub != 2) return;
    if (!capture_binding(owner, item, sub == 1 ? CaptureSide::Key : CaptureSide::Pad)) return;
    if (!save_controls()) return;
    fill_controls();
    set_status(L"Saved controls.ini. Start the game again to use it.");
}

void on_reset() {
    if (ask(L"Put every input back to the built-in map? This rewrites controls.ini.", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    g.rows = controlsfile::defaults();
    if (!save_controls()) return;
    fill_controls();
    set_status(L"Controls reset to the built-in map and written to controls.ini. Start the game again to use them.");
}

void note_legacy_save() {
    if (saveimport::decimal_legacy_folder(user_root()))
        set_status(L"A save is in save\\2147483649 from an older build. Migrate old save copies it to save\\80000001 and leaves the old folder in place.",
                   Tone::Warn);
}

void open_folder(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    ShellExecuteW(alert_owner(), L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Settings.
struct SettingsWindow : Window {
    HWND game = nullptr, exe = nullptr, user = nullptr, seconds = nullptr, fullscreen = nullptr, gamepad_view = nullptr;
    HWND game_b = nullptr, exe_b = nullptr, user_b = nullptr, save_b = nullptr, cancel_b = nullptr;
    struct Label {
        RECT title, help;
        const wchar_t* t;
        const wchar_t* h;
    };
    Label labels[4]{};
    RECT seconds_note{};
    SettingsWindow() { bar = 64; }
    void create() override {
        game = ui::edit(hwnd, ID_GAME);
        game_b = ui::button(hwnd, ID_GAME_BROWSE, L"Browse", ui::Kind::Normal);
        exe = ui::edit(hwnd, ID_EXE);
        exe_b = ui::button(hwnd, ID_EXE_BROWSE, L"Browse", ui::Kind::Normal);
        user = ui::edit(hwnd, ID_USER);
        user_b = ui::button(hwnd, ID_USER_BROWSE, L"Browse", ui::Kind::Normal);
        fullscreen = ui::button(hwnd, ID_FULLSCREEN, L"Start in fullscreen", ui::Kind::Check);
        gamepad_view = ui::button(hwnd, ID_GAMEPAD_VIEW, L"Show GamePad screen beside TV (F2 / Guide)", ui::Kind::Check);
        seconds = ui::edit(hwnd, ID_SECONDS, ES_NUMBER);
        save_b = ui::button(hwnd, ID_SAVE_SETTINGS, L"Save", ui::Kind::Primary, true);
        cancel_b = ui::button(hwnd, ID_CANCEL_SETTINGS, L"Cancel", ui::Kind::Normal, true);
        set_text(game, g.cfg.game);
        set_text(exe, g.cfg.exe);
        set_text(user, g.cfg.user);
        set_text(seconds, g.cfg.seconds.empty() ? L"0" : g.cfg.seconds);
        g.cfg.gamepad_view = read_user_gamepad_view(user_root(), g.cfg.gamepad_view);
        ui::set_checked(fullscreen, g.cfg.fullscreen);
        ui::set_checked(gamepad_view, g.cfg.gamepad_view);
        labels[0] = {{}, {}, L"Game folder", L"The folder with code, content and meta from your v1.5.0 dump."};
        labels[1] = {{}, {}, L"Game program", L"BotWRecomp.exe. It is usually next to this launcher."};
        labels[2] = {{}, {}, L"Saves and logs", L"Saves, controls.ini, mods and runtime.log live here (BOTW_USER_ROOT)."};
        labels[3] = {{}, {}, L"Time limit", nullptr};
        SetFocus(game);
    }
    void layout(int w, int h) override {
        int m = S(24), y = S(24), field = S(32), gap = S(8);
        HWND edits[3] = {game, exe, user};
        HWND browse[3] = {game_b, exe_b, user_b};
        int lh = ui::line_height(ui::fonts().body_strong), sh = ui::line_height(ui::fonts().small);
        for (int i = 0; i < 3; i++) {
            labels[i].title = {m, y, w - m, y + lh};
            y += lh + S(2);
            labels[i].help = {m, y, w - m, y + sh};
            y += sh + gap;
            int bw = ui::button_width(browse[i]);
            ui::place_field(edits[i], {m, y, w - m - bw - gap, y + field});
            MoveWindow(browse[i], w - m - bw, y, bw, field, TRUE);
            y += field + S(24);
        }
        labels[3].title = {m, y, w - m, y + lh};
        y += lh + gap;
        ui::place_field(seconds, {m, y, m + S(88), y + field});
        seconds_note = {m + S(88) + gap + S(4), y, w - m, y + field};
        y += field + S(16);
        MoveWindow(fullscreen, m, y, ui::button_width(fullscreen), S(24), TRUE);
        y += S(24) + S(8);
        MoveWindow(gamepad_view, m, y, ui::button_width(gamepad_view), S(24), TRUE);
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row_right({cancel_b, save_b}, w - m, by, field, gap);
    }
    void paint(HDC dc, const RECT&) override {
        for (auto& l : labels) {
            ui::draw_text(dc, l.t, l.title, ui::fonts().body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            if (l.h) ui::draw_text(dc, l.h, l.help, ui::fonts().small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        ui::draw_text(dc, L"seconds, then the game closes. 0 means no limit.", seconds_note, ui::fonts().small, ui::pal.muted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_GAME_BROWSE: {
                auto p = pick_browse(hwnd, L"Folder that contains code, content, and meta.", false);
                if (!p.empty()) set_text(game, p);
                break;
            }
            case ID_EXE_BROWSE: {
                auto p = pick_exe(hwnd, get_text(exe));
                if (!p.empty()) set_text(exe, p);
                break;
            }
            case ID_USER_BROWSE: {
                auto p = pick_browse(hwnd, L"User data folder (BOTW_USER_ROOT). Saves, controls.ini, and the log go here.", false);
                if (!p.empty()) set_text(user, p);
                break;
            }
            case ID_SAVE_SETTINGS: ok(); break;
            case ID_CANCEL_SETTINGS: cancel(); break;
            default: break;
        }
    }
    void ok() override {
        std::wstring ga = get_text(game), ex = get_text(exe), us = get_text(user), se = get_text(seconds);
        if (has_quote(ga) || has_quote(ex) || has_quote(us)) {
            alert(L"Paths cannot contain a double quote.");
            return;
        }
        if (us.empty()) us = appdata_root();
        if (se.empty()) se = L"0";
        bool user_changed = us != g.cfg.user;
        g.cfg.game = ga;
        g.cfg.exe = ex;
        g.cfg.user = us;
        g.cfg.seconds = se;
        g.cfg.fullscreen = ui::checked(fullscreen);
        g.cfg.window_mode = g.cfg.fullscreen ? (g.cfg.window_mode ? g.cfg.window_mode : 1) : 0;
        g.cfg.gamepad_view = ui::checked(gamepad_view);
        write_user_gamepad_view(user_root(), g.cfg.gamepad_view);
        save_launcher_ini();
        if (user_changed) load_controls();
        refresh_game();
        if (user_changed) note_legacy_save();
        close();
    }
};

// Controls.
struct ControlsWindow : Window {
    HWND list = nullptr, set_key = nullptr, set_pad = nullptr, reset = nullptr, close_b = nullptr;
    RECT hint{};
    ControlsWindow() {
        bar = 64;
        min_w = 520;
        min_h = 360;
    }
    void create() override {
        g_controls_win = this;
        list = ui::list(hwnd, ID_CONTROLS, 0);
        const wchar_t* cols[] = {L"Input", L"Keyboard", L"Controller"};
        add_columns(list, cols, 3);
        set_key = ui::button(hwnd, ID_SET_KEY, L"Set key", ui::Kind::Normal, true);
        set_pad = ui::button(hwnd, ID_SET_PAD, L"Set controller button", ui::Kind::Normal, true);
        reset = ui::button(hwnd, ID_RESET, L"Reset to defaults", ui::Kind::Normal, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        load_controls();
        SetFocus(list);
        if (!g.rows.empty()) ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    ~ControlsWindow() override { g_controls_win = nullptr; }
    void layout(int w, int h) override {
        int m = S(24), field = S(32), gap = S(8);
        int th = text_height(ui::fonts().body, hint_text(), w - 2 * m);
        hint = {m, S(20), w - m, S(20) + th};
        int top = hint.bottom + S(12), bottom = h - S(bar) - S(16);
        ui::place_framed(list, {m, top, w - m, std::max(top + S(40), bottom)});
        int lw = w - 2 * m - S(4) - GetSystemMetrics(SM_CXVSCROLL);
        ListView_SetColumnWidth(list, 0, lw * 40 / 100);
        ListView_SetColumnWidth(list, 1, lw * 30 / 100);
        ListView_SetColumnWidth(list, 2, lw - lw * 40 / 100 - lw * 30 / 100);
        int by = h - S(bar) + (S(bar) - field) / 2;
        int x = place_row({set_key, set_pad}, m, by, field, gap);
        MoveWindow(reset, x + S(8), by, ui::button_width(reset), field, TRUE);
        place_row_right({close_b}, w - m, by, field, gap);
    }
    static std::wstring hint_text() {
        return L"Click a Keyboard or Controller cell, then press the new key or button. Backspace clears it, Esc keeps it. Changes are saved to controls.ini right away.";
    }
    void paint(HDC dc, const RECT&) override { paint_lines(dc, hint_text(), hint, ui::fonts().body, ui::pal.muted); }
    int selected() const { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_SET_KEY: on_capture(hwnd, selected(), 1); break;
            case ID_SET_PAD: on_capture(hwnd, selected(), 2); break;
            case ID_RESET: on_reset(); break;
            case ID_CLOSE: close(); break;
            default: break;
        }
    }
    LRESULT notify(NMHDR* hdr) override {
        if (hdr->hwndFrom == list && (hdr->code == NM_CLICK || hdr->code == NM_DBLCLK)) {
            auto* act = reinterpret_cast<NMITEMACTIVATE*>(hdr);
            LVHITTESTINFO ht{};
            ht.pt = act->ptAction;
            if (ListView_SubItemHitTest(list, &ht) >= 0) on_capture(hwnd, ht.iItem, ht.iSubItem);
        }
        return 0;
    }
    void ok() override { on_capture(hwnd, selected(), 1); }
};

void fill_controls() {
    if (!g_controls_win || !g_controls_win->list) return;
    HWND lv = g_controls_win->list;
    int keep = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < static_cast<int>(g.rows.size()); i++) {
        const auto& row = g.rows[static_cast<size_t>(i)];
        const auto* action = controlsfile::find_action(row.id);
        std::wstring label = widen(action ? action->label : row.id.c_str());
        std::wstring key = widen(controlsfile::keys_label(row));
        std::wstring pad = widen(controlsfile::pad_label(row.pad));
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = label.data();
        it.lParam = i;
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, i, 1, key.data());
        ListView_SetItemText(lv, i, 2, pad.data());
    }
    if (keep >= 0 && keep < static_cast<int>(g.rows.size())) {
        ListView_SetItemState(lv, keep, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(lv, keep, FALSE);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

// Save sets.
struct SavesWindow : Window {
    HWND list = nullptr, import_b = nullptr, switch_b = nullptr, delete_b = nullptr, migrate_b = nullptr;
    HWND open_b = nullptr, refresh_b = nullptr, close_b = nullptr;
    RECT hint{};
    SavesWindow() {
        bar = 64;
        min_w = 680;
        min_h = 340;
    }
    void create() override {
        g.saves_win = this;
        list = ui::list(hwnd, ID_SAVES, 0);
        const wchar_t* cols[] = {L"Name", L"Kind", L"When", L"Notes"};
        add_columns(list, cols, 4);
        import_b = ui::button(hwnd, ID_IMPORT, L"Import save", ui::Kind::Primary, true);
        switch_b = ui::button(hwnd, ID_SWITCH, L"Switch to", ui::Kind::Normal, true);
        delete_b = ui::button(hwnd, ID_DELETE, L"Delete", ui::Kind::Normal, true);
        migrate_b = ui::button(hwnd, ID_MIGRATE, L"Migrate old save", ui::Kind::Normal, true);
        open_b = ui::button(hwnd, ID_OPEN, L"Open saves folder", ui::Kind::Flat, true);
        refresh_b = ui::button(hwnd, ID_REFRESH, L"Refresh", ui::Kind::Flat, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        DragAcceptFiles(hwnd, TRUE);
        fill_saves();
        sync();
        SetFocus(list);
    }
    ~SavesWindow() override { g.saves_win = nullptr; }
    std::wstring hint_text() const {
        return L"The live save is in " + (user_root() / "save" / "80000001").wstring() +
               L". Import, Switch to and Migrate back it up first. Drop a save folder or .zip on this window to import it.";
    }
    void layout(int w, int h) override {
        int m = S(24), field = S(32), gap = S(8);
        int th = text_height(ui::fonts().body, hint_text(), w - 2 * m);
        hint = {m, S(20), w - m, S(20) + th};
        int top = hint.bottom + S(12), bottom = h - S(bar) - S(16);
        ui::place_framed(list, {m, top, w - m, std::max(top + S(40), bottom)});
        int lw = w - 2 * m - S(4) - GetSystemMetrics(SM_CXVSCROLL);
        ListView_SetColumnWidth(list, 0, lw * 34 / 100);
        ListView_SetColumnWidth(list, 1, lw * 14 / 100);
        ListView_SetColumnWidth(list, 2, lw * 22 / 100);
        ListView_SetColumnWidth(list, 3, lw - lw * 34 / 100 - lw * 14 / 100 - lw * 22 / 100);
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row({import_b, switch_b, delete_b, migrate_b}, m, by, field, gap);
        place_row_right({close_b, refresh_b, open_b}, w - m, by, field, S(4));
    }
    void paint(HDC dc, const RECT&) override { paint_lines(dc, hint_text(), hint, ui::fonts().body, ui::pal.muted); }
    void sync() {
        BOOL on = game_running() ? FALSE : TRUE;
        for (HWND b : {import_b, switch_b, delete_b, migrate_b}) EnableWindow(b, on);
    }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_IMPORT: on_import(); break;
            case ID_SWITCH: on_switch(); break;
            case ID_DELETE: on_delete(); break;
            case ID_MIGRATE: on_migrate(); break;
            case ID_OPEN: on_open_saves(); break;
            case ID_REFRESH:
                fill_saves();
                note_legacy_save();
                break;
            case ID_CLOSE: close(); break;
            default: break;
        }
    }
    LRESULT notify(NMHDR* hdr) override {
        if (hdr->hwndFrom == list && hdr->code == NM_DBLCLK && !game_running()) on_switch();
        return 0;
    }
    void dropped(const std::wstring& p) override { import_path(p); }
};

void fill_saves() {
    g.sets = saveimport::list_sets(user_root());
    if (!g.saves_win || !g.saves_win->list) return;
    HWND lv = g.saves_win->list;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < static_cast<int>(g.sets.size()); i++) {
        const auto& s = g.sets[static_cast<size_t>(i)];
        std::wstring title = widen(s.title);
        std::wstring kind = widen(s.kind);
        std::wstring when = widen(s.created);
        std::string note = s.warnings.empty() ? std::to_string(s.slots) + " slots" : s.warnings;
        std::wstring notes = widen(note);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = title.data();
        it.lParam = i;
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, i, 1, kind.data());
        ListView_SetItemText(lv, i, 2, when.data());
        ListView_SetItemText(lv, i, 3, notes.data());
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

int selected_set() {
    if (!g.saves_win || !g.saves_win->list) return -1;
    int i = ListView_GetNextItem(g.saves_win->list, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(g.sets.size())) return -1;
    return i;
}

// Mods.
std::wstring format_mod_presets(const mods::Mod& md) {
    if (md.presets.empty()) return L"-";
    std::wstring out;
    for (const auto& pc : md.presets) {
        if (pc.active.empty()) continue;
        if (!out.empty()) out += L", ";
        if (!pc.category.empty() && md.presets.size() > 1) {
            out += widen(pc.category) + L": " + widen(pc.active);
        } else {
            out += widen(pc.active);
        }
    }
    return out.empty() ? L"-" : out;
}

std::wstring format_mod_status(const mods::Mod& md) {
    std::vector<std::wstring> parts;
    uint32_t total_files = md.content_files + md.aoc_files;
    if (total_files > 0) parts.push_back(std::to_wstring(total_files) + (total_files == 1 ? L" file" : L" files"));
    if (md.patch_files > 0) parts.push_back(std::to_wstring(md.patch_files) + (md.patch_files == 1 ? L" patch" : L" patches"));
    if (md.texture_rules > 0) parts.push_back(std::to_wstring(md.texture_rules) + (md.texture_rules == 1 ? L" tex rule" : L" tex rules"));
    if (md.shader_files > 0) parts.push_back(std::to_wstring(md.shader_files) + (md.shader_files == 1 ? L" shader" : L" shaders"));
    if (parts.empty()) return L"No files or rules";
    std::wstring s;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) s += L", ";
        s += parts[i];
    }
    if (!md.overrides_mods.empty()) s += L" · Overrides " + std::to_wstring(md.overrides_mods.size());
    if (!md.overridden_by_mods.empty()) s += L" · Overridden";
    return s;
}

bool install_mod_path(const std::wstring& picked, std::string* installed_folder_out = nullptr) {
    if (picked.empty()) return false;
    if (!have_user()) return false;
    std::string folder, err;
    if (!mods::install_mod(user_root(), std::filesystem::path(picked), folder, err)) {
        alert(L"Could not install mod:\n" + widen(err));
        return false;
    }
    g.mods = mods::load(user_root());
    update_play_button();
    set_status(L"Installed and enabled mod "" + widen(folder) + L"".", Tone::Good);
    if (installed_folder_out) *installed_folder_out = folder;
    return true;
}

struct ModsWindow : Window {
    struct PresetDropdown {
        std::string category;
        RECT label_r{};
        HWND combo = nullptr;
    };
    HWND list = nullptr, install_b = nullptr, toggle_b = nullptr, up_b = nullptr, down_b = nullptr;
    HWND folder_b = nullptr, refresh_b = nullptr, no_mods = nullptr, no_dlc = nullptr, close_b = nullptr;
    std::vector<PresetDropdown> preset_combos;
    RECT hint{}, detail_r{}, detail_title_r{}, detail_meta_r{}, detail_desc_r{}, detail_conflict_r{};
    bool filling = false;
    ModsWindow() {
        bar = 64;
        min_w = 780;
        min_h = 520;
    }
    void create() override {
        list = ui::list(hwnd, ID_MODS, LVS_EX_CHECKBOXES);
        const wchar_t* cols[] = {L"On / Mod Name", L"#", L"Version", L"Preset", L"Status / Files"};
        add_columns(list, cols, 5);
        install_b = ui::button(hwnd, ID_MOD_INSTALL, L"Install Mod (.zip / .bnp / Folder)...", ui::Kind::Primary, true);
        toggle_b = ui::button(hwnd, ID_MOD_TOGGLE, L"Enable / Disable", ui::Kind::Normal, true);
        up_b = ui::button(hwnd, ID_MOD_UP, L"Move up", ui::Kind::Normal, true);
        down_b = ui::button(hwnd, ID_MOD_DOWN, L"Move down", ui::Kind::Normal, true);
        folder_b = ui::button(hwnd, ID_MODS_FOLDER, L"Open mods folder", ui::Kind::Flat, true);
        refresh_b = ui::button(hwnd, ID_MODS_REFRESH, L"Refresh", ui::Kind::Flat, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        no_mods = ui::button(hwnd, ID_NO_MODS, L"Launch without mods", ui::Kind::Check);
        ui::set_checked(no_mods, g.cfg.no_mods);
        no_dlc = ui::button(hwnd, ID_NO_DLC, L"Launch without DLC", ui::Kind::Check);
        ui::set_checked(no_dlc, g.cfg.no_dlc);
        DragAcceptFiles(hwnd, TRUE);
        g.mods = mods::load(user_root());
        fill(0);
        SetFocus(list);
    }
    std::wstring hint_text() const {
        std::wstring s = L"Mods live in " + mods::mods_dir(user_root()).wstring() +
                         L". Checked mods load in priority order from top (#1) to bottom (later overrides earlier). Drop a .zip, .bnp, or mod folder anywhere here to install it.";
        if (g.mods.list.empty()) s += L" No mods installed yet.";
        return s;
    }
    int selected() const {
        int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (i < 0 || i >= static_cast<int>(g.mods.list.size())) return -1;
        return i;
    }
    void clear_preset_combos() {
        for (auto& pd : preset_combos) {
            if (pd.combo) DestroyWindow(pd.combo);
        }
        preset_combos.clear();
    }
    void rebuild_preset_combos() {
        clear_preset_combos();
        int idx = selected();
        if (idx < 0) return;
        const auto& md = g.mods.list[static_cast<size_t>(idx)];
        int count = std::min<int>(static_cast<int>(md.presets.size()), ID_MOD_PRESET_MAX - ID_MOD_PRESET_BASE + 1);
        for (int i = 0; i < count; ++i) {
            const auto& pc = md.presets[static_cast<size_t>(i)];
            if (pc.options.empty()) continue;
            HWND cb = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      0, 0, 120, S(240), hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MOD_PRESET_BASE + i)),
                                      g.inst, nullptr);
            SendMessageW(cb, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
            int sel = 0;
            for (int j = 0; j < static_cast<int>(pc.options.size()); ++j) {
                std::wstring opt = widen(pc.options[static_cast<size_t>(j)]);
                SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(opt.c_str()));
                if (pc.options[static_cast<size_t>(j)] == pc.active) sel = j;
            }
            SendMessageW(cb, CB_SETCURSEL, sel, 0);
            preset_combos.push_back({pc.category, {}, cb});
        }
    }
    void layout(int w, int h) override {
        int m = S(24), field = S(32), gap = S(8);
        int th = text_height(ui::fonts().body, hint_text(), w - 2 * m);
        hint = {m, S(16), w - m, S(16) + th};
        int detail_h = S(132);
        int bottom_check_y = h - S(bar) - S(12) - S(24);
        int detail_bottom = bottom_check_y - S(10);
        int detail_top = std::max<int>(static_cast<int>(hint.bottom) + S(120), detail_bottom - detail_h);
        int list_top = hint.bottom + S(10);
        int list_bottom = std::max(list_top + S(80), detail_top - S(10));

        ui::place_framed(list, {m, list_top, w - m, list_bottom});
        int lw = w - 2 * m - S(4) - GetSystemMetrics(SM_CXVSCROLL);
        int c0 = lw * 30 / 100;
        int c1 = lw * 7 / 100;
        int c2 = lw * 11 / 100;
        int c3 = lw * 22 / 100;
        int c4 = lw - c0 - c1 - c2 - c3;
        ListView_SetColumnWidth(list, 0, c0);
        ListView_SetColumnWidth(list, 1, c1);
        ListView_SetColumnWidth(list, 2, c2);
        ListView_SetColumnWidth(list, 3, c3);
        ListView_SetColumnWidth(list, 4, c4);

        detail_r = {m, detail_top, w - m, detail_bottom};
        int pad = S(12);
        int right_col_w = preset_combos.empty() ? 0 : std::min(S(280), (w - 2 * m) * 42 / 100);
        int left_right = detail_r.right - pad - (right_col_w ? (right_col_w + S(16)) : 0);
        int lh_strong = ui::line_height(ui::fonts().body_strong);
        int lh_small = ui::line_height(ui::fonts().small);
        int dy = detail_r.top + pad;
        detail_title_r = {detail_r.left + pad, dy, left_right, dy + lh_strong};
        dy += lh_strong + S(4);
        detail_meta_r = {detail_r.left + pad, dy, left_right, dy + lh_small};
        dy += lh_small + S(4);
        detail_conflict_r = {detail_r.left + pad, dy, left_right, dy + lh_small};
        dy += lh_small + S(6);
        detail_desc_r = {detail_r.left + pad, dy, left_right, detail_r.bottom - pad};

        if (!preset_combos.empty()) {
            int rx = detail_r.right - pad - right_col_w;
            int ry = detail_r.top + pad;
            int row_h = S(26);
            for (auto& pd : preset_combos) {
                int label_w = S(96);
                pd.label_r = {rx, ry, rx + label_w - S(6), ry + row_h};
                MoveWindow(pd.combo, rx + label_w, ry, right_col_w - label_w, S(220), TRUE);
                ry += row_h + S(6);
            }
        }

        MoveWindow(no_mods, m, bottom_check_y, ui::button_width(no_mods), S(24), TRUE);
        MoveWindow(no_dlc, m + ui::button_width(no_mods) + S(20), bottom_check_y, ui::button_width(no_dlc), S(24), TRUE);
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row({install_b, toggle_b, up_b, down_b}, m, by, field, gap);
        place_row_right({close_b, refresh_b, folder_b}, w - m, by, field, S(4));
    }
    void paint(HDC dc, const RECT&) override {
        const auto& f = ui::fonts();
        paint_lines(dc, hint_text(), hint, f.body, ui::pal.muted);
        ui::round_rect(dc, detail_r, S(4), ui::pal.field, ui::pal.field_line);
        int idx = selected();
        if (idx < 0) {
            ui::draw_text(dc, g.mods.list.empty() ? L"No mods installed. Click \"Install Mod (.zip / Folder)...\" or drop a mod .zip here."
                                                  : L"Select a mod above to view details, file conflicts, and graphic pack presets.",
                          detail_title_r, f.body, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            return;
        }
        const auto& md = g.mods.list[static_cast<size_t>(idx)];
        std::wstring title = widen(md.name);
        if (!md.version.empty()) title += L"  v" + widen(md.version);
        title += md.enabled ? L"   [Enabled]" : L"   [Disabled]";
        ui::draw_text(dc, title, detail_title_r, f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        std::wstring meta = L"Folder: mods\\" + widen(md.folder) + L"   \u00B7   " +
                            std::to_wstring(md.content_files) + L" content files, " +
                            std::to_wstring(md.aoc_files) + L" DLC files, " +
                            std::to_wstring(md.patch_files) + L" patches";
        if (md.texture_rules > 0) meta += L", " + std::to_wstring(md.texture_rules) + L" texture rules";
        if (md.shader_files > 0) meta += L", " + std::to_wstring(md.shader_files) + L" custom shaders";
        ui::draw_text(dc, meta, detail_meta_r, f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        std::wstring conflict;
        COLORREF conflict_col = ui::pal.muted;
        if (!md.overrides_mods.empty()) {
            conflict += L"Overrides: ";
            for (size_t i = 0; i < md.overrides_mods.size(); ++i) {
                if (i) conflict += L", ";
                conflict += widen(md.overrides_mods[i]);
            }
        }
        if (!md.overridden_by_mods.empty()) {
            if (!conflict.empty()) conflict += L"   ·   ";
            conflict += L"Overridden by: ";
            for (size_t i = 0; i < md.overridden_by_mods.size(); ++i) {
                if (i) conflict += L", ";
                conflict += widen(md.overridden_by_mods[i]);
            }
            conflict_col = ui::pal.warn;
        }
        if (conflict.empty()) conflict = L"No file conflicts with other enabled mods.";
        ui::draw_text(dc, conflict, detail_conflict_r, f.small, conflict_col, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        std::wstring desc = md.description.empty() ? L"No description in rules.txt." : widen(md.description);
        ui::draw_text(dc, desc, detail_desc_r, f.body, ui::pal.text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);

        for (const auto& pd : preset_combos) {
            std::wstring lbl = pd.category.empty() ? L"Preset:" : (widen(pd.category) + L":");
            ui::draw_text(dc, lbl, pd.label_r, f.small, ui::pal.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    void update_row_texts() {
        filling = true;
        for (int i = 0; i < static_cast<int>(g.mods.list.size()); i++) {
            const auto& md = g.mods.list[static_cast<size_t>(i)];
            std::wstring name = widen(md.name);
            std::wstring prio = L"#" + std::to_wstring(i + 1);
            std::wstring ver = md.version.empty() ? L"-" : widen(md.version);
            std::wstring preset = format_mod_presets(md);
            std::wstring status = format_mod_status(md);
            ListView_SetItemText(list, i, 0, name.data());
            ListView_SetItemText(list, i, 1, prio.data());
            ListView_SetItemText(list, i, 2, ver.data());
            ListView_SetItemText(list, i, 3, preset.data());
            ListView_SetItemText(list, i, 4, status.data());
            ListView_SetCheckState(list, i, md.enabled ? TRUE : FALSE);
        }
        filling = false;
    }
    void fill(int select) {
        filling = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        for (int i = 0; i < static_cast<int>(g.mods.list.size()); i++) {
            const auto& md = g.mods.list[static_cast<size_t>(i)];
            std::wstring name = widen(md.name);
            std::wstring prio = L"#" + std::to_wstring(i + 1);
            std::wstring ver = md.version.empty() ? L"-" : widen(md.version);
            std::wstring preset = format_mod_presets(md);
            std::wstring status = format_mod_status(md);
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = i;
            it.pszText = name.data();
            it.lParam = i;
            ListView_InsertItem(list, &it);
            ListView_SetItemText(list, i, 1, prio.data());
            ListView_SetItemText(list, i, 2, ver.data());
            ListView_SetItemText(list, i, 3, preset.data());
            ListView_SetItemText(list, i, 4, status.data());
            ListView_SetCheckState(list, i, md.enabled ? TRUE : FALSE);
        }
        if (select >= 0 && select < static_cast<int>(g.mods.list.size())) {
            ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, select, FALSE);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        filling = false;
        rebuild_preset_combos();
        relayout();
    }
    void save(bool order) {
        std::string err;
        if (!mods::save(user_root(), g.mods, order, err)) alert(L"Could not write mods.ini:\n" + widen(err));
        int sel = selected();
        g.mods = mods::load(user_root());
        update_row_texts();
        if (sel >= 0 && sel < static_cast<int>(g.mods.list.size())) {
            filling = true;
            ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            filling = false;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        update_play_button();
    }
    void move(int delta) {
        int i = selected();
        int j = i + delta;
        if (i < 0 || j < 0 || j >= static_cast<int>(g.mods.list.size())) return;
        std::swap(g.mods.list[static_cast<size_t>(i)], g.mods.list[static_cast<size_t>(j)]);
        std::string err;
        if (!mods::save(user_root(), g.mods, true, err)) alert(L"Could not write mods.ini:\n" + widen(err));
        g.mods = mods::load(user_root());
        fill(j);
        update_play_button();
    }
    void toggle_selected() {
        int i = selected();
        if (i < 0) return;
        auto& md = g.mods.list[static_cast<size_t>(i)];
        md.enabled = !md.enabled;
        save(false);
    }
    void on_install() {
        std::wstring picked = pick_browse(hwnd, L"Select a BotW mod folder or a .zip / .bnp archive to install.", true);
        if (picked.empty()) return;
        install_and_select(picked);
    }
    void install_and_select(const std::wstring& picked) {
        std::string folder;
        if (!install_mod_path(picked, &folder)) return;
        int sel = 0;
        for (int i = 0; i < static_cast<int>(g.mods.list.size()); ++i) {
            if (_stricmp(g.mods.list[static_cast<size_t>(i)].folder.c_str(), folder.c_str()) == 0) {
                sel = i;
                break;
            }
        }
        fill(sel);
    }
    void command(int id, int code, HWND ctl) override {
        if (id >= ID_MOD_PRESET_BASE && id <= ID_MOD_PRESET_MAX && code == CBN_SELCHANGE) {
            int idx = selected();
            int p_idx = id - ID_MOD_PRESET_BASE;
            if (idx >= 0 && p_idx >= 0 && p_idx < static_cast<int>(preset_combos.size())) {
                auto& md = g.mods.list[static_cast<size_t>(idx)];
                if (p_idx < static_cast<int>(md.presets.size())) {
                    int cur = static_cast<int>(SendMessageW(ctl, CB_GETCURSEL, 0, 0));
                    auto& pc = md.presets[static_cast<size_t>(p_idx)];
                    if (cur >= 0 && cur < static_cast<int>(pc.options.size())) {
                        pc.active = pc.options[static_cast<size_t>(cur)];
                        save(false);
                    }
                }
            }
            return;
        }
        switch (id) {
            case ID_MOD_INSTALL: on_install(); break;
            case ID_MOD_TOGGLE: toggle_selected(); break;
            case ID_MOD_UP: move(-1); break;
            case ID_MOD_DOWN: move(1); break;
            case ID_MODS_FOLDER: open_folder(mods::mods_dir(user_root())); break;
            case ID_MODS_REFRESH:
                g.mods = mods::load(user_root());
                fill(std::max(0, selected()));
                update_play_button();
                break;
            case ID_NO_MODS:
                g.cfg.no_mods = ui::checked(no_mods);
                save_launcher_ini();
                update_play_button();
                break;
            case ID_NO_DLC:
                g.cfg.no_dlc = ui::checked(no_dlc);
                save_launcher_ini();
                break;
            case ID_CLOSE: close(); break;
            default: break;
        }
    }
    LRESULT notify(NMHDR* hdr) override {
        if (hdr->hwndFrom != list || filling) return 0;
        if (hdr->code == NM_DBLCLK) {
            toggle_selected();
            return 0;
        }
        if (hdr->code != LVN_ITEMCHANGED) return 0;
        auto* nm = reinterpret_cast<NMLISTVIEW*>(hdr);
        if (!(nm->uChanged & LVIF_STATE)) return 0;
        if (nm->iItem < 0 || nm->iItem >= static_cast<int>(g.mods.list.size())) return 0;
        if ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) {
            bool on = ListView_GetCheckState(list, nm->iItem) != 0;
            auto& md = g.mods.list[static_cast<size_t>(nm->iItem)];
            if (md.enabled != on) {
                md.enabled = on;
                save(false);
            }
        }
        if ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED) {
            if (nm->uNewState & LVIS_SELECTED) {
                rebuild_preset_combos();
                relayout();
            }
        }
        return 0;
    }
    void dropped(const std::wstring& p) override { install_and_select(p); }
};

// Log.
struct LogWindow : Window {
    HWND text = nullptr, file_b = nullptr, close_b = nullptr;
    LogWindow() {
        bar = 64;
        min_w = 520;
        min_h = 300;
    }
    void create() override {
        text = ui::edit(hwnd, ID_LOG_TEXT, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL, ui::FontRole::Mono);
        SetWindowLongW(text, GWL_STYLE, GetWindowLongW(text, GWL_STYLE) && ~ES_AUTOHSCROLL);
        set_text(text, tail_file(user_root() / "runtime.log"));
        file_b = ui::button(hwnd, ID_LOG_FILE, L"Open in editor", ui::Kind::Normal, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        SetFocus(close_b);
    }
    void layout(int w, int h) override {
        int m = S(24), field = S(32);
        ui::place_framed(text, {m, S(24), w - m, std::max(S(64), h - S(bar) - S(16))});
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row({file_b}, m, by, field, S(8));
        place_row_right({close_b}, w - m, by, field, S(8));
        int n = GetWindowTextLengthW(text);
        SendMessageW(text, EM_SETSEL, n, n);
        SendMessageW(text, EM_SCROLLCARET, 0, 0);
    }
    void command(int id, int, HWND) override {
        if (id == ID_CLOSE) close();
        else if (id == ID_LOG_FILE) {
            auto p = user_root() / "runtime.log";
            std::error_code ec;
            if (!std::filesystem::is_regular_file(p, ec)) alert(L"No runtime.log yet. Launch the game once.");
            else ShellExecuteW(hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
};

// Setup Assistant (4-step wizard).

struct SetupWizardWindow : Window {
    int step = 0;  // 0..3
    SetupPreset preset = SetupPreset::None;
    setupwizard::ToolchainStatus tc;
    gameinfo::Info ginfo;
    gameinfo::RpxVerify rpx_ver;
    bool separate_mode = false;
    std::wstring merge_note;
    Tone merge_tone = Tone::Normal;

    // Step 1 controls
    HWND user_e = nullptr, user_b = nullptr;
    HWND tc_refresh_b = nullptr, tc_install_b = nullptr;

    // Step 2 controls
    HWND mode_merged_b = nullptr, mode_sep_b = nullptr, autodetect_b = nullptr;
    HWND game_e = nullptr, game_b = nullptr;
    HWND base_e = nullptr, base_b = nullptr;
    HWND upd_e = nullptr, upd_b = nullptr;
    HWND dlc_e = nullptr, dlc_b = nullptr;
    HWND merge_b = nullptr;

    // Step 3 controls && worker state
    HWND build_start_b = nullptr, build_cancel_b = nullptr, exe_pick_b = nullptr;
    HWND log_e = nullptr;
    std::mutex build_mu;
    setupwizard::BuildProgress prog;
    std::string log_buf;
    bool log_dirty = false;
    bool building = false;
    std::atomic<bool> cancel_req{false};
    HANDLE child_proc = nullptr;
    DWORD build_start_tick = 0;
    DWORD elapsed_sec = 0;
    std::thread worker;

    // Step 4 controls
    setupwizard::GamePrefs prefs;
    HWND scale_1x = nullptr, scale_2x = nullptr, scale_3x = nullptr;
    HWND fps_30 = nullptr, fps_60 = nullptr;
    HWND lay_auto = nullptr, lay_xbox = nullptr, lay_nin = nullptr;
    HWND chk_fs = nullptr, chk_gpv = nullptr, chk_gyro = nullptr;
    HWND imp_save_b = nullptr;
    std::wstring save_note;

    // Bottom bar controls
    HWND back_b = nullptr, next_b = nullptr, close_b = nullptr;

    explicit SetupWizardWindow(SetupPreset p = SetupPreset::None) : preset(p) {
        bar = 64;
        min_w = 760;
        min_h = 560;
        if (p == SetupPreset::Step2) step = 1;
        else if (p == SetupPreset::Step3Idle || p == SetupPreset::Step3Recomp || p == SetupPreset::Step3Compile) step = 2;
        else if (p == SetupPreset::Step4) step = 3;
    }

    ~SetupWizardWindow() override {
        stop_worker();
    }

    void stop_worker() {
        cancel_req = true;
        {
            std::lock_guard<std::mutex> lk(build_mu);
            if (child_proc) TerminateProcess(child_proc, 1);
        }
        if (worker.joinable()) worker.join();
    }

    void create() override {
        // Step 1
        user_e = ui::edit(hwnd, ID_WIZ_USER);
        user_b = ui::button(hwnd, ID_WIZ_USER_BROWSE, L"Browse", ui::Kind::Normal);
        tc_refresh_b = ui::button(hwnd, ID_WIZ_TOOLCHAIN_REFRESH, L"Re-check environment", ui::Kind::Normal);
        tc_install_b = ui::button(hwnd, ID_WIZ_TOOLCHAIN_INSTALL, L"Install MSYS2 toolchain", ui::Kind::Normal);

        // Step 2
        mode_merged_b = ui::button(hwnd, ID_WIZ_MODE_MERGED, L"Single merged folder", ui::Kind::Primary);
        mode_sep_b = ui::button(hwnd, ID_WIZ_MODE_SEPARATE, L"Separate Base + Update + DLC", ui::Kind::Normal);
        autodetect_b = ui::button(hwnd, ID_WIZ_AUTODETECT, L"Auto-detect dumps...", ui::Kind::Normal);
        game_e = ui::edit(hwnd, ID_WIZ_GAME);
        game_b = ui::button(hwnd, ID_WIZ_GAME_BROWSE, L"Browse", ui::Kind::Normal);
        base_e = ui::edit(hwnd, ID_WIZ_BASE);
        base_b = ui::button(hwnd, ID_WIZ_BASE_BROWSE, L"Browse", ui::Kind::Normal);
        upd_e = ui::edit(hwnd, ID_WIZ_UPD);
        upd_b = ui::button(hwnd, ID_WIZ_UPD_BROWSE, L"Browse", ui::Kind::Normal);
        dlc_e = ui::edit(hwnd, ID_WIZ_DLC);
        dlc_b = ui::button(hwnd, ID_WIZ_DLC_BROWSE, L"Browse", ui::Kind::Normal);
        merge_b = ui::button(hwnd, ID_WIZ_MERGE, L"Merge Base + Update + DLC", ui::Kind::Primary);

        // Step 3
        build_start_b = ui::button(hwnd, ID_WIZ_BUILD_START, L"Convert Wii U to C++ & Build Executable", ui::Kind::Primary);
        build_cancel_b = ui::button(hwnd, ID_WIZ_BUILD_CANCEL, L"Cancel build", ui::Kind::Normal);
        exe_pick_b = ui::button(hwnd, ID_WIZ_EXE_BROWSE, L"Use existing BotWRecomp.exe...", ui::Kind::Normal);
        log_e = ui::edit(hwnd, ID_WIZ_LOG_EDIT, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL, ui::FontRole::Mono);
        SetWindowLongW(log_e, GWL_STYLE, GetWindowLongW(log_e, GWL_STYLE) && ~ES_AUTOHSCROLL);

        // Step 4
        scale_1x = ui::button(hwnd, ID_WIZ_SCALE_1X, L"720p (1x Native)", ui::Kind::Normal);
        scale_2x = ui::button(hwnd, ID_WIZ_SCALE_2X, L"1440p (2x Crisp)", ui::Kind::Normal);
        scale_3x = ui::button(hwnd, ID_WIZ_SCALE_3X, L"2160p (3x 4K)", ui::Kind::Normal);
        fps_30 = ui::button(hwnd, ID_WIZ_FPS_30, L"30 FPS (Original)", ui::Kind::Normal);
        fps_60 = ui::button(hwnd, ID_WIZ_FPS_60, L"60 FPS", ui::Kind::Normal);
        lay_auto = ui::button(hwnd, ID_WIZ_LAYOUT_AUTO, L"Auto-detect", ui::Kind::Normal);
        lay_xbox = ui::button(hwnd, ID_WIZ_LAYOUT_XBOX, L"Xbox (A = bottom)", ui::Kind::Normal);
        lay_nin = ui::button(hwnd, ID_WIZ_LAYOUT_NINTENDO, L"Nintendo (A = right)", ui::Kind::Normal);
        chk_fs = ui::button(hwnd, ID_WIZ_FULLSCREEN, L"Start in fullscreen", ui::Kind::Check);
        chk_gpv = ui::button(hwnd, ID_WIZ_GAMEPAD_VIEW, L"Show Wii U GamePad screen beside TV (F2 / Guide)", ui::Kind::Check);
        chk_gyro = ui::button(hwnd, ID_WIZ_GYRO, L"Enable controller motion / gyro aiming", ui::Kind::Check);
        imp_save_b = ui::button(hwnd, ID_WIZ_IMPORT_SAVE, L"Import Cemu or Wii U save...", ui::Kind::Normal);

        // Bottom bar
        back_b = ui::button(hwnd, ID_WIZ_BACK, L"Back", ui::Kind::Normal, true);
        next_b = ui::button(hwnd, ID_WIZ_NEXT, L"Next", ui::Kind::Primary, true);
        close_b = ui::button(hwnd, ID_WIZ_CANCEL, L"Close", ui::Kind::Normal, true);

        set_text(user_e, g.cfg.user.empty() ? appdata_root() : g.cfg.user);
        set_text(game_e, g.cfg.game);

        prefs = setupwizard::read_game_prefs(user_root());
        prefs.gamepad_view = g.cfg.gamepad_view;
        ui::set_checked(chk_fs, g.cfg.fullscreen);
        ui::set_checked(chk_gpv, prefs.gamepad_view);
        ui::set_checked(chk_gyro, prefs.gyro);

        refresh_toolchain();
        inspect_game_folder();
        sync_pref_buttons();

        if (preset == SetupPreset::Step3Recomp) {
            building = true;
            elapsed_sec = 18;
            prog.stage = setupwizard::BuildStage::RecompileWiiUToCpp;
            prog.recompile_step = 3;
            prog.recompile_total = 4;
            prog.sub_done = 42;
            prog.sub_total = 258;
            prog.stage_percent = 16;
            prog.overall_percent = 16;
            tc.has_exe = false;
            prog.current_item = "fn_020c0000.cpp";
            prog.phase_text = "Step 1/2: Converting Wii U binary to C++ (shard 42 / 258, 16%)...";
            log_buf =
                "[recomp 1/4] Decompressing & parsing U-King.rpx...\r\n"
                "  text: 0x02000000 .. 0x030eab60 (17,738,592 bytes)\r\n"
                "  rodata: 0x10000000 .. 0x109a3800 (10,106,880 bytes)\r\n"
                "[recomp 2/4] Analyzing 66137 PowerPC functions...\r\n"
                "  discovered 66,137 functions, 1,412,908 basic blocks, 3,842 jump tables\r\n"
                "[recomp 3/4] Generating C++ shard 1/258 (0%) [fn_02000000.cpp]...\r\n"
                "[recomp 3/4] Generating C++ shard 16/258 (6%) [fn_02040000.cpp]...\r\n"
                "[recomp 3/4] Generating C++ shard 32/258 (12%) [fn_02080000.cpp]...\r\n"
                "[recomp 3/4] Generating C++ shard 42/258 (16%) [fn_020c0000.cpp]...\r\n";
            set_text(log_e, widen(log_buf));
        } else if (preset == SetupPreset::Step3Compile) {
            building = true;
            elapsed_sec = 94;
            prog.stage = setupwizard::BuildStage::CompileCppToExe;
            prog.recompile_step = 4;
            prog.recompile_total = 4;
            prog.sub_done = 412;
            prog.sub_total = 1267;
            prog.stage_percent = 32;
            prog.overall_percent = 63;
            prog.current_item = "fn_02340000.cpp.obj";
            prog.phase_text = "Step 2/2: Compiling C++ to native executable (412 / 1267, 32%)...";
            log_buf =
                "[recomp 4/4] Writing dispatch table & HLE stubs (100%)\r\n"
                "-- Configuring done (0.8s)\r\n"
                "-- Generating done (0.3s)\r\n"
                "[128/1267] Building CXX object CMakeFiles/BotWRecomp.dir/generated/fn_02100000.cpp.obj\r\n"
                "[256/1267] Building CXX object CMakeFiles/BotWRecomp.dir/generated/fn_02200000.cpp.obj\r\n"
                "[412/1267] Building CXX object CMakeFiles/BotWRecomp.dir/generated/fn_02340000.cpp.obj\r\n";
            set_text(log_e, widen(log_buf));
        } else {
            if (tc.has_exe) {
                prog.stage = setupwizard::BuildStage::Complete;
                prog.stage_percent = 100;
                prog.overall_percent = 100;
                prog.phase_text = "Ready: BotWRecomp.exe is built and verified.";
                log_buf = "BotWRecomp.exe found at:\r\n  " + narrow(tc.exe_path.wstring()) +
                          "\r\n\r\nClick 'Rebuild Wii U -> C++ & Executable' below if you want to regenerate generated/*.cpp from U-King.rpx and recompile.\r\n";
            } else {
                prog.stage = setupwizard::BuildStage::Idle;
                prog.phase_text = "Ready to convert Wii U U-King.rpx to C++ and compile BotWRecomp.exe.";
                log_buf = "Pipeline stages:\r\n"
                          "  Stage 1/2: tools/recompile.py reads code\\U-King.rpx and emits 258 C++ shards into generated/*.cpp\r\n"
                          "  Stage 2/2: CMake + Ninja compiles generated/*.cpp + runtime into BotWRecomp.exe\r\n";
            }
            set_text(log_e, widen(log_buf));
        }

        SetTimer(hwnd, 2, 200, nullptr);
        update_step_visibility();
    }

    void refresh_toolchain() {
        tc = setupwizard::inspect_toolchain(exe_dir(), std::filesystem::path(g.cfg.exe));
        if (g.cfg.exe.empty() && tc.has_exe) {
            g.cfg.exe = tc.exe_path.wstring();
        }
        if (build_start_b) {
            set_text(build_start_b, tc.has_exe ? L"Rebuild Wii U -> C++ & Executable"
                                               : L"Convert Wii U to C++ & Build Executable");
        }
    }

    void inspect_game_folder() {
        std::wstring gp = get_text(game_e);
        ginfo = gameinfo::read(std::filesystem::path(gp));
        if (ginfo.has_rpx) {
            rpx_ver = gameinfo::verify_rpx(std::filesystem::path(gp));
        } else {
            rpx_ver = {};
            rpx_ver.summary = gp.empty() ? "No game folder selected yet."
                                         : "code\\U-King.rpx not found in this folder.";
        }
    }

    void sync_pref_buttons() {
        ui::set_button_kind(scale_1x, prefs.scale == 1 ? ui::Kind::Primary : ui::Kind::Normal);
        ui::set_button_kind(scale_2x, prefs.scale == 2 ? ui::Kind::Primary : ui::Kind::Normal);
        ui::set_button_kind(scale_3x, prefs.scale >= 3 ? ui::Kind::Primary : ui::Kind::Normal);

        ui::set_button_kind(fps_30, prefs.fps < 45 ? ui::Kind::Primary : ui::Kind::Normal);
        ui::set_button_kind(fps_60, prefs.fps >= 45 ? ui::Kind::Primary : ui::Kind::Normal);

        ui::set_button_kind(lay_auto, prefs.layout < 0 ? ui::Kind::Primary : ui::Kind::Normal);
        ui::set_button_kind(lay_xbox, prefs.layout == 1 ? ui::Kind::Primary : ui::Kind::Normal);
        ui::set_button_kind(lay_nin, prefs.layout == 0 ? ui::Kind::Primary : ui::Kind::Normal);

        ui::set_button_kind(mode_merged_b, !separate_mode ? ui::Kind::Primary : ui::Kind::Normal);
        ui::set_button_kind(mode_sep_b, separate_mode ? ui::Kind::Primary : ui::Kind::Normal);
    }

    void update_step_visibility() {
        auto show = [](HWND h, bool on) {
            if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
        };
        show(user_e, step == 0);
        show(user_b, step == 0);
        show(tc_refresh_b, step == 0);
        show(tc_install_b, step == 0 && (!tc.has_python || !tc.has_compiler));

        show(mode_merged_b, step == 1);
        show(mode_sep_b, step == 1);
        show(autodetect_b, step == 1);
        show(game_e, step == 1 && !separate_mode);
        show(game_b, step == 1 && !separate_mode);
        show(base_e, step == 1 && separate_mode);
        show(base_b, step == 1 && separate_mode);
        show(upd_e, step == 1 && separate_mode);
        show(upd_b, step == 1 && separate_mode);
        show(dlc_e, step == 1 && separate_mode);
        show(dlc_b, step == 1 && separate_mode);
        show(merge_b, step == 1 && separate_mode);

        show(build_start_b, step == 2);
        show(build_cancel_b, step == 2);
        show(exe_pick_b, step == 2);
        show(log_e, step == 2);
        EnableWindow(build_start_b, !building);
        EnableWindow(build_cancel_b, building);
        EnableWindow(exe_pick_b, !building);

        show(scale_1x, step == 3);
        show(scale_2x, step == 3);
        show(scale_3x, step == 3);
        show(fps_30, step == 3);
        show(fps_60, step == 3);
        show(lay_auto, step == 3);
        show(lay_xbox, step == 3);
        show(lay_nin, step == 3);
        show(chk_fs, step == 3);
        show(chk_gpv, step == 3);
        show(chk_gyro, step == 3);
        show(imp_save_b, step == 3);

        EnableWindow(back_b, step > 0 && !building);
        set_text(next_b, step == 3 ? L"Finish & Open Launcher" : L"Next");
        EnableWindow(next_b, !building);
        relayout();
    }

    void layout(int w, int h) override {
        int m = S(24), field = S(32), gap = S(8);
        int top = S(82);

        if (step == 0) {
            int y = top + S(52);
            int bw = ui::button_width(user_b);
            ui::place_field(user_e, {m, y, w - m - bw - gap, y + field});
            MoveWindow(user_b, w - m - bw, y, bw, field, TRUE);

            int cy = h - S(bar) - S(52);
            place_row({tc_refresh_b, tc_install_b}, m, cy, field, gap);
        } else if (step == 1) {
            int y = top + S(32);
            place_row({mode_merged_b, mode_sep_b}, m, y, field, gap);
            place_row_right({autodetect_b}, w - m, y, field, gap);
            y += field + S(16);

            if (!separate_mode) {
                y += S(38);
                int bw = ui::button_width(game_b);
                ui::place_field(game_e, {m, y, w - m - bw - gap, y + field});
                MoveWindow(game_b, w - m - bw, y, bw, field, TRUE);
            } else {
                HWND edits[3] = {base_e, upd_e, dlc_e};
                HWND btns[3] = {base_b, upd_b, dlc_b};
                for (int i = 0; i < 3; i++) {
                    y += S(22);
                    int bw = ui::button_width(btns[i]);
                    ui::place_field(edits[i], {m, y, w - m - bw - gap, y + field});
                    MoveWindow(btns[i], w - m - bw, y, bw, field, TRUE);
                    y += field + S(8);
                }
                int mw = ui::button_width(merge_b);
                MoveWindow(merge_b, m, y + S(4), mw, field, TRUE);
            }
        } else if (step == 2) {
            int btn_y = top + S(198);
            int x = place_row({build_start_b, build_cancel_b}, m, btn_y, field, gap);
            (void)x;
            place_row_right({exe_pick_b}, w - m, btn_y, field, gap);
            int log_top = btn_y + field + S(12);
            int log_bot = std::max(log_top + S(60), h - S(bar) - S(16));
            ui::place_framed(log_e, {m, log_top, w - m, log_bot});
        } else if (step == 3) {
            int y = top + S(58);
            place_row({scale_1x, scale_2x, scale_3x}, m, y, field, gap);
            y += field + S(36);
            place_row({fps_30, fps_60}, m, y, field, gap);
            y += field + S(36);
            place_row({lay_auto, lay_xbox, lay_nin}, m, y, field, gap);
            y += field + S(24);
            MoveWindow(chk_fs, m, y, ui::button_width(chk_fs), S(24), TRUE);
            MoveWindow(chk_gyro, m + S(220), y, ui::button_width(chk_gyro), S(24), TRUE);
            y += S(28);
            MoveWindow(chk_gpv, m, y, ui::button_width(chk_gpv), S(24), TRUE);
            y += S(36);
            MoveWindow(imp_save_b, m, y, ui::button_width(imp_save_b), field, TRUE);
        }

        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row({close_b}, m, by, field, gap);
        place_row_right({next_b, back_b}, w - m, by, field, gap);
    }

    void draw_stepper(HDC dc, int w) {
        const auto& f = ui::fonts();
        int m = S(24);
        RECT hdr{0, 0, w, S(68)};
        ui::fill(dc, hdr, ui::pal.panel);
        ui::hline(dc, 0, w, S(68), ui::pal.line);

        const wchar_t* titles[4] = {
            L"1. Environment",
            L"2. Wii U Game Dump",
            L"3. Wii U \u2192 C++ & Build",
            L"4. Preferences & Finish",
        };
        int avail = w - 2 * m;
        int tab_w = (avail - 3 * S(8)) / 4;
        for (int i = 0; i < 4; i++) {
            int x0 = m + i * (tab_w + S(8));
            RECT tr{x0, S(14), x0 + tab_w, S(54)};
            bool active = (i == step);
            bool done = (i < step);
            COLORREF bg = active ? ui::pal.accent : done ? ui::pal.control : ui::pal.window;
            COLORREF border = active ? CLR_INVALID : ui::pal.control_line;
            COLORREF fg = active ? ui::pal.accent_text : done ? ui::pal.text : ui::pal.muted;
            ui::round_rect(dc, tr, S(5), bg, border);
            ui::draw_text(dc, titles[i], tr, active ? f.body_strong : f.small, fg,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    void draw_status_row(HDC dc, int x, int y, int w, const wchar_t* label, const std::wstring& val, Tone t) {
        const auto& f = ui::fonts();
        COLORREF dot = t == Tone::Good ? ui::pal.accent : t == Tone::Warn ? ui::pal.warn : t == Tone::Error ? ui::pal.error : ui::pal.muted;
        RECT dr{x, y + S(6), x + S(8), y + S(14)};
        ui::round_rect(dc, dr, S(8), dot, CLR_INVALID);
        RECT lr{x + S(16), y, x + S(210), y + S(20)};
        ui::draw_text(dc, label, lr, f.body_strong, ui::pal.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT vr{x + S(214), y, x + w, y + S(20)};
        ui::draw_text(dc, val, vr, f.body, ui::pal.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    void paint(HDC dc, const RECT& rc) override {
        const auto& f = ui::fonts();
        int w = rc.right, h = rc.bottom;
        int m = S(24);
        draw_stepper(dc, w);

        int top = S(82);
        if (step == 0) {
            ui::draw_text(dc, L"Saves, Controls & Logs Folder (BOTW_USER_ROOT)",
                          {m, top, w - m, top + S(22)}, f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            ui::draw_text(dc, L"Your save files, controls.ini, settings.ini, mods, and runtime.log are stored here.",
                          {m, top + S(22), w - m, top + S(44)}, f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            int cy = top + S(104);
            RECT card{m, cy, w - m, h - S(bar) - S(64)};
            ui::round_rect(dc, card, S(6), ui::pal.panel, ui::pal.line);

            int rx = m + S(16), ry = cy + S(14), rw = (w - 2 * m) - S(32);
            ui::draw_text(dc, L"System & Build Environment Checklist", {rx, ry, rx + rw, ry + S(22)},
                          f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            ry += S(30);

            draw_status_row(dc, rx, ry, rw, L"Project & Recompiler",
                            tc.has_repo ? (L"Ready (" + tc.repo_dir.wstring() + L")")
                                        : L"Standalone mode (place beside tools\\recompile.py to build from source)",
                            tc.has_repo ? Tone::Good : Tone::Warn);
            ry += S(28);

            draw_status_row(dc, rx, ry, rw, L"Generated C++ Shards",
                            tc.has_generated_cpp ? (L"Found " + std::to_wstring(tc.generated_cpp_count) + L" C++ files in generated\\")
                                                 : L"Not generated yet (Step 3 converts U-King.rpx \u2192 generated\\*.cpp)",
                            tc.has_generated_cpp ? Tone::Good : Tone::Warn);
            ry += S(28);

            draw_status_row(dc, rx, ry, rw, L"Native Game Binary",
                            tc.has_exe ? (L"Found (" + tc.exe_path.wstring() + L")")
                                       : L"Not compiled yet (Step 3 compiles C++ \u2192 BotWRecomp.exe)",
                            tc.has_exe ? Tone::Good : Tone::Warn);
            ry += S(28);

            draw_status_row(dc, rx, ry, rw, L"Python 3 Runtime",
                            tc.has_python ? (L"Ready (" + tc.python_exe.wstring() + L")")
                                          : L"Not found (needed to convert U-King.rpx \u2192 C++)",
                            tc.has_python ? Tone::Good : Tone::Warn);
            ry += S(28);

            draw_status_row(dc, rx, ry, rw, L"MSYS2 C++ Toolchain",
                            tc.has_compiler ? (L"Ready (g++, cmake, ninja in " + tc.gxx_exe.parent_path().wstring() + L")")
                                            : L"Not found (install MSYS2 UCRT64 to compile C++ shards)",
                            tc.has_compiler ? Tone::Good : Tone::Warn);
            ry += S(28);

            draw_status_row(dc, rx, ry, rw, L"Vulkan Graphics Driver",
                            tc.has_vulkan ? L"Ready (vulkan-1.dll detected)" : L"vulkan-1.dll not detected",
                            tc.has_vulkan ? Tone::Good : Tone::Error);
        } else if (step == 1) {
            ui::draw_text(dc, L"Select Your Wii U Game Dump (USA v1.5.0 / Update v208)",
                          {m, top, w - m, top + S(24)}, f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            int y = top + S(80);
            if (!separate_mode) {
                ui::draw_text(dc, L"Game folder (containing code\\U-King.rpx, content\\, and meta\\meta.xml)",
                              {m, y, w - m, y + S(20)}, f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                ui::draw_text(dc, L"Tip: Have separate Base, Update (v208), and DLC folders from Dumpling? Click 'Auto-detect dumps...' above.",
                              {m, y + S(18), w - m, y + S(36)}, f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            } else {
                const wchar_t* lbls[3] = {
                    L"Base Game Folder (00050000101C9400, v0)",
                    L"Update v1.5.0 Folder (0005000E101C9400, v208)  \u2014  required",
                    L"DLC Folder (0005000C101C9400, v80)  \u2014  optional",
                };
                int ly = y;
                for (int i = 0; i < 3; i++) {
                    ui::draw_text(dc, lbls[i], {m, ly, w - m, ly + S(20)}, f.small, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                    ly += S(22) + S(32) + S(8);
                }
                if (!merge_note.empty()) {
                    COLORREF mc = merge_tone == Tone::Good ? ui::pal.accent : merge_tone == Tone::Error ? ui::pal.error : ui::pal.muted;
                    ui::draw_text(dc, merge_note, {m + S(210), ly + S(8), w - m, ly + S(32)}, f.small, mc,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
            }

            // Verification card at bottom of Step 2
            int card_top = h - S(bar) - S(148);
            RECT card{m, card_top, w - m, h - S(bar) - S(14)};
            ui::round_rect(dc, card, S(6), ui::pal.panel, ui::pal.line);

            int ix = m + S(14), iy = card_top + S(14), isz = S(84);
            RECT ir{ix, iy, ix + isz, iy + isz};
            if (!ginfo.icon.empty()) {
                ui::draw_bitmap(dc, ir, ginfo.icon.bgra.data(), ginfo.icon.width, ginfo.icon.height);
            } else {
                ui::round_rect(dc, ir, S(6), ui::pal.window, ui::pal.line);
                ui::draw_text(dc, L"No icon", ir, f.small, ui::pal.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            int tx = ir.right + S(16), tw = (w - m - S(14)) - tx;
            std::wstring title = ginfo.long_name.empty() ? L"No game dump verified yet" : widen(ginfo.long_name);
            for (auto& ch : title) if (ch == L'\n') ch = L' ';
            ui::draw_text(dc, title, {tx, iy, tx + tw, iy + S(22)}, f.body_strong, ui::pal.text,
                          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            std::wstring meta_line;
            if (ginfo.folder_exists) {
                meta_line = L"Title ID: " + (ginfo.title_id.empty() ? L"Unknown" : widen(ginfo.title_id));
                std::string reg = gameinfo::region_name(ginfo.title_id);
                if (!reg.empty()) meta_line += L" (" + widen(reg) + L")";
                if (ginfo.has_title_version) meta_line += L"   \u00B7   Version: " + widen(gameinfo::version_name(ginfo.title_version)) + L" (v" + std::to_wstring(ginfo.title_version) + L")";
                meta_line += ginfo.has_dlc ? L"   \u00B7   DLC: Installed" : L"   \u00B7   DLC: Not found";
            } else {
                meta_line = L"Point to a folder with code\\U-King.rpx, content\\, and meta\\meta.xml.";
            }
            ui::draw_text(dc, meta_line, {tx, iy + S(24), tx + tw, iy + S(44)}, f.small, ui::pal.muted,
                          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            std::wstring sha_line = L"U-King.rpx SHA-1: ";
            if (!rpx_ver.sha1.empty()) {
                sha_line += widen(rpx_ver.sha1);
                sha_line += rpx_ver.hash_match ? L"  (Verified v208)" : L"  (Hash mismatch)";
            } else {
                sha_line += L"not checked";
            }
            ui::draw_text(dc, sha_line, {tx, iy + S(46), tx + tw, iy + S(66)}, f.mono, ui::pal.muted,
                          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            COLORREF sc = rpx_ver.hash_match ? ui::pal.accent : (ginfo.has_rpx ? ui::pal.warn : ui::pal.muted);
            ui::draw_text(dc, widen(rpx_ver.summary), {tx, iy + S(70), tx + tw, iy + S(112)}, f.small, sc,
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        } else if (step == 2) {
            setupwizard::BuildProgress snap;
            {
                std::lock_guard<std::mutex> lk(build_mu);
                snap = prog;
            }
            ui::draw_text(dc, L"Convert Wii U Binary (U-King.rpx) to C++ & Compile Native Executable",
                          {m, top, w - m, top + S(22)}, f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            // Two stage cards side by side
            int cw = (w - 2 * m - S(12)) / 2;
            int cy = top + S(28), ch = S(62);
            RECT c1{m, cy, m + cw, cy + ch};
            RECT c2{m + cw + S(12), cy, w - m, cy + ch};

            bool s1_active = (snap.stage == setupwizard::BuildStage::RecompileWiiUToCpp);
            bool s1_done = (snap.stage == setupwizard::BuildStage::CompileCppToExe || snap.stage == setupwizard::BuildStage::Complete || tc.has_generated_cpp);
            bool s2_active = (snap.stage == setupwizard::BuildStage::CompileCppToExe);
            bool s2_done = (snap.stage == setupwizard::BuildStage::Complete || tc.has_exe);

            ui::round_rect(dc, c1, S(6), ui::pal.panel, s1_active ? ui::pal.accent : ui::pal.line, s1_active ? S(2) : 1);
            ui::round_rect(dc, c2, S(6), ui::pal.panel, s2_active ? ui::pal.accent : ui::pal.line, s2_active ? S(2) : 1);

            ui::draw_text(dc, L"Stage 1/2  \u00B7  Wii U (U-King.rpx) \u2192 C++",
                          {c1.left + S(12), c1.top + S(8), c1.right - S(12), c1.top + S(28)},
                          f.body_strong, s1_active ? ui::pal.accent : ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            std::wstring s1_sub = s1_active
                ? (L"Generating C++ shard " + std::to_wstring(snap.sub_done) + L" / " + std::to_wstring(snap.sub_total) +
                   L" (" + widen(snap.current_item) + L")")
                : (s1_done ? L"258 C++ shards generated in generated/*.cpp"
                           : L"Decompresses RPX, analyzes 66,137 PPC funcs, emits C++");
            ui::draw_text(dc, s1_sub, {c1.left + S(12), c1.top + S(30), c1.right - S(12), c1.bottom - S(6)},
                          f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            ui::draw_text(dc, L"Stage 2/2  \u00B7  Compile C++ \u2192 BotWRecomp.exe",
                          {c2.left + S(12), c2.top + S(8), c2.right - S(12), c2.top + S(28)},
                          f.body_strong, s2_active ? ui::pal.accent : ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            std::wstring s2_sub = s2_active
                ? (L"Compiling object " + std::to_wstring(snap.sub_done) + L" / " + std::to_wstring(snap.sub_total) +
                   L" (" + widen(snap.current_item) + L")")
                : (s2_done ? L"Native x86_64 executable ready (BotWRecomp.exe)"
                           : L"Compiles C++ shards + runtime with CMake & Ninja");
            ui::draw_text(dc, s2_sub, {c2.left + S(12), c2.top + S(30), c2.right - S(12), c2.bottom - S(6)},
                          f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Progress bar panel
            int py = cy + ch + S(12), ph = S(84);
            RECT pcard{m, py, w - m, py + ph};
            ui::round_rect(dc, pcard, S(6), ui::pal.panel, ui::pal.line);

            std::wstring phase = widen(snap.phase_text);
            ui::draw_text(dc, phase, {m + S(14), py + S(10), w - m - S(120), py + S(30)},
                          f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            wchar_t tbuf[64];
            swprintf(tbuf, 64, L"%d%%   \u00B7   %02lu:%02lu", snap.stage_percent, elapsed_sec / 60, elapsed_sec % 60);
            ui::draw_text(dc, tbuf, {w - m - S(130), py + S(10), w - m - S(14), py + S(30)},
                          f.body_strong, ui::pal.accent, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            // Progress track && fill
            RECT bar_r{m + S(14), py + S(36), w - m - S(14), py + S(56)};
            ui::round_rect(dc, bar_r, S(4), ui::pal.field, ui::pal.field_line);
            int pct = std::clamp(snap.stage_percent, 0, 100);
            if (pct > 0) {
                int fill_w = std::max(S(8), static_cast<int>((bar_r.right - bar_r.left) * pct / 100));
                RECT fill_r{bar_r.left, bar_r.top, std::min(bar_r.right, bar_r.left + fill_w), bar_r.bottom};
                COLORREF bar_col = snap.stage == setupwizard::BuildStage::Failed ? ui::pal.error : ui::pal.accent;
                ui::round_rect(dc, fill_r, S(4), bar_col, CLR_INVALID);
            }

            std::wstring detail;
            if (snap.stage == setupwizard::BuildStage::RecompileWiiUToCpp) {
                detail = L"Current output: generated\\" + widen(snap.current_item) +
                         L"   \u00B7   Overall pipeline progress: " + std::to_wstring(snap.overall_percent) + L"%";
            } else if (snap.stage == setupwizard::BuildStage::CompileCppToExe) {
                detail = L"Current target: " + widen(snap.current_item) +
                         L"   \u00B7   Overall pipeline progress: " + std::to_wstring(snap.overall_percent) + L"%";
            } else {
                detail = L"Live output log below (window stays 100% responsive during conversion & compilation):";
            }
            ui::draw_text(dc, detail, {m + S(14), py + S(60), w - m - S(14), py + S(80)},
                          f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else if (step == 3) {
            ui::draw_text(dc, L"Quick Graphics, Controller & Save Setup",
                          {m, top, w - m, top + S(24)}, f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            ui::draw_text(dc, L"These preferences are saved to settings.ini and BotWLauncher.ini. You can change them anytime.",
                          {m, top + S(22), w - m, top + S(42)}, f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            int y = top + S(38);
            ui::draw_text(dc, L"Internal Resolution Scale", {m, y, w - m, y + S(20)},
                          f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            y += S(32) + S(36);
            ui::draw_text(dc, L"Frame Rate Target", {m, y - S(20), w - m, y},
                          f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            y += S(32) + S(36);
            ui::draw_text(dc, L"Controller Button Prompts & Layout", {m, y - S(20), w - m, y},
                          f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            int sy = h - S(bar) - S(62);
            RECT scard{m, sy, w - m, h - S(bar) - S(14)};
            ui::round_rect(dc, scard, S(6), ui::pal.panel, ui::pal.line);
            std::wstring summary = L"Ready to launch:  Game = " + (g.cfg.game.empty() ? L"(not set)" : g.cfg.game) +
                                   L"   \u00B7   Exe = " + (g.cfg.exe.empty() ? L"(not set)" : g.cfg.exe);
            if (!save_note.empty()) summary = save_note;
            ui::draw_text(dc, summary, {m + S(14), sy + S(14), w - m - S(14), h - S(bar) - S(24)},
                          f.small, ui::pal.accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    void append_log(const std::string& line) {
        std::lock_guard<std::mutex> lk(build_mu);
        setupwizard::parse_progress_line(line, prog);
        log_buf += line + "\r\n";
        if (log_buf.size() > 64 * 1024) {
            log_buf.erase(0, log_buf.size() - 48 * 1024);
        }
        log_dirty = true;
    }

    bool run_command_piped(const std::wstring& cmdline, const std::filesystem::path& cwd, const std::wstring& env_block) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        HANDLE rpipe = nullptr, wpipe = nullptr;
        if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return false;
        SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = wpipe;
        si.hStdError = wpipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
        mutable_cmd.push_back(L'\0');
        std::wstring env = env_block;

        BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                 env.empty() ? nullptr : env.data(),
                                 cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
        CloseHandle(wpipe);
        if (!ok) {
            CloseHandle(rpipe);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(build_mu);
            child_proc = pi.hProcess;
        }

        std::string pending;
        char buf[1024];
        DWORD nread = 0;
        while (ReadFile(rpipe, buf, sizeof(buf), &nread, nullptr) && nread > 0) {
            if (cancel_req) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            pending.append(buf, nread);
            size_t pos = 0;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pending.erase(0, pos + 1);
                append_log(line);
            }
        }
        if (!pending.empty()) append_log(pending);
        CloseHandle(rpipe);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        {
            std::lock_guard<std::mutex> lk(build_mu);
            child_proc = nullptr;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return !cancel_req && code == 0;
    }

    void start_pipeline() {
        if (building) return;
        refresh_toolchain();
        std::wstring gp = get_text(game_e);
        if (gp.empty()) gp = g.cfg.game;

        if (!tc.has_repo) {
            alert(L"Could not find tools\\recompile.py and CMakeLists.txt next to the launcher.\n"
                  L"If you already have a compiled BotWRecomp.exe, click 'Use existing BotWRecomp.exe...'.");
            return;
        }
        std::filesystem::path rpx_path = std::filesystem::path(gp) / "code" / "U-King.rpx";
        std::error_code ec;
        bool has_rpx = !gp.empty() && std::filesystem::is_regular_file(rpx_path, ec);
        if (!has_rpx && !tc.has_generated_cpp) {
            alert(L"Please select a valid Wii U game folder containing code\\U-King.rpx in Step 2 first.");
            return;
        }
        if (!tc.has_python) {
            alert(L"Python 3 was not found. Click 'Back' to Step 1 and install the MSYS2 toolchain first.");
            return;
        }

        stop_worker();
        cancel_req = false;
        building = true;
        build_start_tick = GetTickCount();
        elapsed_sec = 0;
        {
            std::lock_guard<std::mutex> lk(build_mu);
            log_buf.clear();
            prog = {};
            prog.stage = setupwizard::BuildStage::RecompileWiiUToCpp;
            prog.phase_text = "Starting Wii U (U-King.rpx) -> C++ recompiler...";
            log_dirty = true;
        }
        update_step_visibility();

        std::filesystem::path repo = tc.repo_dir;
        std::filesystem::path py = tc.python_exe;
        std::filesystem::path cmake = tc.cmake_exe;
        std::filesystem::path msys = tc.msys_bin;
        std::filesystem::path user_p = std::filesystem::path(get_text(user_e));
        std::wstring env = build_env(user_p, repo / "build" / "BotWRecomp.exe");

        worker = std::thread([this, repo, py, cmake, msys, rpx_path, has_rpx, env]() {
            bool ok = true;
            if (has_rpx) {
                std::wstring cmd = L"\"" + py.wstring() + L"\" -I \"" + (repo / "tools" / "recompile.py").wstring() +
                                   L"\" \"" + rpx_path.wstring() + L"\" \"" +
                                   (repo / "generated").wstring() + L"\"";
                append_log("> " + narrow(cmd));
                ok = run_command_piped(cmd, repo, env);
            } else {
                append_log("[recomp 4/4] Using existing generated/*.cpp shards (100%)");
            }

            if (ok && !cancel_req) {
                if (!cmake.empty()) {
                    std::filesystem::path bdir = repo / "build";
                    std::wstring cfg_cmd = L"\"" + cmake.wstring() + L"\" -B \"" + bdir.wstring() +
                                           L"\" -G Ninja -DCMAKE_BUILD_TYPE=Release";
                    append_log("> " + narrow(cfg_cmd));
                    ok = run_command_piped(cfg_cmd, repo, env);
                    if (ok && !cancel_req) {
                        std::wstring bld_cmd = L"\"" + cmake.wstring() + L"\" --build \"" + bdir.wstring() + L"\"";
                        append_log("> " + narrow(bld_cmd));
                        ok = run_command_piped(bld_cmd, repo, env);
                    }
                } else {
                    append_log("Note: CMake/Ninja not found; Wii U -> C++ conversion finished.");
                }
            }

            std::lock_guard<std::mutex> lk(build_mu);
            if (cancel_req) {
                prog.stage = setupwizard::BuildStage::Cancelled;
                prog.phase_text = "Build cancelled by user.";
                log_buf += "\r\n[Cancelled]\r\n";
            } else if (ok) {
                prog.stage = setupwizard::BuildStage::Complete;
                prog.stage_percent = 100;
                prog.overall_percent = 100;
                prog.phase_text = "Complete: Wii U binary converted to C++ and compiled to BotWRecomp.exe (100%)";
                log_buf += "\r\n[Success] BotWRecomp.exe is ready.\r\n";
                setupwizard::deploy_runtime_files(repo / "build", repo);
            } else {
                prog.stage = setupwizard::BuildStage::Failed;
                prog.phase_text = "Build failed. Check the log below for details.";
                log_buf += "\r\n[Failed]\r\n";
            }
            log_dirty = true;
            building = false;
        });
    }

    void timer() override {
        if (building && build_start_tick != 0) {
            elapsed_sec = (GetTickCount() - build_start_tick) / 1000;
        }
        bool need_ui_sync = false;
        std::wstring new_log;
        {
            std::lock_guard<std::mutex> lk(build_mu);
            if (log_dirty) {
                new_log = widen(log_buf);
                log_dirty = false;
                need_ui_sync = true;
            }
        }
        if (need_ui_sync) {
            if (log_e) {
                set_text(log_e, new_log);
                int n = GetWindowTextLengthW(log_e);
                SendMessageW(log_e, EM_SETSEL, n, n);
                SendMessageW(log_e, EM_SCROLLCARET, 0, 0);
            }
            if (!building) {
                refresh_toolchain();
                update_step_visibility();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (building && step == 2) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void on_autodetect() {
        auto root = pick_browse(hwnd, L"Select the parent folder containing your Wii U dump (or Base + Update + DLC folders).", false);
        if (root.empty()) return;
        auto lay = gameinfo::discover_dumps(std::filesystem::path(root));
        if (g.cfg.no_dlc && lay.needs_merge)
            gameinfo::validate_dump_pair(lay.base_folder, lay.update_folder, {}, lay.validation_error);
        if (!lay.needs_merge && lay.ready()) {
            separate_mode = false;
            set_text(game_e, lay.merged_game.wstring());
            g.cfg.game = lay.merged_game.wstring();
            inspect_game_folder();
            sync_pref_buttons();
            update_step_visibility();
            return;
        }
        if (lay.needs_merge) {
            separate_mode = true;
            if (!lay.base_folder.empty()) set_text(base_e, lay.base_folder.wstring());
            if (!lay.update_folder.empty()) set_text(upd_e, lay.update_folder.wstring());
            if (!lay.dlc_folder.empty()) set_text(dlc_e, lay.dlc_folder.wstring());
            merge_note = widen(lay.summary());
            merge_tone = lay.ready() ? Tone::Good : Tone::Warn;
            sync_pref_buttons();
            update_step_visibility();
            return;
        }
        alert(widen(lay.summary()));
    }

    void on_merge() {
        std::wstring bp = get_text(base_e), up = get_text(upd_e), dp = g.cfg.no_dlc ? L"" : get_text(dlc_e);
        if (bp.empty() || up.empty()) {
            alert(L"Select both the Base Game folder (v0) and Update v1.5.0 folder (v208) before merging.");
            return;
        }
        std::filesystem::path target = std::filesystem::path(bp).parent_path() / L"BotW_Merged_v208";
        std::string err;
        merge_note = L"Merging Base + Update + DLC via hard links...";
        merge_tone = Tone::Normal;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);

        if (!gameinfo::merge_dumps(bp, up, dp, target, err)) {
            merge_note = L"Merge failed: " + widen(err);
            merge_tone = Tone::Error;
            alert(merge_note);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        merge_note = L"Merged into " + target.wstring();
        merge_tone = Tone::Good;
        separate_mode = false;
        set_text(game_e, target.wstring());
        g.cfg.game = target.wstring();
        inspect_game_folder();
        sync_pref_buttons();
        update_step_visibility();
    }

    void on_install_toolchain() {
        const wchar_t* cmd =
            L"/c echo Installing MSYS2 UCRT64 toolchain for BotW-Native... & "
            L"winget install -e --id MSYS2.MSYS2 & "
            L"C:\\msys64\\usr\\bin\\pacman.exe -Sy --needed --noconfirm "
            L"mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake "
            L"mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL3 python & pause";
        ShellExecuteW(hwnd, L"open", L"cmd.exe", cmd, nullptr, SW_SHOWNORMAL);
    }

    void save_all_and_finish() {
        std::wstring us = get_text(user_e);
        std::wstring ga = get_text(game_e);
        if (!us.empty()) g.cfg.user = us;
        if (!ga.empty()) g.cfg.game = ga;
        if (tc.has_exe && g.cfg.exe.empty()) g.cfg.exe = tc.exe_path.wstring();
        g.cfg.fullscreen = ui::checked(chk_fs);
        g.cfg.window_mode = g.cfg.fullscreen ? (g.cfg.window_mode ? g.cfg.window_mode : 1) : 0;
        g.cfg.gamepad_view = ui::checked(chk_gpv);
        g.cfg.setup_done = true;
        prefs.gamepad_view = g.cfg.gamepad_view;
        prefs.gyro = ui::checked(chk_gyro);
        setupwizard::write_game_prefs(user_root(), prefs);
        save_launcher_ini();
        load_controls();
        refresh_game();
        note_legacy_save();
        close();
    }

    void command(int id, int code, HWND) override {
        switch (id) {
            case ID_WIZ_USER_BROWSE: {
                auto p = pick_browse(hwnd, L"User data folder (BOTW_USER_ROOT). Saves, controls.ini, and logs go here.", false);
                if (!p.empty()) set_text(user_e, p);
                break;
            }
            case ID_WIZ_TOOLCHAIN_REFRESH:
                refresh_toolchain();
                update_step_visibility();
                break;
            case ID_WIZ_TOOLCHAIN_INSTALL:
                on_install_toolchain();
                break;
            case ID_WIZ_MODE_MERGED:
                separate_mode = false;
                sync_pref_buttons();
                update_step_visibility();
                break;
            case ID_WIZ_MODE_SEPARATE:
                separate_mode = true;
                sync_pref_buttons();
                update_step_visibility();
                break;
            case ID_WIZ_AUTODETECT:
                on_autodetect();
                break;
            case ID_WIZ_GAME:
                if (code == EN_CHANGE) {
                    inspect_game_folder();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                break;
            case ID_WIZ_GAME_BROWSE: {
                auto p = pick_browse(hwnd, L"Folder that contains code, content, and meta from your v1.5.0 dump.", false);
                if (!p.empty()) {
                    set_text(game_e, p);
                    g.cfg.game = p;
                    inspect_game_folder();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                break;
            }
            case ID_WIZ_BASE_BROWSE: {
                auto p = pick_browse(hwnd, L"Select Base Game folder (00050000101C9400, v0).", false);
                if (!p.empty()) set_text(base_e, p);
                break;
            }
            case ID_WIZ_UPD_BROWSE: {
                auto p = pick_browse(hwnd, L"Select Update v1.5.0 folder (0005000E101C9400, v208).", false);
                if (!p.empty()) set_text(upd_e, p);
                break;
            }
            case ID_WIZ_DLC_BROWSE: {
                auto p = pick_browse(hwnd, L"Select DLC folder (0005000C101C9400, v80).", false);
                if (!p.empty()) set_text(dlc_e, p);
                break;
            }
            case ID_WIZ_MERGE:
                on_merge();
                break;
            case ID_WIZ_BUILD_START:
                start_pipeline();
                break;
            case ID_WIZ_BUILD_CANCEL:
                cancel_req = true;
                {
                    std::lock_guard<std::mutex> lk(build_mu);
                    if (child_proc) TerminateProcess(child_proc, 1);
                }
                break;
            case ID_WIZ_EXE_BROWSE: {
                auto p = pick_exe(hwnd, g.cfg.exe);
                if (!p.empty()) {
                    g.cfg.exe = p;
                    refresh_toolchain();
                    std::lock_guard<std::mutex> lk(build_mu);
                    prog.stage = setupwizard::BuildStage::Complete;
                    prog.stage_percent = 100;
                    prog.overall_percent = 100;
                    prog.phase_text = "Ready: Selected existing BotWRecomp.exe.";
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                break;
            }
            case ID_WIZ_SCALE_1X: prefs.scale = 1; sync_pref_buttons(); break;
            case ID_WIZ_SCALE_2X: prefs.scale = 2; sync_pref_buttons(); break;
            case ID_WIZ_SCALE_3X: prefs.scale = 3; sync_pref_buttons(); break;
            case ID_WIZ_FPS_30: prefs.fps = 30; sync_pref_buttons(); break;
            case ID_WIZ_FPS_60: prefs.fps = 60; sync_pref_buttons(); break;
            case ID_WIZ_LAYOUT_AUTO: prefs.layout = -1; sync_pref_buttons(); break;
            case ID_WIZ_LAYOUT_XBOX: prefs.layout = 1; sync_pref_buttons(); break;
            case ID_WIZ_LAYOUT_NINTENDO: prefs.layout = 0; sync_pref_buttons(); break;
            case ID_WIZ_IMPORT_SAVE: {
                std::wstring us = get_text(user_e);
                if (!us.empty()) g.cfg.user = us;
                std::wstring picked = pick_browse(hwnd, L"Select a Cemu mlc01 folder, save folder, or .zip to import.", true);
                if (!picked.empty()) {
                    import_path(picked);
                    save_note = g.status;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                break;
            }
            case ID_WIZ_BACK:
                if (step > 0 && !building) {
                    step--;
                    update_step_visibility();
                }
                break;
            case ID_WIZ_NEXT:
                if (building) break;
                if (step == 0) {
                    std::wstring us = get_text(user_e);
                    if (!us.empty()) g.cfg.user = us;
                    step = 1;
                    inspect_game_folder();
                    update_step_visibility();
                } else if (step == 1) {
                    std::wstring ga = get_text(game_e);
                    if (!ga.empty()) g.cfg.game = ga;
                    step = 2;
                    refresh_toolchain();
                    update_step_visibility();
                } else if (step == 2) {
                    step = 3;
                    update_step_visibility();
                } else {
                    save_all_and_finish();
                }
                break;
            case ID_WIZ_CANCEL:
                cancel();
                break;
            default: break;
        }
    }

    void cancel() override {
        if (building) {
            if (ask(L"A conversion or build is currently running. Cancel it and close the Setup Assistant?",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return;
            }
            stop_worker();
        }
        KillTimer(hwnd, 2);
        close();
    }
};

void show_setup_wizard(SetupPreset preset) {
    SetupWizardWindow w(preset);
    run_dialog(&w, L"Setup Assistant", 780, 580, true);
}

void show_settings() {
    SettingsWindow w;
    run_dialog(&w, L"Settings", 600, 560, false);
}
void show_controls() {
    ControlsWindow w;
    run_dialog(&w, L"Controls", 640, 600, true);
}
void show_saves() {
    SavesWindow w;
    run_dialog(&w, L"Saves", 880, 480, true);
}
void show_mods() {
    ModsWindow w;
    run_dialog(&w, L"Mods", 860, 560, true);
    refresh_game();
}
void show_log() {
    LogWindow w;
    run_dialog(&w, L"runtime.log (last 250 lines)", 800, 540, true);
}

void on_play() {
    if (game_running()) {
        alert(L"BotWRecomp.exe is already running.");
        return;
    }
    if (g.cfg.game.empty()) {
        show_setup_wizard(SetupPreset::Step2);
        return;
    }
    std::wstring game = g.cfg.game, exe = g.cfg.exe, user = g.cfg.user;
    if (has_quote(game) || has_quote(exe) || has_quote(user)) {
        alert(L"Paths cannot contain a double quote.");
        return;
    }
    std::error_code ec;
    if (exe.empty() || !std::filesystem::is_regular_file(std::filesystem::path(exe), ec)) {
        alert(L"Set the path to BotWRecomp.exe in Settings.");
        return;
    }
    std::filesystem::path game_path(game);
    bool looks = std::filesystem::is_regular_file(game_path / "code" / "U-King.rpx", ec);
    if (!looks) {
        if (ask(L"That game folder does not contain code\\U-King.rpx.\nLaunch anyway?", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    if (user.empty()) {
        alert(L"Set the saves and logs folder in Settings. Saves and controls.ini go there.");
        return;
    }
    std::filesystem::create_directories(std::filesystem::path(user), ec);
    int seconds = g.cfg.seconds.empty() ? 0 : _wtoi(g.cfg.seconds.c_str());
    if (seconds < 0) seconds = 0;
    std::wstring cmd = L"\"" + exe + L"\" --game \"" + game + L"\" --renderer vulkan";
    if (seconds > 0) cmd += L" --seconds " + std::to_wstring(seconds);
    cmd += L" --window-mode " + std::to_wstring(g.cfg.window_mode);
    if (g.cfg.gamepad_view) cmd += L" --gamepad-view";
    if (g.cfg.no_mods) cmd += L" --no-mods";
    if (g.cfg.no_dlc) cmd += L" --no-dlc";
    if (!g.cfg.language.empty()) cmd += L" --language " + g.cfg.language;
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');
    std::wstring env = build_env(std::filesystem::path(user), std::filesystem::path(exe));
    std::wstring cwd = std::filesystem::path(exe).parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(), cwd.c_str(), &si, &pi);
    if (!ok) {
        DWORD e = GetLastError();
        wchar_t* msg = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                       reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
        std::wstring text = L"Could not start the game.";
        if (msg) {
            text += L"\n";
            text += msg;
            LocalFree(msg);
        }
        text += L"\nIf this was a missing DLL, start build\\BotWLauncher.cmd so C:\\msys64\\ucrt64\\bin is on PATH.";
        alert(text);
        set_status(L"Could not start the game.", Tone::Error);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    save_launcher_ini();
    set_status(L"Launched. Saves stay locked until BotWRecomp.exe closes.");
    g.was_running = true;
    if (g.saves_win) g.saves_win->sync();
    update_play_button();
}

// Main window.
constexpr int kMainW = 680, kMainH = 264;

struct MainWindow : Window {
    HWND play = nullptr, setup_b = nullptr, settings = nullptr, controls = nullptr, saves = nullptr, mods_b = nullptr, user_b = nullptr, log_b = nullptr;
    RECT icon_r{}, over_r{}, title_r{}, info_r{}, status_r{};
    int divider = 0;
    MainWindow() {
        is_main = true;
        bar = 36;
    }
    void create() override {
        play = ui::button(hwnd, ID_PLAY, L"Play", ui::Kind::Primary, false, true);
        setup_b = ui::button(hwnd, ID_SETUP_OPEN, L"Setup Wizard", ui::Kind::Flat);
        settings = ui::button(hwnd, ID_SETTINGS, L"Settings", ui::Kind::Flat);
        controls = ui::button(hwnd, ID_CONTROLS_OPEN, L"Controls", ui::Kind::Flat);
        saves = ui::button(hwnd, ID_SAVES_OPEN, L"Saves", ui::Kind::Flat);
        mods_b = ui::button(hwnd, ID_MODS_OPEN, L"Mods", ui::Kind::Flat);
        user_b = ui::button(hwnd, ID_USER_OPEN, L"Open user folder", ui::Kind::Flat);
        log_b = ui::button(hwnd, ID_LOG_OPEN, L"Open log", ui::Kind::Flat);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 1, 400, nullptr);
        SetFocus(play);
    }
    void layout(int w, int h) override {
        int m = S(24), icon = S(128);
        icon_r = {m, m, m + icon, m + icon};
        int x = icon_r.right + S(24);
        int pw = std::max(S(152), ui::button_width(play)), ph = S(40);
        int mid = (icon_r.top + icon_r.bottom) / 2;
        MoveWindow(play, w - m - pw, mid - ph / 2 - S(12), pw, ph, TRUE);
        int oh = ui::line_height(ui::fonts().overline), ih = ui::line_height(ui::fonts().body);
        over_r = {x, m + S(4), w - m - pw - S(24), m + S(4) + oh};
        info_r = {x, icon_r.bottom - ih, w - m, icon_r.bottom};
        title_r = {x, over_r.bottom + S(4), w - m - pw - S(24), info_r.top - S(4)};
        divider = icon_r.bottom + S(24);
        int ty = divider + S(8), th = S(32);
        place_row({setup_b, settings, controls, saves, mods_b}, m - S(12), ty, th, S(0));
        place_row_right({log_b, user_b}, w - m + S(12), ty, th, S(0));
        status_r = {m, h - S(bar), w - m, h};
    }
    void paint(HDC dc, const RECT& rc) override {
        const auto& f = ui::fonts();
        if (!g.info.icon.empty()) {
            ui::draw_bitmap(dc, icon_r, g.info.icon.bgra.data(), g.info.icon.width, g.info.icon.height);
        } else {
            ui::round_rect(dc, icon_r, S(8), ui::pal.panel, ui::pal.line);
            ui::draw_text(dc, L"No icon", icon_r, f.small, ui::pal.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        std::wstring over = L"WII U";
        if (!g.info.title_id.empty()) over += L"  \u00B7  " + widen(g.info.title_id);
        ui::draw_text(dc, over, over_r, f.overline, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        ui::draw_text(dc, nice_name(), title_r, f.title, ui::pal.text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!g.cfg.game.empty() && g.info.folder_exists)
            ui::draw_text(dc, info_line(), info_r, f.body, ui::pal.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ui::hline(dc, 0, rc.right, divider, ui::pal.line);
        COLORREF dot = g.tone == Tone::Good ? ui::pal.accent : g.tone == Tone::Warn ? ui::pal.warn : g.tone == Tone::Error ? ui::pal.error : ui::pal.muted;
        int cy = (status_r.top + status_r.bottom) / 2;
        RECT d{status_r.left, cy - S(4), status_r.left + S(8), cy + S(4)};
        ui::round_rect(dc, d, S(8), dot, CLR_INVALID);
        RECT t = status_r;
        t.left += S(16);
        ui::draw_text(dc, g.status, t, f.small, ui::pal.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_PLAY: on_play(); break;
            case ID_SETUP_OPEN: show_setup_wizard(); break;
            case ID_SETTINGS: show_settings(); break;
            case ID_CONTROLS_OPEN: show_controls(); break;
            case ID_SAVES_OPEN: show_saves(); break;
            case ID_MODS_OPEN: show_mods(); break;
            case ID_USER_OPEN:
                if (!user_root().empty()) open_folder(user_root());
                break;
            case ID_LOG_OPEN: show_log(); break;
            default: break;
        }
    }
    void ok() override {
        if (IsWindowEnabled(play)) on_play();
    }
    void cancel() override {}
    void dropped(const std::wstring& p) override {
        std::string folder;
        if (install_mod_path(p, &folder)) {
            alert(L"Installed and enabled mod \"" + widen(folder) + L"\".", MB_OK | MB_ICONINFORMATION);
        }
    }
    void on_close() override {
        save_launcher_ini();
        DestroyWindow(hwnd);
    }
    void timer() override {
        pump_sdl();
        if (g.in_capture) return;
        bool run = game_running();
        if (g.saves_win) g.saves_win->sync();
        if (run != g.was_running) {
            g.was_running = run;
            if (run) set_status(L"BotWRecomp.exe is running. Save import, switch, delete, and migrate are blocked until it closes.");
            else set_status(L"Game closed. Saves can be changed again.");
            update_play_button();
        }
    }
};

MainWindow* g_main = nullptr;

void update_play_button() {
    if (!g_main || !g_main->play) return;
    bool run = game_running();
    const wchar_t* label = g.cfg.game.empty() ? L"Choose game folder" : run ? L"Running" : L"Play";
    if (get_text(g_main->play) != label) {
        set_text(g_main->play, label);
        g_main->relayout();
    }
    EnableWindow(g_main->play, run ? FALSE : TRUE);
    InvalidateRect(g_main->play, nullptr, FALSE);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void set_process_dpi_awareness() {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    using CtxFn = BOOL(WINAPI*)(HANDLE);
    FARPROC raw = user ? GetProcAddress(user, "SetProcessDpiAwarenessContext") : nullptr;
    CtxFn fn = nullptr;
    memcpy(&fn, &raw, sizeof fn);
    if (fn && fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
    SetProcessDPIAware();
}

int system_dpi() {
    HDC dc = GetDC(nullptr);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d > 0 ? d : 96;
}

}  // namespace

int main(int argc, char** argv) {
    std::string win_arg;
    bool force_setup = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i] ? argv[i] : "";
        if (a == "--setup") force_setup = true;
        else if (a == "--window" && i + 1 < argc) win_arg = argv[++i];
        else if (a.rfind("--window=", 0) == 0) win_arg = a.substr(9);
    }
    SDL_SetMainReady();
    set_process_dpi_awareness();
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        g.sdl = true;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids) {
            for (int i = 0; i < n; i++) remember_pad(ids[i]);
            SDL_free(ids);
        }
    }
    g.inst = GetModuleHandleW(nullptr);
    load_launcher_ini();
    ui::startup();
    ui::set_theme(theme_pref());
    ui::set_dpi(system_dpi());
    ui::rebuild_fonts();
    register_classes();
    MainWindow main_win;
    g_main = &main_win;
    HWND hwnd = make_window(&main_win, kMainClass, kAppTitle, kMainW, kMainH, false, nullptr);
    if (!hwnd) return 1;
    g.hwnd = hwnd;
    int wd = ui::window_dpi(hwnd);
    if (wd != ui::dpi()) {
        apply_dpi_everywhere(wd);
        RECT r{0, 0, S(kMainW), S(kMainH)};
        AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    ui::center_on(hwnd, nullptr);
    refresh_game();
    note_legacy_save();
    main_win.relayout();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    if (!win_arg.empty()) {
        PostMessageW(hwnd, WM_APP + 10, 0, reinterpret_cast<LPARAM>(_strdup(win_arg.c_str())));
    } else if (force_setup || (!g.cfg.setup_done && (g.cfg.game.empty() || g.cfg.exe.empty()))) {
        PostMessageW(hwnd, WM_COMMAND, ID_SETUP_OPEN, 0);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        ui::note_key(msg);
        if (IsWindow(hwnd) && IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_main = nullptr;
    for (auto* p : g.pads) SDL_CloseGamepad(p);
    g.pads.clear();
    if (g.sdl) SDL_Quit();
    ui::shutdown();
    OleUninitialize();
    return 0;
}
