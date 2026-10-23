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

#include "input/controls_file.h"
#include "save_import.h"
#include "unzip.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

namespace {

enum {
    ID_GAME = 100,
    ID_GAME_BROWSE,
    ID_EXE,
    ID_EXE_BROWSE,
    ID_USER,
    ID_USER_BROWSE,
    ID_SECONDS,
    ID_FULLSCREEN,
    ID_PLAY,
    ID_LOG,
    ID_CONTROLS,
    ID_RESET,
    ID_SAVES,
    ID_IMPORT,
    ID_SWITCH,
    ID_DELETE,
    ID_OPEN,
    ID_REFRESH,
    ID_MIGRATE,
    ID_STATUS,
};

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND game{}, exe{}, user{}, seconds{}, fullscreen{};
    HWND controls{}, saves{}, status{};
    HWND import_btn{}, switch_btn{}, delete_btn{}, migrate_btn{};
    HFONT font = nullptr;
    int dpi = 96;
    bool sdl = false;
    bool in_capture = false;
    bool was_running = false;
    std::vector<SDL_Gamepad*> pads;
    std::vector<controlsfile::Binding> rows;
    std::vector<saveimport::SetInfo> sets;
};

App g;

template <class Fn>
Fn proc_address(HMODULE mod, const char* name) {
    static_assert(sizeof(Fn) == sizeof(FARPROC));
    FARPROC raw = GetProcAddress(mod, name);
    Fn fn = nullptr;
    memcpy(&fn, &raw, sizeof fn);
    return fn;
}

int dpi_of(HWND hwnd) {
    auto fn = proc_address<UINT(WINAPI*)(HWND)>(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && hwnd) return static_cast<int>(fn(hwnd));
    HDC dc = GetDC(hwnd);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi > 0 ? dpi : 96;
}

int S(int px) { return MulDiv(px, g.dpi, 96); }

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(wcslen(s.c_str()));
    return s;
}

void set_text(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

void set_status(const std::wstring& s) { set_text(g.status, s); }

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

void alert(const std::wstring& text, UINT flags = MB_OK | MB_ICONWARNING) {
    MessageBoxW(g.hwnd, text.c_str(), L"BotW Recomp Launcher", flags);
}

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
    std::wstring w = get_text(g.user);
    if (w.empty()) return {};
    return std::filesystem::path(w);
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
    auto ex = proc_address<ExFn>(GetModuleHandleW(L"shell32.dll"), "SHGetPathFromIDListEx");
    BOOL ok = ex ? ex(pidl, path, 32768, 0) : SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? std::wstring(path) : std::wstring();
}

std::wstring pick_exe(HWND owner) {
    wchar_t buf[32768];
    buf[0] = 0;
    std::wstring cur = get_text(g.exe);
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

void save_launcher_ini() {
    std::ofstream out(launcher_ini(), std::ios::binary | std::ios::trunc);
    if (!out) return;
    auto line = [&](const char* k, const std::wstring& v) { out << k << "=" << narrow(v) << "\n"; };
    line("game", get_text(g.game));
    line("exe", get_text(g.exe));
    line("user", get_text(g.user));
    line("seconds", get_text(g.seconds));
    line("fullscreen", SendMessageW(g.fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
}

void load_launcher_ini() {
    std::ifstream in(launcher_ini());
    std::wstring game, exe, user = appdata_root(), seconds = L"0";
    bool fullscreen = false;
    std::string line;
    auto take = [](const std::string& v) { return widen(v); };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "game") game = take(v);
        else if (k == "exe") exe = take(v);
        else if (k == "user" && !v.empty()) user = take(v);
        else if (k == "seconds") seconds = take(v);
        else if (k == "fullscreen") fullscreen = v == "1";
    }
    if (exe.empty()) {
        auto guess = exe_dir() / L"BotWRecomp.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(guess, ec)) exe = guess.wstring();
    }
    set_text(g.game, game);
    set_text(g.exe, exe);
    set_text(g.user, user);
    set_text(g.seconds, seconds.empty() ? L"0" : seconds);
    SendMessageW(g.fullscreen, BM_SETCHECK, fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
}

void fill_controls() {
    ListView_DeleteAllItems(g.controls);
    for (int i = 0; i < static_cast<int>(g.rows.size()); i++) {
        const auto* action = controlsfile::find_action(g.rows[static_cast<size_t>(i)].id);
        std::wstring label = widen(action ? action->label : g.rows[static_cast<size_t>(i)].id.c_str());
        std::wstring key = widen(controlsfile::keys_label(g.rows[static_cast<size_t>(i)]));
        std::wstring pad = widen(controlsfile::pad_label(g.rows[static_cast<size_t>(i)].pad));
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = label.data();
        it.lParam = i;
        ListView_InsertItem(g.controls, &it);
        ListView_SetItemText(g.controls, i, 1, key.data());
        ListView_SetItemText(g.controls, i, 2, pad.data());
    }
}

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
        alert(L"Set the user data folder before saving controls.");
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

void fill_saves() {
    ListView_DeleteAllItems(g.saves);
    g.sets = saveimport::list_sets(user_root());
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
        ListView_InsertItem(g.saves, &it);
        ListView_SetItemText(g.saves, i, 1, kind.data());
        ListView_SetItemText(g.saves, i, 2, when.data());
        ListView_SetItemText(g.saves, i, 3, notes.data());
    }
}

void refresh_lists() {
    load_controls();
    fill_saves();
}

void note_legacy_save() {
    if (saveimport::decimal_legacy_folder(user_root())) {
        set_status(L"A save is in save\\2147483649 from an older build. Migrate old save copies it to save\\80000001 and leaves the old folder in place.");
    }
}

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

bool capture_binding(int row, CaptureSide side) {
    if (!g.sdl) {
        alert(L"SDL did not start, so keys and controller buttons cannot be captured.");
        return false;
    }
    const auto* action = controlsfile::find_action(g.rows[static_cast<size_t>(row)].id);
    std::string label = action ? action->label : g.rows[static_cast<size_t>(row)].id;
    std::string title = std::string(side == CaptureSide::Key ? "Key for " : "Controller for ") + label + "  -  Esc cancels, Backspace clears";
    g.in_capture = true;
    EnableWindow(g.hwnd, FALSE);
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
    EnableWindow(g.hwnd, TRUE);
    SetForegroundWindow(g.hwnd);
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

void on_capture(int item, int sub) {
    if (item < 0 || item >= static_cast<int>(g.rows.size())) return;
    if (sub != 1 && sub != 2) return;
    if (!capture_binding(item, sub == 1 ? CaptureSide::Key : CaptureSide::Pad)) return;
    if (!save_controls()) return;
    fill_controls();
    set_status(L"Saved controls.ini. Start the game again to use it.");
}

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
        if (MessageBoxW(g.hwnd, text.c_str(), L"BotW Recomp Launcher", MB_YESNO | MB_ICONWARNING | def) != IDYES) {
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
    if (MessageBoxW(g.hwnd, text.c_str(), L"BotW Recomp Launcher", MB_YESNO | MB_ICONWARNING | def) != IDYES) return;
    auto result = saveimport::migrate_decimal(user_root());
    fill_saves();
    note_legacy_save();
    set_status(widen(result.message));
    alert(widen(result.message), result.ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

void on_import() {
    std::wstring picked = pick_browse(g.hwnd, L"Select a save folder, a .zip, or option.sav / game_data.sav inside the save.", true);
    import_path(picked);
}

int selected_set() {
    int i = ListView_GetNextItem(g.saves, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(g.sets.size())) return -1;
    return i;
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
    if (MessageBoxW(g.hwnd, text.c_str(), L"BotW Recomp Launcher", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
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
    if (MessageBoxW(g.hwnd, (L"Delete \"" + title + L"\"? This does not change the live save.").c_str(), L"BotW Recomp Launcher",
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
    ShellExecuteW(g.hwnd, L"open", (root / "save").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    return widen(data.substr(begin));
}

LRESULT CALLBACK log_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HWND edit = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_SIZE: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (edit) MoveWindow(edit, 0, 0, rc.right, rc.bottom, TRUE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_log() {
    auto path = user_root() / "runtime.log";
    std::wstring body = tail_file(path);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = log_wnd_proc;
        wc.hInstance = g.inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"BotWLauncherLog";
        RegisterClassW(&wc);
        registered = true;
    }
    HWND w = CreateWindowExW(0, L"BotWLauncherLog", L"runtime.log (last lines)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             S(760), S(520), g.hwnd, nullptr, g.inst, nullptr);
    if (!w) return;
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", body.c_str(),
                                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL, 0, 0, 10, 10, w,
                                nullptr, g.inst, nullptr);
    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(edit));
    if (g.font) SendMessageW(edit, WM_SETFONT, (WPARAM)g.font, TRUE);
    RECT rc{};
    GetClientRect(w, &rc);
    MoveWindow(edit, 0, 0, rc.right, rc.bottom, TRUE);
    EnableWindow(g.hwnd, FALSE);
    ShowWindow(w, SW_SHOW);
    MSG msg;
    while (IsWindow(w)) {
        BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
        if (gm == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (gm < 0) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) DestroyWindow(w);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(g.hwnd, TRUE);
    SetForegroundWindow(g.hwnd);
}

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

void on_play() {
    if (game_running()) {
        alert(L"BotWRecomp.exe is already running.");
        return;
    }
    std::wstring game = get_text(g.game);
    std::wstring exe = get_text(g.exe);
    std::wstring user = get_text(g.user);
    if (has_quote(game) || has_quote(exe) || has_quote(user)) {
        alert(L"Paths cannot contain a double quote.");
        return;
    }
    if (exe.empty() || !std::filesystem::is_regular_file(std::filesystem::path(exe))) {
        alert(L"Set the path to BotWRecomp.exe.");
        return;
    }
    std::filesystem::path game_path(game);
    std::error_code ec;
    bool looks = std::filesystem::is_regular_file(game_path / "code" / "U-King.rpx", ec);
    if (!looks) {
        if (MessageBoxW(g.hwnd, L"That game folder does not contain code\\U-King.rpx.\nLaunch anyway?", L"BotW Recomp Launcher",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    if (user.empty()) {
        alert(L"Set the user data folder. Saves and controls.ini go there.");
        return;
    }
    std::filesystem::create_directories(std::filesystem::path(user), ec);
    int seconds = 0;
    {
        std::wstring s = get_text(g.seconds);
        if (!s.empty()) seconds = _wtoi(s.c_str());
        if (seconds < 0) seconds = 0;
    }
    bool fullscreen = SendMessageW(g.fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring cmd = L"\"" + exe + L"\" --game \"" + game + L"\" --renderer vulkan";
    if (seconds > 0) cmd += L" --seconds " + std::to_wstring(seconds);
    if (fullscreen) cmd += L" --fullscreen";
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');
    std::wstring env = build_env(std::filesystem::path(user), std::filesystem::path(exe));
    std::wstring cwd = std::filesystem::path(exe).parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(),
                             cwd.c_str(), &si, &pi);
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
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    save_launcher_ini();
    set_status(L"Launched. Saves stay locked until BotWRecomp.exe closes.");
    g.was_running = true;
    EnableWindow(g.import_btn, FALSE);
    EnableWindow(g.switch_btn, FALSE);
    EnableWindow(g.delete_btn, FALSE);
    EnableWindow(g.migrate_btn, FALSE);
}

void rebuild_font() {
    if (g.font) DeleteObject(g.font);
    g.font = CreateFontW(-MulDiv(10, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    EnumChildWindows(g.hwnd, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        return TRUE;
    }, (LPARAM)g.font);
}

void layout() {
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    int m = S(12), gap = S(6), row = S(26), label_w = S(92), browse_w = S(84);
    int y = m;
    int width = rc.right - rc.left;
    auto place_row = [&](HWND edit, HWND button) {
        MoveWindow(edit, m + label_w, y, std::max(40, width - m * 2 - label_w - browse_w - gap), row, TRUE);
        MoveWindow(button, width - m - browse_w, y, browse_w, row, TRUE);
        y += row + gap;
    };
    // labels were created at fixed spots; move the edits and buttons, then the labels by child order.
    HWND labels[3] = {GetDlgItem(g.hwnd, 1), GetDlgItem(g.hwnd, 2), GetDlgItem(g.hwnd, 3)};
    HWND edits[3] = {g.game, g.exe, g.user};
    HWND buttons[3] = {GetDlgItem(g.hwnd, ID_GAME_BROWSE), GetDlgItem(g.hwnd, ID_EXE_BROWSE), GetDlgItem(g.hwnd, ID_USER_BROWSE)};
    for (int i = 0; i < 3; i++) {
        MoveWindow(labels[i], m, y + S(4), label_w, row, TRUE);
        place_row(edits[i], buttons[i]);
    }
    MoveWindow(GetDlgItem(g.hwnd, 4), m, y + S(4), S(70), row, TRUE);
    MoveWindow(g.seconds, m + S(74), y, S(64), row, TRUE);
    MoveWindow(g.fullscreen, m + S(150), y, S(120), row, TRUE);
    MoveWindow(GetDlgItem(g.hwnd, ID_PLAY), m + S(280), y, S(120), row, TRUE);
    MoveWindow(GetDlgItem(g.hwnd, ID_LOG), m + S(408), y, S(110), row, TRUE);
    y += row + gap;
    MoveWindow(GetDlgItem(g.hwnd, 5), m, y, S(400), S(18), TRUE);
    y += S(20);
    int list_h = S(250);
    MoveWindow(g.controls, m, y, width - m * 2, list_h, TRUE);
    ListView_SetColumnWidth(g.controls, 0, S(170));
    ListView_SetColumnWidth(g.controls, 1, S(220));
    ListView_SetColumnWidth(g.controls, 2, std::max(S(180), width - m * 2 - S(170) - S(220) - S(24)));
    y += list_h + gap;
    MoveWindow(GetDlgItem(g.hwnd, ID_RESET), m, y, S(200), row, TRUE);
    y += row + gap;
    MoveWindow(GetDlgItem(g.hwnd, 6), m, y, width - m * 2, S(18), TRUE);
    y += S(20);
    int buttons_h = row + gap + S(40);
    int saves_h = std::max(S(120), static_cast<int>(rc.bottom) - y - buttons_h - m);
    MoveWindow(g.saves, m, y, width - m * 2, saves_h, TRUE);
    int col = std::max(S(80), (width - m * 2) / 5);
    ListView_SetColumnWidth(g.saves, 0, col * 2);
    ListView_SetColumnWidth(g.saves, 1, col / 2 + S(10));
    ListView_SetColumnWidth(g.saves, 2, col);
    ListView_SetColumnWidth(g.saves, 3, std::max(S(80), width - m * 2 - col * 3 - S(10) - S(24)));
    y += saves_h + gap;
    int bw = S(118);
    int x = m;
    auto place = [&](HWND btn, int w) {
        MoveWindow(btn, x, y, w, row, TRUE);
        x += w + gap;
    };
    place(g.migrate_btn, S(150));
    place(g.import_btn, bw);
    place(g.switch_btn, bw);
    place(g.delete_btn, S(80));
    place(GetDlgItem(g.hwnd, ID_OPEN), S(140));
    place(GetDlgItem(g.hwnd, ID_REFRESH), S(80));
    y += row + gap;
    MoveWindow(g.status, m, y, width - m * 2, rc.bottom - y - m, TRUE);
}

HWND make_label(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
}

HWND make_edit(HWND parent, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)id, g.inst, nullptr);
}

HWND make_button(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g.inst,
                           nullptr);
}

HWND make_list(HWND parent, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0,
                           10, 10, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
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
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
}

void on_reset() {
    g.rows = controlsfile::defaults();
    if (!save_controls()) return;
    fill_controls();
    set_status(L"Controls reset to the built-in map and written to controls.ini. Start the game again to use them.");
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.dpi = dpi_of(hwnd);
            make_label(hwnd, 1, L"Game folder");
            make_label(hwnd, 2, L"Game exe");
            make_label(hwnd, 3, L"User data");
            g.game = make_edit(hwnd, ID_GAME);
            g.exe = make_edit(hwnd, ID_EXE);
            g.user = make_edit(hwnd, ID_USER);
            make_button(hwnd, ID_GAME_BROWSE, L"Browse");
            make_button(hwnd, ID_EXE_BROWSE, L"Browse");
            make_button(hwnd, ID_USER_BROWSE, L"Browse");
            make_label(hwnd, 4, L"Seconds");
            g.seconds = make_edit(hwnd, ID_SECONDS);
            g.fullscreen = CreateWindowExW(0, L"BUTTON", L"Fullscreen", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10, hwnd,
                                           (HMENU)(INT_PTR)ID_FULLSCREEN, g.inst, nullptr);
            make_button(hwnd, ID_PLAY, L"Play");
            make_button(hwnd, ID_LOG, L"View log");
            make_label(hwnd, 5, L"Controls  -  click Keyboard or Controller, then press a key or button");
            g.controls = make_list(hwnd, ID_CONTROLS);
            const wchar_t* control_cols[] = {L"Input", L"Keyboard", L"Controller"};
            add_columns(g.controls, control_cols, 3);
            make_button(hwnd, ID_RESET, L"Reset controls");
            make_label(hwnd, 6, L"Save sets  -  live files are in user data\\save\\80000001");
            g.saves = make_list(hwnd, ID_SAVES);
            const wchar_t* save_cols[] = {L"Name", L"Kind", L"When", L"Notes"};
            add_columns(g.saves, save_cols, 4);
            g.migrate_btn = make_button(hwnd, ID_MIGRATE, L"Migrate old save");
            g.import_btn = make_button(hwnd, ID_IMPORT, L"Import save");
            g.switch_btn = make_button(hwnd, ID_SWITCH, L"Switch to");
            g.delete_btn = make_button(hwnd, ID_DELETE, L"Delete");
            make_button(hwnd, ID_OPEN, L"Open saves folder");
            make_button(hwnd, ID_REFRESH, L"Refresh");
            g.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_STATUS, g.inst,
                                       nullptr);
            rebuild_font();
            load_launcher_ini();
            refresh_lists();
            if (saveimport::decimal_legacy_present(user_root())) note_legacy_save();
            else set_status(L"Drop a save folder or .zip here, or use Import save. Close the game before changing saves.");
            DragAcceptFiles(hwnd, TRUE);
            SetTimer(hwnd, 1, 400, nullptr);
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            rebuild_font();
            layout();
            return 0;
        }
        case WM_TIMER:
            pump_sdl();
            if (!g.in_capture) {
                bool run = game_running();
                EnableWindow(g.import_btn, run ? FALSE : TRUE);
                EnableWindow(g.switch_btn, run ? FALSE : TRUE);
                EnableWindow(g.delete_btn, run ? FALSE : TRUE);
                EnableWindow(g.migrate_btn, run ? FALSE : TRUE);
                if (run != g.was_running) {
                    g.was_running = run;
                    if (run) set_status(L"BotWRecomp.exe is running. Save import, switch, delete, and migrate are blocked until it closes.");
                    else set_status(L"Game closed. Saves can be changed again.");
                }
            }
            return 0;
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case ID_GAME_BROWSE: {
                    auto p = pick_browse(hwnd, L"Folder that contains code, content, and meta.", false);
                    if (!p.empty()) set_text(g.game, p);
                    save_launcher_ini();
                    break;
                }
                case ID_EXE_BROWSE: {
                    auto p = pick_exe(hwnd);
                    if (!p.empty()) set_text(g.exe, p);
                    save_launcher_ini();
                    break;
                }
                case ID_USER_BROWSE: {
                    auto p = pick_browse(hwnd, L"User data folder (BOTW_USER_ROOT). Saves, controls.ini, and the log go here.", false);
                    if (!p.empty()) {
                        set_text(g.user, p);
                        refresh_lists();
                        note_legacy_save();
                    }
                    save_launcher_ini();
                    break;
                }
                case ID_PLAY: on_play(); break;
                case ID_LOG: show_log(); break;
                case ID_RESET: on_reset(); break;
                case ID_IMPORT: on_import(); break;
                case ID_MIGRATE: on_migrate(); break;
                case ID_SWITCH: on_switch(); break;
                case ID_DELETE: on_delete(); break;
                case ID_OPEN: on_open_saves(); break;
                case ID_REFRESH:
                    refresh_lists();
                    note_legacy_save();
                    break;
                case ID_USER:
                    if (HIWORD(wp) == EN_KILLFOCUS) {
                        refresh_lists();
                        note_legacy_save();
                        save_launcher_ini();
                    }
                    break;
                default: break;
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == g.controls && hdr->code == NM_CLICK) {
                auto* act = reinterpret_cast<NMITEMACTIVATE*>(lp);
                LVHITTESTINFO ht{};
                ht.pt = act->ptAction;
                if (ListView_SubItemHitTest(g.controls, &ht) >= 0) on_capture(ht.iItem, ht.iSubItem);
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t path[32768];
            if (DragQueryFileW(drop, 0, path, 32768)) import_path(path);
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE:
            save_launcher_ini();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            for (auto* p : g.pads) SDL_CloseGamepad(p);
            g.pads.clear();
            if (g.sdl) SDL_Quit();
            if (g.font) DeleteObject(g.font);
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main(int, char**) {
    SDL_SetMainReady();
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
    g.dpi = dpi_of(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"BotWLauncher";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"BotW Recomp Launcher", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, MulDiv(1000, g.dpi, 96), MulDiv(860, g.dpi, 96), nullptr, nullptr, g.inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return 0;
}
