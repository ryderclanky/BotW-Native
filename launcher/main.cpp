// BotW-Native launcher: a guided first-run setup followed by a small main screen.
//
// First run walks through one step at a time: check the install, find and verify the game
// files (Base + Update v1.5.0, optional DLC), choose where saves go, check controllers, pick
// graphics defaults detected from this display, add optional mods, then Play. Every step says
// what it needs in plain words and will not move on until it is satisfied (or is optional).
// After that the launcher opens straight to Play / Mods / Settings / folders.
//
// Plain Win32 common controls, system font, one muted accent colour. Settings are shared with
// the classic launcher (BotWLauncher.ini beside the exe, settings.ini and mods.ini in the user
// data folder), so both can be used on the same install.
#include <SDL3/SDL.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include "game_build.h"
#include "game_info.h"
#include "gpu_list.h"
#include "mods.h"
#include "setup_wizard.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

const wchar_t* kTitle = L"BotW-Native";
const COLORREF kAccent = RGB(56, 96, 128);   // muted steel blue, the only accent
const COLORREF kOk = RGB(40, 110, 60);
const COLORREF kBad = RGB(160, 50, 40);

enum : UINT { WM_MERGE_PROGRESS = WM_APP + 1, WM_MERGE_DONE };
enum Step { S_WELCOME, S_GAME, S_BUILD, S_USER, S_PAD, S_GFX, S_MODS, S_READY, S_COUNT };
const wchar_t* kStepNames[S_COUNT] = {L"Welcome", L"Game files", L"Build the game", L"Saves folder", L"Controller",
                                      L"Graphics", L"Mods (optional)", L"Ready"};

enum Id {
    ID_BACK = 100, ID_NEXT, ID_GAME_BROWSE, ID_GAME_MERGE, ID_USER_BROWSE, ID_USER_DEFAULT,
    ID_LAYOUT, ID_GYRO, ID_SCALE, ID_FPS, ID_FULLSCREEN, ID_MOD_ADD, ID_MOD_ADDDIR,
    ID_PLAY, ID_MODS, ID_SETTINGS, ID_OPEN_GAME, ID_OPEN_USER, ID_OPEN_LOG, ID_CLASSIC,
    ID_MODLIST, ID_MODSAVE, ID_GAME_EDIT, ID_USER_EDIT,
    ID_SRC_EDIT, ID_SRC_BROWSE, ID_TC_INSTALL, ID_TC_LINK, ID_TC_RECHECK, ID_PREBUILT, ID_BUILD, ID_BUILD_STOP, ID_GPU,
};

struct Config {
    std::wstring game, exe, user, seconds = L"0", theme, source, build_state;
    bool fullscreen = false, gamepad_view = false, no_mods = false, setup_done = false;
    std::vector<std::string> other;
};

struct App {
    HINSTANCE inst{};
    HWND wnd{};
    HFONT font{}, bold{}, title{};
    HBRUSH white{}, side{};
    Config cfg;
    setupwizard::GamePrefs prefs;
    bool wizard = true;
    int step = 0;
    // wizard widgets
    HWND steps[S_COUNT]{};
    HWND head{}, help{}, status{}, back{}, next{};
    std::vector<HWND> page[S_COUNT];
    HWND game_edit{}, game_browse{}, game_rows{}, game_merge{};
    HWND user_edit{}, user_note{};
    HWND pad_list{}, layout{}, gyro{};
    HWND gfx_note{}, scale{}, fps{}, fullscreen{}, gpu{};
    std::vector<gpupick::Gpu> gpus;
    bool gpus_loaded = false;
    int gpu_missing = -1;  // combo item for a saved GPU that is not in the list
    HWND mod_note{};
    HWND src_edit{}, tc_rows{}, prebuilt{}, build_btn{}, stop_btn{}, bstage{}, bbar{}, belapsed{}, blog{}, bjobs{};
    gamebuild::Toolchain tc;
    gamebuild::Runner runner;
    bool build_ok = false, dry_run = false, was_running = false;
    ULONGLONG build_t0 = 0;
    HFONT mono{};
    HWND ready_text{};
    // main screen widgets
    std::vector<HWND> main_ctl;
    HWND main_title{}, main_info{}, main_status{}, play{};
    // state
    gameinfo::DumpLayout dumps;
    bool game_ok = false, game_warn = false;
    std::wstring status_text;
    COLORREF status_color = 0;
    std::atomic<bool> merging{false};
    std::wstring merge_label;
    int merge_done = 0, merge_total = 0;
    std::string merge_err;
    fs::path merge_target;
    bool sdl_ok = false;
    int disp_w = 0, disp_h = 0, disp_hz = 0;
} A;

// ---------- small helpers ----------
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring text_of(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
bool is_file(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
bool is_dir(const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec); }

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}
fs::path ini_path() { return exe_dir() / L"BotWLauncher.ini"; }
std::wstring default_user() {
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf) != S_OK) return L"BotWRecomp";
    return std::wstring(buf) + L"\\BotWRecomp";
}

void load_ini() {
    Config c;
    c.user = default_user();
    std::ifstream in(ini_path());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) { if (!line.empty()) c.other.push_back(line); continue; }
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "game") c.game = widen(v);
        else if (k == "exe") c.exe = widen(v);
        else if (k == "user") { if (!v.empty()) c.user = widen(v); }
        else if (k == "seconds") c.seconds = widen(v);
        else if (k == "fullscreen") c.fullscreen = v == "1";
        else if (k == "gamepad_view") c.gamepad_view = v == "1";
        else if (k == "no_mods") c.no_mods = v == "1";
        else if (k == "setup_done") c.setup_done = v == "1";
        else if (k == "theme") c.theme = widen(v);
        else if (k == "source") c.source = widen(v);
        else if (k == "build_state") c.build_state = widen(v);
        else c.other.push_back(line);
    }
    if (c.exe.empty() || !is_file(c.exe)) {
        fs::path guess = exe_dir() / L"BotWRecomp.exe";
        if (is_file(guess)) c.exe = guess.wstring();
    }
    if (c.seconds.empty()) c.seconds = L"0";
    A.cfg = c;
}

void write_ini(std::ostream& out);
// Writes to a temporary file first and swaps it in, so a crash or power loss never leaves a
// half-written ini. Tells the user once if the folder cannot be written (for example a
// read-only or protected install folder).
void save_ini() {
    static bool warned = false;
    fs::path tmp = ini_path();
    tmp += L".tmp";
    bool ok = false;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) { write_ini(out); out.flush(); ok = (bool)out; }
    }
    if (ok) ok = MoveFileExW(tmp.c_str(), ini_path().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        std::error_code ec;
        fs::remove(tmp, ec);
        if (!warned) {
            warned = true;
            MessageBoxW(nullptr, (L"Settings could not be saved to\n" + ini_path().wstring() +
                L"\n\nThe folder is read-only or protected. Move the game folder somewhere you can write to "
                L"(for example your Documents folder) so your choices are kept.").c_str(),
                kTitle, MB_OK | MB_ICONWARNING);
        }
    }
}

void write_ini(std::ostream& out) {
    auto line = [&](const char* k, const std::wstring& v) { out << k << "=" << narrow(v) << "\n"; };
    line("game", A.cfg.game);
    line("exe", A.cfg.exe);
    line("user", A.cfg.user);
    line("seconds", A.cfg.seconds);
    line("fullscreen", A.cfg.fullscreen ? L"1" : L"0");
    line("gamepad_view", A.cfg.gamepad_view ? L"1" : L"0");
    line("no_mods", A.cfg.no_mods ? L"1" : L"0");
    line("setup_done", A.cfg.setup_done ? L"1" : L"0");
    line("theme", A.cfg.theme);
    line("source", A.cfg.source);
    line("build_state", A.cfg.build_state);
    for (auto& l : A.cfg.other) out << l << "\n";
}

std::wstring pick_folder(HWND owner, const wchar_t* title, const std::wstring& start) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (!start.empty() && is_dir(start)) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) { dlg->SetFolder(si); si->Release(); }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    return result;
}

std::wstring pick_zip(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    COMDLG_FILTERSPEC f[] = {{L"Mod archives (*.zip, *.bnp)", L"*.zip;*.bnp"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, f);
    dlg->SetTitle(L"Choose a mod archive");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    return result;
}

bool process_running(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

void open_folder(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    ShellExecuteW(A.wnd, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool vulkan_present() {
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    if (!m) return false;
    FreeLibrary(m);
    return true;
}

void detect_display() {
    HMONITOR mon = MonitorFromWindow(A.wnd ? A.wnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        A.disp_w = (int)dm.dmPelsWidth;
        A.disp_h = (int)dm.dmPelsHeight;
        A.disp_hz = (int)dm.dmDisplayFrequency;
    }
}
int recommended_scale() {
    if (A.disp_h >= 2160) return 3;
    if (A.disp_h >= 1440) return 2;
    return 1;
}
int recommended_fps() { return (A.disp_hz == 0 || A.disp_hz >= 59) ? 60 : 30; }

std::vector<std::wstring> gamepads() {
    std::vector<std::wstring> out;
    if (!A.sdl_ok) return out;
    SDL_PumpEvents();
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        const char* nm = SDL_GetGamepadNameForID(ids[i]);
        out.push_back(widen(nm ? nm : "Controller"));
    }
    SDL_free(ids);
    return out;
}

// ---------- widgets ----------
HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, w, h, A.wnd, (HMENU)(INT_PTR)id, A.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : A.font), TRUE);
    return c;
}
HWND on_page(int s, HWND c) { A.page[s].push_back(c); return c; }
HWND label(int s, const wchar_t* t, int x, int y, int w, int h) { return on_page(s, mk(L"STATIC", t, SS_LEFT, x, y, w, h)); }

void set_status(const std::wstring& t, COLORREF c = 0) {
    A.status_text = t;
    A.status_color = c;
    SetWindowTextW(A.status, t.c_str());
    SetWindowTextW(A.main_status, t.c_str());
    InvalidateRect(A.status, nullptr, TRUE);
    InvalidateRect(A.main_status, nullptr, TRUE);
}

const wchar_t* kHelp[S_COUNT] = {
    L"This will take a couple of minutes. Each step explains what it needs, and you can go back at any time. "
    L"Nothing is downloaded and your game files are never changed.",
    L"Choose the folder that holds your own dump of the game. BotW-Native needs the USA base game and the "
    L"v1.5.0 update (title version 208). The DLC is optional. You can pick one folder that already has "
    L"both merged (it contains code, content and meta), or a parent folder that holds the separate Base, "
    L"Update and DLC dumps - they will be found automatically.",
    L"First setup only. The game code is recompiled from your own U-King.rpx and compiled on this PC, once - "
    L"usually 30 to 60 minutes with every CPU core busy. You can stop at any time: finished parts are kept and "
    L"Build continues where it stopped. Later launches skip this.",
    L"Saves, controls.ini, settings, mods and the log are kept in this folder. The default is fine for most "
    L"people. If you already have saves from an earlier setup, point this at that folder to keep using them.",
    L"Plug in or pair a controller now if you want to use one; it shows up below within a second. "
    L"Keyboard and mouse always work, so this step is never required.",
    L"These defaults were picked from your current display. 60 FPS needs a reasonably modern CPU; if the game "
    L"stutters, choose 30. You can change all of this later in Settings.",
    L"Mods are optional. Add a graphic pack or content mod as a .zip/.bnp archive or a folder. They are copied "
    L"into your saves folder and turned on. You can turn them off later from Mods.",
    L"Everything is set. Press Finish to save these choices and go to the main screen.",
};

void build_wizard() {
    int y = 20;
    for (int i = 0; i < S_COUNT; i++, y += 28)
        A.steps[i] = mk(L"STATIC", kStepNames[i], SS_LEFT | SS_NOTIFY, 20, y, 150, 22);
    A.head = mk(L"STATIC", L"", SS_LEFT, 200, 16, 500, 30, 0, A.title);
    A.help = mk(L"STATIC", L"", SS_LEFT, 200, 52, 500, 96);
    A.status = mk(L"STATIC", L"", SS_LEFT, 200, 462, 500, 40);
    A.back = mk(L"BUTTON", L"< Back", BS_PUSHBUTTON | WS_TABSTOP, 500, 512, 96, 28, ID_BACK);
    A.next = mk(L"BUTTON", L"Next >", BS_DEFPUSHBUTTON | WS_TABSTOP, 604, 512, 96, 28, ID_NEXT);

    const int X = 200, W = 500;
    // welcome
    label(S_WELCOME, L"", X, 156, W, 200);
    // game
    label(S_GAME, L"Game folder:", X, 160, 120, 20);
    A.game_edit = on_page(S_GAME, mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, X, 182, W - 104, 24, ID_GAME_EDIT));
    A.game_browse = on_page(S_GAME, mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, X + W - 96, 181, 96, 26, ID_GAME_BROWSE));
    A.game_rows = label(S_GAME, L"", X, 216, W, 120);
    A.game_merge = on_page(S_GAME, mk(L"BUTTON", L"Combine into one game folder...", BS_PUSHBUTTON | WS_TABSTOP, X, 340, 240, 28, ID_GAME_MERGE));
    // build
    label(S_BUILD, L"Source folder:", X, 156, 96, 20);
    A.src_edit = on_page(S_BUILD, mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, X + 100, 152, W - 204, 24, ID_SRC_EDIT));
    on_page(S_BUILD, mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, X + W - 96, 151, 96, 26, ID_SRC_BROWSE));
    A.tc_rows = label(S_BUILD, L"", X, 184, W, 52);
    on_page(S_BUILD, mk(L"BUTTON", L"Install build tools...", BS_PUSHBUTTON | WS_TABSTOP, X, 238, 150, 26, ID_TC_INSTALL));
    on_page(S_BUILD, mk(L"BUTTON", L"MSYS2 website", BS_PUSHBUTTON | WS_TABSTOP, X + 158, 238, 110, 26, ID_TC_LINK));
    on_page(S_BUILD, mk(L"BUTTON", L"Check again", BS_PUSHBUTTON | WS_TABSTOP, X + 276, 238, 100, 26, ID_TC_RECHECK));
    A.prebuilt = on_page(S_BUILD, mk(L"BUTTON", L"Use the game program that is already here (skip building)", BS_AUTOCHECKBOX | WS_TABSTOP, X, 272, W, 22, ID_PREBUILT));
    A.bjobs = label(S_BUILD, L"", X, 298, W, 20);
    A.build_btn = on_page(S_BUILD, mk(L"BUTTON", L"Build game", BS_PUSHBUTTON | WS_TABSTOP, X, 322, 120, 28, ID_BUILD));
    A.stop_btn = on_page(S_BUILD, mk(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, X + 128, 322, 80, 28, ID_BUILD_STOP));
    A.bstage = label(S_BUILD, L"", X + 220, 320, W - 220, 34);
    A.bbar = on_page(S_BUILD, mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, X, 358, W - 90, 14));
    SendMessageW(A.bbar, PBM_SETRANGE32, 0, 1000);
    SendMessageW(A.bbar, PBM_SETBARCOLOR, 0, kAccent);
    A.belapsed = label(S_BUILD, L"", X + W - 82, 356, 82, 20);
    A.blog = on_page(S_BUILD, mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, X, 378, W, 78, 0, A.mono));
    SendMessageW(A.blog, EM_SETLIMITTEXT, 0, 0);
    // user
    label(S_USER, L"Saves folder:", X, 160, 120, 20);
    A.user_edit = on_page(S_USER, mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, X, 182, W - 104, 24, ID_USER_EDIT));
    on_page(S_USER, mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, X + W - 96, 181, 96, 26, ID_USER_BROWSE));
    on_page(S_USER, mk(L"BUTTON", L"Use default", BS_PUSHBUTTON | WS_TABSTOP, X, 214, 110, 26, ID_USER_DEFAULT));
    A.user_note = label(S_USER, L"", X, 250, W, 100);
    // pad
    label(S_PAD, L"Connected controllers:", X, 160, 200, 20);
    A.pad_list = on_page(S_PAD, mk(L"LISTBOX", L"", WS_BORDER | LBS_NOSEL, X, 182, W, 80));
    label(S_PAD, L"Button layout:", X, 276, 110, 20);
    A.layout = on_page(S_PAD, mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, X + 120, 272, 220, 200, ID_LAYOUT));
    for (auto s : {L"Automatic (match controller)", L"Xbox (A is bottom)", L"Nintendo (A is right)"})
        SendMessageW(A.layout, CB_ADDSTRING, 0, (LPARAM)s);
    A.gyro = on_page(S_PAD, mk(L"BUTTON", L"Use motion controls (gyro) when the controller has them", BS_AUTOCHECKBOX | WS_TABSTOP, X, 308, W, 22, ID_GYRO));
    // gfx
    A.gfx_note = label(S_GFX, L"", X, 156, W, 40);
    label(S_GFX, L"Resolution:", X, 204, 110, 20);
    A.scale = on_page(S_GFX, mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, X + 120, 200, 220, 200, ID_SCALE));
    for (auto s : {L"720p (1x, original)", L"1440p (2x)", L"2160p (3x)"}) SendMessageW(A.scale, CB_ADDSTRING, 0, (LPARAM)s);
    label(S_GFX, L"Frame rate:", X, 240, 110, 20);
    A.fps = on_page(S_GFX, mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, X + 120, 236, 220, 200, ID_FPS));
    for (auto s : {L"30 FPS (original)", L"60 FPS"}) SendMessageW(A.fps, CB_ADDSTRING, 0, (LPARAM)s);
    label(S_GFX, L"Graphics card:", X, 276, 110, 20);
    A.gpu = on_page(S_GFX, mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, X + 120, 272, W - 120, 200, ID_GPU));
    A.fullscreen = on_page(S_GFX, mk(L"BUTTON", L"Start in fullscreen", BS_AUTOCHECKBOX | WS_TABSTOP, X, 312, W, 22, ID_FULLSCREEN));
    // mods
    on_page(S_MODS, mk(L"BUTTON", L"Add mod archive...", BS_PUSHBUTTON | WS_TABSTOP, X, 160, 160, 28, ID_MOD_ADD));
    on_page(S_MODS, mk(L"BUTTON", L"Add mod folder...", BS_PUSHBUTTON | WS_TABSTOP, X + 170, 160, 160, 28, ID_MOD_ADDDIR));
    A.mod_note = label(S_MODS, L"", X, 200, W, 160);
    // ready
    A.ready_text = label(S_READY, L"", X, 156, W, 220);
}

// Graphics card list: "Automatic" first, then every Vulkan GPU, then a saved GPU that is missing.
void fill_gpu_combo() {
    if (!A.gpus_loaded) { A.gpus = gpulist::enumerate(); A.gpus_loaded = true; }
    SendMessageW(A.gpu, CB_RESETCONTENT, 0, 0);
    int b = gpupick::best(A.gpus);
    std::wstring a = A.gpus.empty() ? L"Automatic (no Vulkan GPU found)"
                   : L"Automatic (best GPU: " + widen(A.gpus[b < 0 ? 0 : b].name) + L")";
    SendMessageW(A.gpu, CB_ADDSTRING, 0, (LPARAM)a.c_str());
    int by_uuid = 0, by_name = 0;
    for (size_t i = 0; i < A.gpus.size(); i++) {
        const auto& g = A.gpus[i];
        std::wstring s = widen(g.name) + L" (" + widen(gpupick::type_name(g.type)) + L", " +
                         std::to_wstring((g.vram + (512ull << 20)) >> 30) + L" GB)";
        SendMessageW(A.gpu, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        if (!by_uuid && !A.prefs.gpu.empty() && gpupick::uuid_hex(g.uuid) == gpupick::lower(A.prefs.gpu)) by_uuid = (int)i + 1;
        if (!by_name && !A.prefs.gpu_name.empty() && g.name == A.prefs.gpu_name) by_name = (int)i + 1;
    }
    int sel = by_uuid ? by_uuid : by_name;
    A.gpu_missing = -1;
    if (!sel && (!A.prefs.gpu.empty() || !A.prefs.gpu_name.empty())) {
        std::wstring s = widen(A.prefs.gpu_name.empty() ? A.prefs.gpu : A.prefs.gpu_name) + L" (saved, not found)";
        A.gpu_missing = (int)SendMessageW(A.gpu, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        sel = A.gpu_missing;
    }
    SendMessageW(A.gpu, CB_SETCURSEL, sel, 0);
}

void build_main() {
    A.main_title = mk(L"STATIC", L"", SS_LEFT, 24, 20, 670, 32, 0, A.title);
    A.main_info = mk(L"STATIC", L"", SS_LEFT, 24, 58, 670, 60);
    A.play = mk(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 24, 130, 200, 48, ID_PLAY, A.bold);
    HWND b[] = {
        mk(L"BUTTON", L"Mods...", BS_PUSHBUTTON | WS_TABSTOP, 24, 196, 160, 30, ID_MODS),
        mk(L"BUTTON", L"Settings (run setup again)", BS_PUSHBUTTON | WS_TABSTOP, 192, 196, 200, 30, ID_SETTINGS),
        mk(L"BUTTON", L"Open game folder", BS_PUSHBUTTON | WS_TABSTOP, 24, 236, 160, 30, ID_OPEN_GAME),
        mk(L"BUTTON", L"Open saves folder", BS_PUSHBUTTON | WS_TABSTOP, 192, 236, 160, 30, ID_OPEN_USER),
        mk(L"BUTTON", L"Open log", BS_PUSHBUTTON | WS_TABSTOP, 360, 236, 120, 30, ID_OPEN_LOG),
        mk(L"BUTTON", L"Classic launcher", BS_PUSHBUTTON | WS_TABSTOP, 488, 236, 140, 30, ID_CLASSIC),
    };
    A.main_status = mk(L"STATIC", L"", SS_LEFT, 24, 290, 670, 60);
    A.main_ctl = {A.main_title, A.main_info, A.play, A.main_status};
    for (HWND h : b) A.main_ctl.push_back(h);
}

// ---------- validation ----------
std::wstring mark(bool ok) { return ok ? L"[OK]  " : L"[--]  "; }

void check_game() {
    std::wstring path = text_of(A.game_edit);
    A.game_ok = A.game_warn = false;
    ShowWindow(A.game_merge, SW_HIDE);
    if (path.empty()) {
        SetWindowTextW(A.game_rows, L"No folder chosen yet.");
        set_status(L"Choose a folder to continue.");
        return;
    }
    if (!is_dir(path)) {
        SetWindowTextW(A.game_rows, L"That folder does not exist.");
        set_status(L"Check the path, or use Browse.", kBad);
        return;
    }
    fs::path p(path);
    std::wstring rows;
    if (is_file(p / "code" / "U-King.rpx")) {
        gameinfo::Info info = gameinfo::read(p);
        bool v208 = info.has_title_version && info.title_version == gameinfo::kExpectedTitleVersion;
        std::wstring ver = info.has_title_version ? widen(gameinfo::version_name(info.title_version)) : L"unknown version";
        std::wstring reg = widen(gameinfo::region_name(info.title_id));
        rows += mark(true) + L"Game code found (code\\U-King.rpx)\r\n";
        rows += mark(info.has_content) + L"Game data (content)\r\n";
        rows += mark(v208) + L"Version: " + ver + (reg.empty() ? L"" : L", " + reg) + L" - needs v1.5.0\r\n";
        rows += mark(info.has_dlc) + (info.has_dlc ? L"DLC found" : L"DLC not found (optional)") + L"\r\n";
        SetWindowTextW(A.game_rows, rows.c_str());
        if (!info.has_content) { set_status(L"The content folder is missing. Pick the top folder of the dump.", kBad); return; }
        if (!v208) {
            set_status(L"This is not the v1.5.0 update. Install the v1.5.0 update into this dump, or pick the "
                       L"parent folder holding the separate Base and Update dumps.", kBad);
            return;
        }
        A.game_ok = true;
        A.cfg.game = path;
        set_status(L"Game files look good. Press Next.", kOk);
        return;
    }
    A.dumps = gameinfo::discover_dumps(p);
    if (!A.dumps.merged_game.empty()) {
        SetWindowTextW(A.game_edit, A.dumps.merged_game.wstring().c_str());
        check_game();
        return;
    }
    bool base = !A.dumps.base_folder.empty(), upd = !A.dumps.update_folder.empty(), dlc = !A.dumps.dlc_folder.empty();
    rows += mark(base) + L"Base game" + (base ? L": " + A.dumps.base_folder.filename().wstring() : L" not found") + L"\r\n";
    rows += mark(upd) + L"Update v1.5.0" + (upd ? L": " + A.dumps.update_folder.filename().wstring() : L" not found") + L"\r\n";
    rows += mark(dlc) + (dlc ? L"DLC: " + A.dumps.dlc_folder.filename().wstring() : std::wstring(L"DLC not found (optional)")) + L"\r\n";
    SetWindowTextW(A.game_rows, rows.c_str());
    if (base && upd) {
        ShowWindow(A.game_merge, SW_SHOW);
        set_status(L"Found separate dumps. Press \"Combine into one game folder\" - it uses hard links on the same "
                   L"drive, so it is quick and takes almost no extra space.", kAccent);
    } else {
        set_status(L"Could not find the game here. The folder should contain code, content and meta, or the "
                   L"separate Base and Update dumps.", kBad);
    }
}

void start_merge() {
    fs::path base = A.dumps.base_folder;
    fs::path target = base.parent_path() / L"BotW-Native game";
    std::wstring chosen = pick_folder(A.wnd, L"Where should the combined game folder go? (a new subfolder is created)", base.parent_path().wstring());
    if (chosen.empty()) return;
    target = fs::path(chosen) / L"BotW-Native game";
    A.merge_target = target;
    A.merging = true;
    EnableWindow(A.next, FALSE);
    EnableWindow(A.back, FALSE);
    EnableWindow(A.game_merge, FALSE);
    set_status(L"Combining... this can take a minute.", kAccent);
    gameinfo::DumpLayout d = A.dumps;
    HWND w = A.wnd;
    std::thread([d, target, w] {
        std::string err;
        bool ok = gameinfo::merge_dumps(d.base_folder, d.update_folder, d.dlc_folder, target, err,
                                        [w](int done, int total, const std::string&) {
                                            PostMessageW(w, WM_MERGE_PROGRESS, done, total);
                                        });
        A.merge_err = err;
        PostMessageW(w, WM_MERGE_DONE, ok, 0);
    }).detach();
}

bool check_user() {
    std::wstring path = text_of(A.user_edit);
    if (path.empty()) { SetWindowTextW(A.user_note, L""); set_status(L"Choose a folder, or press Use default.", kBad); return false; }
    fs::path p(path);
    std::error_code ec;
    fs::create_directories(p, ec);
    fs::path probe = p / L".write_test";
    bool writable = false;
    { std::ofstream t(probe); writable = (bool)t; }
    fs::remove(probe, ec);
    if (!writable) {
        SetWindowTextW(A.user_note, L"");
        set_status(L"Can't write to that folder. Pick one you own, not under Program Files.", kBad);
        return false;
    }
    bool saves = is_dir(p / L"save") || is_dir(p / L"saves");
    bool controls = is_file(p / L"controls.ini");
    std::wstring note;
    note += saves ? L"Existing saves found here - they will be used.\r\n" : L"No saves here yet - a new game starts fresh.\r\n";
    if (controls) note += L"Existing controls.ini found - your bindings are kept.\r\n";
    SetWindowTextW(A.user_note, note.c_str());
    A.cfg.user = path;
    set_status(L"Folder is writable. Press Next.", kOk);
    return true;
}

void refresh_pads() {
    auto pads = gamepads();
    SendMessageW(A.pad_list, LB_RESETCONTENT, 0, 0);
    if (pads.empty()) SendMessageW(A.pad_list, LB_ADDSTRING, 0, (LPARAM)L"None yet - keyboard and mouse will be used.");
    for (auto& n : pads) SendMessageW(A.pad_list, LB_ADDSTRING, 0, (LPARAM)n.c_str());
    if (A.step == S_PAD && A.wizard)
        set_status(pads.empty() ? L"No controller detected. That's fine - press Next to use the keyboard."
                                : L"Controller detected. Press Next.", pads.empty() ? 0 : kOk);
}

void refresh_mods_note() {
    fs::path root(A.cfg.user);
    mods::State st = mods::load(root);
    std::wstring s = L"Installed mods: " + std::to_wstring(st.list.size()) + L" (" + std::to_wstring(mods::enabled_count(st)) + L" on)\r\n";
    for (auto& m : st.list) s += (m.enabled ? L"  on   " : L"  off  ") + widen(m.name) + L"\r\n";
    SetWindowTextW(A.mod_note, s.c_str());
}

void add_mod(const std::wstring& src) {
    if (src.empty()) return;
    std::string folder, err;
    if (mods::install_mod(fs::path(A.cfg.user), fs::path(src), folder, err))
        set_status(L"Added and turned on: " + widen(folder), kOk);
    else
        set_status(L"Could not add that mod: " + widen(err), kBad);
    refresh_mods_note();
}

void fill_ready() {
    const wchar_t* sc[] = {L"720p", L"1440p", L"2160p"};
    mods::State st = mods::load(fs::path(A.cfg.user));
    std::wstring s;
    s += L"Game folder:\r\n    " + A.cfg.game + L"\r\n";
    s += L"Saves folder:\r\n    " + A.cfg.user + L"\r\n";
    s += L"Graphics: " + std::wstring(sc[std::clamp(A.prefs.scale, 1, 3) - 1]) + L", " + std::to_wstring(A.prefs.fps) + L" FPS" + (A.cfg.fullscreen ? L", fullscreen" : L", windowed") + L"\r\n";
    s += L"Mods: " + std::to_wstring(mods::enabled_count(st)) + L" on\r\n";
    s += A.cfg.exe.empty() ? L"\r\nWarning: BotWRecomp.exe was not found next to the launcher." : L"";
    SetWindowTextW(A.ready_text, s.c_str());
    set_status(L"Press Finish.", kOk);
}

std::wstring welcome_text() {
    bool exe = !A.cfg.exe.empty() && is_file(A.cfg.exe);
    bool vk = vulkan_present();
    std::wstring s = L"You will need:\r\n"
                     L"    - your own dump of Breath of the Wild (USA) with the v1.5.0 update\r\n"
                     L"    - optionally the DLC and any mods you want\r\n\r\nChecking this install:\r\n";
    s += L"    " + mark(exe) + (exe ? L"Game program found (BotWRecomp.exe)" : L"Game program not built yet - setup builds it for you") + L"\r\n";
    s += L"    " + mark(vk) + (vk ? L"Vulkan graphics driver found" : L"Vulkan not found - update your graphics driver") + L"\r\n";
    return s;
}

// ---------- build step ----------
fs::path install_exe() { return exe_dir() / L"BotWRecomp.exe"; }
bool prebuilt_present() { return is_file(install_exe()) && is_file(exe_dir() / L"SDL3.dll"); }

void refresh_build_page() {
    A.tc = gamebuild::detect_toolchain();
    std::wstring src = text_of(A.src_edit);
    bool src_ok = gamebuild::is_source_dir(src);
    std::wstring rows;
    rows += mark(src_ok) + (src_ok ? L"Source found (tools\\recompile.py, CMakeLists.txt)" : L"Choose the BotW-Native source folder") + L"\r\n";
    rows += mark(!A.tc.ucrt_bin.empty()) + (A.tc.ucrt_bin.empty() ? std::wstring(L"MSYS2 not found") : L"MSYS2 UCRT64: " + A.tc.ucrt_bin.wstring()) + L"\r\n";
    rows += mark(A.tc.ready()) + L"Tools:  g++ " + (A.tc.gxx ? L"yes" : L"missing") + L",  cmake " + (A.tc.cmake ? L"yes" : L"missing") +
            L",  ninja " + (A.tc.ninja ? L"yes" : L"missing") + L",  python " + (A.tc.python ? L"yes" : L"missing");
    SetWindowTextW(A.tc_rows, rows.c_str());
    int cores = 0;
    double gb = 0;
    int jobs = gamebuild::auto_jobs(&cores, &gb);
    wchar_t jl[160];
    swprintf(jl, 160, L"Will compile %d files at a time (this PC: %d threads, %.0f GB memory).%ls", jobs, cores, gb, A.dry_run ? L"  [dry run]" : L"");
    SetWindowTextW(A.bjobs, jl);
    bool pre = prebuilt_present();
    ShowWindow(A.prebuilt, (A.step == S_BUILD && pre && !A.cfg.setup_done) ? SW_SHOW : SW_HIDE);
    bool busy = A.runner.running();
    EnableWindow(A.build_btn, !busy && src_ok && A.tc.ready());
    EnableWindow(A.stop_btn, busy);
    SetWindowTextW(A.build_btn, A.cfg.setup_done ? L"Rebuild game" : (A.cfg.build_state == L"stopped" || A.cfg.build_state == L"failed" ? L"Continue build" : L"Build game"));
    if (busy) return;
    if (A.build_ok) set_status(L"The game is built and installed. Press Next.", kOk);
    else if (A.cfg.setup_done) set_status(pre ? L"The game is already built - press Next. Use Rebuild game only after updating the source." : L"BotWRecomp.exe is missing - press Rebuild game.", pre ? kOk : kBad);
    else if (pre && SendMessageW(A.prebuilt, BM_GETCHECK, 0, 0) == BST_CHECKED) set_status(L"The existing game program will be used. Press Next.", kOk);
    else if (!src_ok) set_status(L"Choose the source folder (it holds tools and CMakeLists.txt).", kBad);
    else if (!A.tc.ready()) set_status(L"Build tools are missing. Press Install build tools, wait for the window to finish, then Check again.", kBad);
    else if (A.cfg.build_state == L"stopped" || A.cfg.build_state == L"failed") set_status(L"An earlier build did not finish. Press Continue build - finished parts are kept.", kAccent);
    else set_status(L"Ready to build. Press Build game and leave the PC to it.", kAccent);
}

bool build_step_ok() {
    if (A.build_ok) return true;
    if (A.runner.running()) return false;
    if (A.cfg.setup_done) return prebuilt_present();
    return prebuilt_present() && SendMessageW(A.prebuilt, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void install_tools() {
    // Guided install: MSYS2 via winget if needed, then the UCRT64 packages from README.md.
    std::wstring root = A.tc.msys_root.empty() ? L"C:\\msys64" : A.tc.msys_root.wstring();
    std::wstring cmd = L"/k echo Installing the BotW-Native build tools (MSYS2 UCRT64). This can take a while. && ";
    if (A.tc.msys_root.empty()) cmd += L"winget install -e --id MSYS2.MSYS2 --accept-source-agreements --accept-package-agreements && ";
    cmd += L"\"" + root + L"\\usr\\bin\\bash.exe\" -lc \"pacman -Sy --needed --noconfirm " + std::wstring(gamebuild::pacman_packages()) +
           L"\" && echo. && echo Done. Close this window and press Check again in the launcher.";
    ShellExecuteW(A.wnd, L"open", L"cmd.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
    set_status(L"A window is installing the tools. When it says Done, press Check again.", kAccent);
}

void start_build() {
    gamebuild::Options o;
    o.source = text_of(A.src_edit);
    o.rpx = fs::path(A.cfg.game) / L"code" / L"U-King.rpx";
    o.install_dir = exe_dir();
    o.tc = A.tc;
    o.jobs = gamebuild::auto_jobs();
    o.dry_run = A.dry_run;
    A.cfg.source = o.source.wstring();
    if (!A.dry_run) { A.cfg.build_state = L"stopped"; save_ini(); }
    A.build_ok = false;
    SetWindowTextW(A.blog, L"");
    A.build_t0 = GetTickCount64();
    A.runner.start(o);
    A.was_running = true;
    EnableWindow(A.back, FALSE);
    EnableWindow(A.next, FALSE);
    SetTimer(A.wnd, 2, 250, nullptr);
    refresh_build_page();
}

void tick_build() {
    std::string add = A.runner.take_log();
    if (!add.empty()) {
        int len = GetWindowTextLengthW(A.blog);
        if (len > 400000) { SendMessageW(A.blog, EM_SETSEL, 0, len - 200000); SendMessageW(A.blog, EM_REPLACESEL, FALSE, (LPARAM)L""); len = GetWindowTextLengthW(A.blog); }
        SendMessageW(A.blog, EM_SETSEL, len, len);
        SendMessageW(A.blog, EM_REPLACESEL, FALSE, (LPARAM)widen(add).c_str());
    }
    gamebuild::Stage st = A.runner.stage();
    static const int weight[] = {0, 2, 30, 3, 63, 2};  // per-stage share of the bar, in percent
    int idx = gamebuild::stage_index(st), overall = 0;
    for (int i = 1; i < idx; i++) overall += weight[i] * 10;
    if (idx) overall += weight[idx] * A.runner.percent() / 10;
    if (st == gamebuild::Stage::Done) overall = 1000;
    SendMessageW(A.bbar, PBM_SETPOS, overall, 0);
    std::wstring lab = idx ? L"Step " + std::to_wstring(idx) + L" of " + std::to_wstring(gamebuild::kStageCount) + L": " + gamebuild::stage_name(st)
                           : std::wstring(gamebuild::stage_name(st));
    std::string det = A.runner.detail();
    if ((st == gamebuild::Stage::Recompile || st == gamebuild::Stage::Compile) && !det.empty()) lab += L"\r\n" + widen(det) + L"  (" + std::to_wstring(A.runner.percent()) + L"%)";
    SetWindowTextW(A.bstage, lab.c_str());
    ULONGLONG sec = (GetTickCount64() - A.build_t0) / 1000;
    wchar_t el[32];
    swprintf(el, 32, L"%llu:%02llu:%02llu", sec / 3600, (sec / 60) % 60, sec % 60);
    SetWindowTextW(A.belapsed, el);
    if (A.runner.running()) { set_status(L"Building... you can keep using the PC, but it will be slow.", kAccent); return; }
    // finished
    KillTimer(A.wnd, 2);
    RedrawWindow(A.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    A.was_running = false;
    EnableWindow(A.back, TRUE);
    EnableWindow(A.next, TRUE);
    if (st == gamebuild::Stage::Done) {
        A.build_ok = !A.dry_run;
        if (!A.dry_run) { A.cfg.exe = install_exe().wstring(); A.cfg.build_state = L"done"; save_ini(); }
        refresh_build_page();
        set_status(A.dry_run ? L"Dry run finished - every step ran without compiling anything." : L"The game is built and installed. Press Next.", kOk);
        return;
    }
    if (!A.dry_run) { A.cfg.build_state = st == gamebuild::Stage::Cancelled ? L"stopped" : L"failed"; save_ini(); }
    refresh_build_page();
    if (st == gamebuild::Stage::Cancelled) set_status(L"Stopped. Press Continue build to pick up where it left off.", kAccent);
    else set_status(L"Build failed: " + widen(A.runner.error()), kBad);
}

// ---------- navigation ----------
void show_mode() {
    for (int i = 0; i < S_COUNT; i++) {
        ShowWindow(A.steps[i], A.wizard ? SW_SHOW : SW_HIDE);
        for (HWND h : A.page[i]) ShowWindow(h, (A.wizard && i == A.step) ? SW_SHOW : SW_HIDE);
    }
    for (HWND h : {A.head, A.help, A.status, A.back, A.next}) ShowWindow(h, A.wizard ? SW_SHOW : SW_HIDE);
    if (A.wizard && A.step == S_GAME) check_game();  // re-hide merge button as needed
    if (A.wizard && A.step == S_BUILD) refresh_build_page();
    for (HWND h : A.main_ctl) ShowWindow(h, A.wizard ? SW_HIDE : SW_SHOW);
    InvalidateRect(A.wnd, nullptr, TRUE);
}

void enter_step(int s) {
    A.step = s;
    for (int i = 0; i < S_COUNT; i++) {
        SendMessageW(A.steps[i], WM_SETFONT, (WPARAM)(i == s ? A.bold : A.font), TRUE);
        InvalidateRect(A.steps[i], nullptr, TRUE);
    }
    std::wstring h = L"Step " + std::to_wstring(s + 1) + L" of " + std::to_wstring(S_COUNT) + L":  " + kStepNames[s];
    SetWindowTextW(A.head, h.c_str());
    SetWindowTextW(A.help, kHelp[s]);
    EnableWindow(A.back, s > 0);
    SetWindowTextW(A.next, s == S_READY ? L"Finish" : (s == S_MODS ? L"Next (or skip) >" : L"Next >"));
    set_status(L"");
    switch (s) {
    case S_WELCOME: SetWindowTextW(A.page[S_WELCOME][0], welcome_text().c_str()); set_status(L"Press Next to begin."); break;
    case S_GAME: SetWindowTextW(A.game_edit, A.cfg.game.c_str()); break;
    case S_BUILD: {
        std::wstring src = A.cfg.source;
        if (!gamebuild::is_source_dir(src)) src = gamebuild::find_source(exe_dir()).wstring();
        SetWindowTextW(A.src_edit, src.c_str());
        refresh_build_page();
        break;
    }
    case S_USER: SetWindowTextW(A.user_edit, A.cfg.user.c_str()); check_user(); break;
    case S_PAD:
        SendMessageW(A.layout, CB_SETCURSEL, A.prefs.layout == -1 ? 0 : (A.prefs.layout == 1 ? 1 : 2), 0);
        SendMessageW(A.gyro, BM_SETCHECK, A.prefs.gyro ? BST_CHECKED : BST_UNCHECKED, 0);
        refresh_pads();
        break;
    case S_GFX: {
        detect_display();
        std::wstring n = L"Your display: " + std::to_wstring(A.disp_w) + L" x " + std::to_wstring(A.disp_h) + L" at " +
                         std::to_wstring(A.disp_hz) + L" Hz.  Suggested: " +
                         (recommended_scale() == 1 ? L"720p" : recommended_scale() == 2 ? L"1440p" : L"2160p") + L", " +
                         std::to_wstring(recommended_fps()) + L" FPS.";
        SetWindowTextW(A.gfx_note, n.c_str());
        if (!A.cfg.setup_done) { A.prefs.scale = recommended_scale(); A.prefs.fps = recommended_fps(); }
        SendMessageW(A.scale, CB_SETCURSEL, std::clamp(A.prefs.scale, 1, 3) - 1, 0);
        SendMessageW(A.fps, CB_SETCURSEL, A.prefs.fps >= 60 ? 1 : 0, 0);
        SendMessageW(A.fullscreen, BM_SETCHECK, A.cfg.fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
        fill_gpu_combo();
        set_status(L"Press Next to keep these.", kOk);
        break;
    }
    case S_MODS: refresh_mods_note(); set_status(L"Optional - press Next to skip."); break;
    case S_READY: fill_ready(); break;
    }
    show_mode();
}

void refresh_main() {
    gameinfo::Info info = gameinfo::read(fs::path(A.cfg.game));
    std::wstring name = info.long_name.empty() ? L"The Legend of Zelda: Breath of the Wild" : widen(info.long_name);
    for (auto& c : name) if (c == L'\n') c = L' ';
    SetWindowTextW(A.main_title, name.c_str());
    const wchar_t* sc[] = {L"720p", L"1440p", L"2160p"};
    mods::State st = mods::load(fs::path(A.cfg.user));
    std::wstring s = (info.has_title_version ? widen(gameinfo::version_name(info.title_version)) : std::wstring(L"version unknown")) +
                     (info.has_dlc ? L"  -  DLC" : L"") + L"  -  " + sc[std::clamp(A.prefs.scale, 1, 3) - 1] + L", " +
                     std::to_wstring(A.prefs.fps) + L" FPS" + (A.cfg.fullscreen ? L", fullscreen" : L"") + L"  -  " +
                     std::to_wstring(mods::enabled_count(st)) + L" mods on" + (A.cfg.no_mods ? L" (mods disabled)" : L"") +
                     L"\r\nSaves: " + A.cfg.user;
    SetWindowTextW(A.main_info, s.c_str());
    EnableWindow(GetDlgItem(A.wnd, ID_CLASSIC), is_file(exe_dir() / L"BotWLauncherClassic.exe"));
    set_status(process_running(L"BotWRecomp.exe") ? L"The game is running." : L"Ready.");
}

void finish_wizard() {
    A.cfg.setup_done = true;
    save_ini();
    setupwizard::write_game_prefs(fs::path(A.cfg.user), A.prefs);
    A.wizard = false;
    show_mode();
    refresh_main();
}

void go_next() {
    switch (A.step) {
    case S_GAME:
        check_game();
        if (!A.game_ok) { MessageBeep(MB_ICONWARNING); return; }
        break;
    case S_BUILD:
        if (!build_step_ok()) { MessageBeep(MB_ICONWARNING); refresh_build_page(); return; }
        if (!A.build_ok && prebuilt_present()) A.cfg.exe = install_exe().wstring();
        break;
    case S_USER:
        if (!check_user()) { MessageBeep(MB_ICONWARNING); return; }
        break;
    case S_PAD: {
        int l = (int)SendMessageW(A.layout, CB_GETCURSEL, 0, 0);
        A.prefs.layout = l == 0 ? -1 : (l == 1 ? 1 : 0);
        A.prefs.gyro = SendMessageW(A.gyro, BM_GETCHECK, 0, 0) == BST_CHECKED;
        break;
    }
    case S_GFX:
        A.prefs.scale = (int)SendMessageW(A.scale, CB_GETCURSEL, 0, 0) + 1;
        A.prefs.fps = SendMessageW(A.fps, CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;
        A.cfg.fullscreen = SendMessageW(A.fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED;
        {
            int g = (int)SendMessageW(A.gpu, CB_GETCURSEL, 0, 0);
            if (g <= 0) {
                A.prefs.gpu.clear();
                A.prefs.gpu_name.clear();
            } else if (g != A.gpu_missing && g - 1 < (int)A.gpus.size()) {
                A.prefs.gpu = gpupick::uuid_hex(A.gpus[g - 1].uuid);
                A.prefs.gpu_name = A.gpus[g - 1].name;
            }
        }
        break;
    case S_READY: finish_wizard(); return;
    }
    enter_step(A.step + 1);
}

std::wstring build_env(const fs::path& root, const fs::path& exe) {
    std::wstring extra;
    if (GetFileAttributesW(L"C:\\msys64\\ucrt64\\bin") != INVALID_FILE_ATTRIBUTES) extra += L"C:\\msys64\\ucrt64\\bin;";
    extra += exe.parent_path().wstring() + L";";
    std::wstring out;
    bool saw_path = false;
    if (wchar_t* cur = GetEnvironmentStringsW()) {
        for (wchar_t* p = cur; *p;) {
            std::wstring e = p;
            p += e.size() + 1;
            if (_wcsnicmp(e.c_str(), L"PATH=", 5) == 0) { e = L"PATH=" + extra + e.substr(5); saw_path = true; }
            else if (e.rfind(L"BOTW_USER_ROOT=", 0) == 0) continue;
            out += e;
            out.push_back(L'\0');
        }
        FreeEnvironmentStringsW(cur);
    }
    if (!saw_path) { out += L"PATH=" + extra; out.push_back(L'\0'); }
    out += L"BOTW_USER_ROOT=" + root.wstring();
    out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}

void play() {
    if (process_running(L"BotWRecomp.exe")) { set_status(L"The game is already running.", kBad); return; }
    if (A.cfg.exe.empty() || !is_file(A.cfg.exe)) { set_status(L"BotWRecomp.exe was not found next to the launcher.", kBad); return; }
    if (!is_file(fs::path(A.cfg.game) / "code" / "U-King.rpx")) { set_status(L"The game folder is missing. Open Settings to choose it again.", kBad); return; }
    std::error_code ec;
    fs::create_directories(fs::path(A.cfg.user), ec);
    // A trailing backslash right before the closing quote would escape the quote.
    std::wstring game = A.cfg.game;
    while (game.size() > 3 && (game.back() == L'\\' || game.back() == L'/')) game.pop_back();
    std::wstring cmd = L"\"" + A.cfg.exe + L"\" --game \"" + game + L"\" --renderer vulkan";
    int secs = _wtoi(A.cfg.seconds.c_str());
    if (secs > 0) cmd += L" --seconds " + std::to_wstring(secs);
    if (A.cfg.fullscreen) cmd += L" --fullscreen";
    if (A.cfg.gamepad_view) cmd += L" --gamepad-view";
    if (A.cfg.no_mods) cmd += L" --no-mods";
    std::vector<wchar_t> mc(cmd.begin(), cmd.end());
    mc.push_back(0);
    std::wstring env = build_env(fs::path(A.cfg.user), fs::path(A.cfg.exe));
    std::wstring cwd = fs::path(A.cfg.exe).parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(A.cfg.exe.c_str(), mc.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(), cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        set_status(L"Starting the game...", kOk);
    } else {
        set_status(L"Could not start the game (error " + std::to_wstring(GetLastError()) + L").", kBad);
    }
}

// ---------- mods window ----------
HWND g_mods_wnd{}, g_mods_list{};
mods::State g_mods;

LRESULT CALLBACK mods_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_MODSAVE) {
            for (size_t i = 0; i < g_mods.list.size(); i++) g_mods.list[i].enabled = ListView_GetCheckState(g_mods_list, (int)i) != 0;
            std::string err;
            if (!mods::save(fs::path(A.cfg.user), g_mods, false, err)) MessageBoxW(w, widen(err).c_str(), kTitle, MB_ICONWARNING);
            DestroyWindow(w);
        } else if (LOWORD(wp) == IDCANCEL) DestroyWindow(w);
        else if (LOWORD(wp) == ID_MOD_ADD || LOWORD(wp) == ID_MOD_ADDDIR) {
            std::wstring src = LOWORD(wp) == ID_MOD_ADD ? pick_zip(w) : pick_folder(w, L"Choose a mod folder", L"");
            if (!src.empty()) {
                std::string folder, err;
                if (!mods::install_mod(fs::path(A.cfg.user), fs::path(src), folder, err)) MessageBoxW(w, widen(err).c_str(), kTitle, MB_ICONWARNING);
                g_mods = mods::load(fs::path(A.cfg.user));
                ListView_DeleteAllItems(g_mods_list);
                for (size_t i = 0; i < g_mods.list.size(); i++) {
                    std::wstring n = widen(g_mods.list[i].name);
                    LVITEMW it{LVIF_TEXT, (int)i};
                    it.pszText = n.data();
                    ListView_InsertItem(g_mods_list, &it);
                    ListView_SetCheckState(g_mods_list, (int)i, g_mods.list[i].enabled);
                }
            }
        }
        return 0;
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY: EnableWindow(A.wnd, TRUE); SetForegroundWindow(A.wnd); g_mods_wnd = nullptr; refresh_main(); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void show_mods() {
    if (g_mods_wnd) return;
    RECT r{0, 0, 520, 380};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    g_mods_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BotWNativeMods", L"Mods", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, A.wnd, nullptr, A.inst, nullptr);
    auto c = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int x, int y, int ww, int h, int id) {
        HWND hh = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, x, y, ww, h, g_mods_wnd, (HMENU)(INT_PTR)id, A.inst, nullptr);
        SendMessageW(hh, WM_SETFONT, (WPARAM)A.font, TRUE);
        return hh;
    };
    c(L"STATIC", L"Tick a mod to turn it on. Mods lower in the list win when two change the same file.", SS_LEFT, 12, 10, 496, 20, 0);
    g_mods_list = c(WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 12, 34, 496, 280, ID_MODLIST);
    ListView_SetExtendedListViewStyle(g_mods_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNW col{LVCF_WIDTH};
    col.cx = 470;
    ListView_InsertColumn(g_mods_list, 0, &col);
    c(L"BUTTON", L"Add archive...", BS_PUSHBUTTON, 12, 330, 110, 28, ID_MOD_ADD);
    c(L"BUTTON", L"Add folder...", BS_PUSHBUTTON, 128, 330, 110, 28, ID_MOD_ADDDIR);
    c(L"BUTTON", L"Cancel", BS_PUSHBUTTON, 312, 330, 90, 28, IDCANCEL);
    c(L"BUTTON", L"Save", BS_DEFPUSHBUTTON, 410, 330, 98, 28, ID_MODSAVE);
    g_mods = mods::load(fs::path(A.cfg.user));
    for (size_t i = 0; i < g_mods.list.size(); i++) {
        std::wstring n = widen(g_mods.list[i].name);
        LVITEMW it{LVIF_TEXT, (int)i};
        it.pszText = n.data();
        ListView_InsertItem(g_mods_list, &it);
        ListView_SetCheckState(g_mods_list, (int)i, g_mods.list[i].enabled);
    }
    EnableWindow(A.wnd, FALSE);
}

int ask_rebuild(HWND w) {
    return MessageBoxW(w, L"Rebuild the game from the source folder? This recompiles and compiles everything that changed "
                          L"and can take a long time. The current game program is kept as BotWRecomp.old.exe.", kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
}

// ---------- main window ----------
LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        COLORREF col = GetSysColor(COLOR_WINDOWTEXT);
        if (c == A.status || c == A.main_status) col = A.status_color ? A.status_color : GetSysColor(COLOR_GRAYTEXT);
        else if (c == A.head || c == A.main_title) col = kAccent;
        else if (A.wizard && c == A.steps[A.step]) col = kAccent;
        else for (int i = 0; i < S_COUNT; i++) if (c == A.steps[i]) col = i < A.step ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_GRAYTEXT);
        SetTextColor(dc, col);
        for (int i = 0; i < S_COUNT; i++) if (c == A.steps[i]) return (LRESULT)A.side;
        return (LRESULT)A.white;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (A.wizard) {
            RECT side{0, 0, 180, 560};
            HBRUSH b = CreateSolidBrush(RGB(244, 245, 247));
            FillRect(dc, &side, b);
            DeleteObject(b);
            RECT bar{180, 0, 181, 560};
            HBRUSH l = CreateSolidBrush(RGB(220, 222, 226));
            FillRect(dc, &bar, l);
            RECT sel{14, 20 + A.step * 28, 17, 20 + A.step * 28 + 20};
            HBRUSH a = CreateSolidBrush(kAccent);
            FillRect(dc, &sel, a);
            DeleteObject(a);
            RECT line{200, 500, 700, 501};
            FillRect(dc, &line, l);
            DeleteObject(l);
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 2) { tick_build(); return 0; }
        if (A.wizard && A.step == S_PAD) refresh_pads();
        if (!A.wizard && !g_mods_wnd) {
            bool run = process_running(L"BotWRecomp.exe");
            EnableWindow(A.play, !run);
        }
        return 0;
    case WM_MERGE_PROGRESS:
        if (lp > 0) set_status(L"Combining... " + std::to_wstring((int)(100.0 * wp / lp)) + L"%", kAccent);
        return 0;
    case WM_MERGE_DONE:
        A.merging = false;
        EnableWindow(A.next, TRUE);
        EnableWindow(A.back, TRUE);
        EnableWindow(A.game_merge, TRUE);
        if (wp) { SetWindowTextW(A.game_edit, A.merge_target.wstring().c_str()); check_game(); }
        else set_status(L"Combining failed: " + widen(A.merge_err), kBad);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (A.merging) return 0;
        switch (id) {
        case ID_BACK: if (A.step > 0) enter_step(A.step - 1); break;
        case ID_NEXT: go_next(); break;
        case ID_GAME_BROWSE: {
            auto p = pick_folder(w, L"Choose your game folder (or the folder holding Base and Update)", text_of(A.game_edit));
            if (!p.empty()) SetWindowTextW(A.game_edit, p.c_str());
            check_game();
            break;
        }
        case ID_GAME_EDIT: if (HIWORD(wp) == EN_KILLFOCUS) check_game(); break;
        case ID_USER_EDIT: if (HIWORD(wp) == EN_KILLFOCUS) check_user(); break;
        case ID_GAME_MERGE: start_merge(); break;
        case ID_SRC_BROWSE: {
            auto p = pick_folder(w, L"Choose the BotW-Native source folder", text_of(A.src_edit));
            if (!p.empty()) SetWindowTextW(A.src_edit, p.c_str());
            refresh_build_page();
            break;
        }
        case ID_SRC_EDIT: if (HIWORD(wp) == EN_KILLFOCUS) refresh_build_page(); break;
        case ID_TC_RECHECK: case ID_PREBUILT: refresh_build_page(); break;
        case ID_TC_INSTALL: install_tools(); break;
        case ID_TC_LINK: ShellExecuteW(w, L"open", L"https://www.msys2.org/", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_BUILD:
            if (A.cfg.setup_done && ask_rebuild(w) != IDYES) break;
            start_build();
            break;
        case ID_BUILD_STOP: A.runner.cancel(); break;
        case ID_USER_BROWSE: {
            auto p = pick_folder(w, L"Choose the saves folder", text_of(A.user_edit));
            if (!p.empty()) { SetWindowTextW(A.user_edit, p.c_str()); check_user(); }
            break;
        }
        case ID_USER_DEFAULT: SetWindowTextW(A.user_edit, default_user().c_str()); check_user(); break;
        case ID_MOD_ADD: add_mod(pick_zip(w)); break;
        case ID_MOD_ADDDIR: add_mod(pick_folder(w, L"Choose a mod folder", L"")); break;
        case ID_PLAY: play(); break;
        case ID_MODS: show_mods(); break;
        case ID_SETTINGS: A.wizard = true; enter_step(S_GAME); break;
        case ID_OPEN_GAME: if (is_dir(A.cfg.game)) open_folder(A.cfg.game); break;
        case ID_OPEN_USER: open_folder(A.cfg.user); break;
        case ID_OPEN_LOG: {
            fs::path log = fs::path(A.cfg.user) / L"runtime.log";
            if (is_file(log)) ShellExecuteW(w, L"open", log.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else set_status(L"No log yet - it appears after the game has run once.");
            break;
        }
        case ID_CLASSIC: ShellExecuteW(w, L"open", (exe_dir() / L"BotWLauncherClassic.exe").wstring().c_str(), nullptr, exe_dir().wstring().c_str(), SW_SHOWNORMAL); break;
        }
        return 0;
    }
    case WM_CLOSE:
        if (A.merging) return 0;
        if (A.runner.running()) {
            if (MessageBoxW(w, L"Stop the build and close? Finished parts are kept and the next start continues from there.", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            A.runner.cancel();
            A.cfg.build_state = L"stopped";
            save_ini();
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

}  // namespace

static int run(HINSTANCE inst, int show) {
    A.inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    A.sdl_ok = SDL_Init(SDL_INIT_GAMEPAD);

    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    A.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    A.bold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 3 / 2;
    A.title = CreateFontIndirectW(&lf);
    A.white = GetSysColorBrush(COLOR_WINDOW);
    A.mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    A.side = CreateSolidBrush(RGB(244, 245, 247));

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"BotWNativeLauncher";
    RegisterClassW(&wc);
    wc.lpfnWndProc = mods_proc;
    wc.lpszClassName = L"BotWNativeMods";
    RegisterClassW(&wc);

    load_ini();
    A.prefs = setupwizard::read_game_prefs(fs::path(A.cfg.user));
    bool ready = A.cfg.setup_done && is_file(fs::path(A.cfg.game) / "code" / "U-King.rpx");
    A.wizard = !ready;
    // Developer check of the build step: runs every stage without compiling or installing.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; i++) if (wcscmp(argv[i], L"--dry-run-build") == 0) A.dry_run = true;
        if (argv) LocalFree(argv);
    }

    RECT r{0, 0, 720, 560};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    A.wnd = CreateWindowExW(0, L"BotWNativeLauncher", kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    build_wizard();
    build_main();
    if (A.dry_run) { A.wizard = true; enter_step(S_BUILD); if (IsWindowEnabled(A.build_btn)) start_build(); }
    else if (A.wizard) enter_step(S_WELCOME);
    else { show_mode(); refresh_main(); }
    SetTimer(A.wnd, 1, 700, nullptr);
    ShowWindow(A.wnd, show);
    UpdateWindow(A.wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        HWND top = g_mods_wnd ? g_mods_wnd : A.wnd;
        if (IsDialogMessageW(top, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SDL_Quit();
    CoUninitialize();
    return 0;
}

int main() { return run(GetModuleHandleW(nullptr), SW_SHOWNORMAL); }
