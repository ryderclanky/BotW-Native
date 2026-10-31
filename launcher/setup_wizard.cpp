#include "setup_wizard.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace setupwizard {
namespace {

namespace fs = std::filesystem;

bool file_exists(const fs::path& p) {
    if (p.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool dir_exists(const fs::path& p) {
    if (p.empty()) return false;
    std::error_code ec;
    return fs::is_directory(p, ec);
}

fs::path find_repo_root(const fs::path& start) {
    if (start.empty()) return {};
    fs::path cur = start;
    for (int depth = 0; depth < 4 && !cur.empty(); depth++) {
        if (file_exists(cur / "tools" / "recompile.py") && file_exists(cur / "CMakeLists.txt")) {
            return cur;
        }
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

#ifdef _WIN32
fs::path search_path_exe(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = SearchPathW(nullptr, name, L".exe", 32768, buf, nullptr);
    if (n > 0 && n < 32768) return fs::path(buf);
    return {};
}
#endif

std::string trim_cr(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

bool parse_bracket_pair(const std::string& s, size_t open_pos, int& a, int& b, size_t& close_pos) {
    if (open_pos >= s.size() || s[open_pos] != '[') return false;
    size_t slash = s.find('/', open_pos + 1);
    if (slash == std::string::npos) return false;
    close_pos = s.find(']', slash + 1);
    if (close_pos == std::string::npos) return false;
    auto parse_int = [&](size_t start, size_t end, int& out) -> bool {
        while (start < end && s[start] == ' ') start++;
        while (end > start && s[end - 1] == ' ') end--;
        if (start >= end) return false;
        int val = 0;
        for (size_t i = start; i < end; i++) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            val = val * 10 + (s[i] - '0');
        }
        out = val;
        return true;
    };
    return parse_int(open_pos + 1, slash, a) && parse_int(slash + 1, close_pos, b);
}

}  // namespace

ToolchainStatus inspect_toolchain(const fs::path& launcher_dir, const fs::path& configured_exe) {
    ToolchainStatus st;

    st.repo_dir = find_repo_root(launcher_dir);
    if (st.repo_dir.empty() && !configured_exe.empty()) {
        st.repo_dir = find_repo_root(configured_exe.parent_path());
    }
    if (st.repo_dir.empty()) {
        std::error_code ec;
        st.repo_dir = find_repo_root(fs::current_path(ec));
    }
    st.has_repo = !st.repo_dir.empty();

    if (file_exists(configured_exe)) {
        st.exe_path = configured_exe;
    } else if (!launcher_dir.empty() && file_exists(launcher_dir / "BotWRecomp.exe")) {
        st.exe_path = launcher_dir / "BotWRecomp.exe";
    } else if (st.has_repo && file_exists(st.repo_dir / "build" / "BotWRecomp.exe")) {
        st.exe_path = st.repo_dir / "build" / "BotWRecomp.exe";
    } else if (st.has_repo && file_exists(st.repo_dir / "BotWRecomp.exe")) {
        st.exe_path = st.repo_dir / "BotWRecomp.exe";
    }
    st.has_exe = !st.exe_path.empty();

    if (st.has_repo && dir_exists(st.repo_dir / "generated")) {
        std::error_code ec;
        int count = 0;
        for (const auto& entry : fs::directory_iterator(st.repo_dir / "generated", ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".cpp") count++;
        }
        st.generated_cpp_count = count;
        st.has_generated_cpp = file_exists(st.repo_dir / "generated" / "table.cpp") && count >= 10;
    }

    const fs::path msys_candidates[] = {
        L"C:\\msys64\\ucrt64\\bin",
        L"C:\\msys2\\ucrt64\\bin",
        L"D:\\msys64\\ucrt64\\bin",
    };
    for (const auto& cand : msys_candidates) {
        if (dir_exists(cand)) {
            st.msys_bin = cand;
            break;
        }
    }

    if (!st.msys_bin.empty()) {
        if (file_exists(st.msys_bin / "python3.exe")) st.python_exe = st.msys_bin / "python3.exe";
        else if (file_exists(st.msys_bin / "python.exe")) st.python_exe = st.msys_bin / "python.exe";
        if (file_exists(st.msys_bin / "g++.exe")) st.gxx_exe = st.msys_bin / "g++.exe";
        if (file_exists(st.msys_bin / "cmake.exe")) st.cmake_exe = st.msys_bin / "cmake.exe";
        if (file_exists(st.msys_bin / "ninja.exe")) st.ninja_exe = st.msys_bin / "ninja.exe";
    }

#ifdef _WIN32
    if (st.python_exe.empty()) st.python_exe = search_path_exe(L"python3");
    if (st.python_exe.empty()) st.python_exe = search_path_exe(L"python");
    if (st.gxx_exe.empty()) st.gxx_exe = search_path_exe(L"g++");
    if (st.cmake_exe.empty()) st.cmake_exe = search_path_exe(L"cmake");
    if (st.ninja_exe.empty()) st.ninja_exe = search_path_exe(L"ninja");

    HMODULE vk = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (vk) {
        st.has_vulkan = true;
        FreeLibrary(vk);
    } else {
        wchar_t sysdir[MAX_PATH];
        if (GetSystemDirectoryW(sysdir, MAX_PATH)) {
            st.has_vulkan = file_exists(fs::path(sysdir) / "vulkan-1.dll");
        }
    }
#endif

    st.has_python = !st.python_exe.empty();
    st.has_compiler = !st.gxx_exe.empty() && !st.cmake_exe.empty() && !st.ninja_exe.empty();
    return st;
}

bool parse_progress_line(const std::string& raw_line, BuildProgress& state) {
    state.matched = false;
    std::string line = trim_cr(raw_line);
    if (line.empty()) return false;

    // 1. Check for [recomp k/N] lines emitted by tools/recompile.py
    if (line.rfind("[recomp ", 0) == 0) {
        size_t close = line.find(']');
        if (close != std::string::npos) {
            std::string tag = "[" + line.substr(8, close - 8) + "]";
            int step = 0, total = 4;
            size_t dummy = 0;
            if (parse_bracket_pair(tag, 0, step, total, dummy) && total > 0) {
                state.stage = BuildStage::RecompileWiiUToCpp;
                state.recompile_step = step;
                state.recompile_total = total;
                std::string rest = trim_cr(line.substr(close + 1));

                if (step == 1) {
                    state.sub_done = 0;
                    state.sub_total = 0;
                    state.stage_percent = 5;
                    state.overall_percent = 2;
                    state.current_item = "U-King.rpx";
                    state.phase_text = "Step 1/2: Decompressing & parsing Wii U U-King.rpx...";
                } else if (step == 2) {
                    state.sub_done = 0;
                    state.sub_total = 0;
                    state.stage_percent = 12;
                    state.overall_percent = 6;
                    state.current_item = "PowerPC CFG & jump tables";
                    state.phase_text = "Step 1/2: " + rest;
                } else if (step == 3) {
                    // Format: Generating C++ shard 42/258 (16%) [fn_020c0000.cpp]...
                    int shard_done = 0, shard_total = 0;
                    size_t shard_pos = rest.find("shard ");
                    if (shard_pos != std::string::npos) {
                        size_t num_start = shard_pos + 6;
                        size_t slash = rest.find('/', num_start);
                        if (slash != std::string::npos) {
                            shard_done = std::atoi(rest.c_str() + num_start);
                            shard_total = std::atoi(rest.c_str() + slash + 1);
                        }
                    }
                    size_t br_open = rest.find('[');
                    size_t br_close = rest.find(']', br_open == std::string::npos ? 0 : br_open);
                    if (br_open != std::string::npos && br_close != std::string::npos) {
                        state.current_item = rest.substr(br_open + 1, br_close - br_open - 1);
                    }
                    if (shard_total > 0) {
                        state.sub_done = shard_done;
                        state.sub_total = shard_total;
                        int pct = (shard_done * 100) / shard_total;
                        state.stage_percent = std::clamp(12 + (shard_done * 83) / shard_total, 12, 95);
                        state.overall_percent = std::clamp((state.stage_percent * 45) / 100, 6, 45);
                        state.phase_text = "Step 1/2: Converting Wii U binary to C++ (shard " +
                                           std::to_string(shard_done) + " / " + std::to_string(shard_total) +
                                           ", " + std::to_string(pct) + "%)...";
                    } else {
                        state.stage_percent = 50;
                        state.overall_percent = 22;
                        state.phase_text = "Step 1/2: " + rest;
                    }
                } else if (step >= 4) {
                    if (state.sub_total > 0) state.sub_done = state.sub_total;
                    state.stage_percent = 100;
                    state.overall_percent = 45;
                    state.current_item = "table.cpp & hle_stubs.cpp";
                    state.phase_text = "Step 1/2: Writing C++ dispatch table & HLE stubs (100%)...";
                }
                state.matched = true;
                return true;
            }
        }
    }

    // 2. Check for Ninja [k/N] lines
    if (line[0] == '[') {
        int done = 0, total = 0;
        size_t close_pos = 0;
        if (parse_bracket_pair(line, 0, done, total, close_pos) && total > 0 && done <= total) {
            state.stage = BuildStage::CompileCppToExe;
            state.sub_done = done;
            state.sub_total = total;
            int pct = (done * 100) / total;
            state.stage_percent = std::clamp(pct, 0, 100);
            state.overall_percent = std::clamp(45 + (pct * 55) / 100, 45, 100);
            std::string rest = trim_cr(line.substr(close_pos + 1));
            size_t last_slash = rest.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                state.current_item = rest.substr(last_slash + 1);
            } else {
                state.current_item = rest;
            }
            if (rest.find("Linking") != std::string::npos) {
                state.phase_text = "Step 2/2: Linking BotWRecomp.exe (" +
                                   std::to_string(done) + " / " + std::to_string(total) + ", 100%)...";
            } else {
                state.phase_text = "Step 2/2: Compiling C++ to native executable (" +
                                   std::to_string(done) + " / " + std::to_string(total) +
                                   ", " + std::to_string(pct) + "%)...";
            }
            state.matched = true;
            return true;
        }
    }

    // 3. CMake configure lines
    if (line.rfind("-- Configuring done", 0) == 0 || line.rfind("-- Generating done", 0) == 0) {
        state.stage = BuildStage::CompileCppToExe;
        state.stage_percent = 2;
        state.overall_percent = 46;
        state.current_item = "CMakeLists.txt";
        state.phase_text = "Step 2/2: Configured CMake + Ninja build...";
        state.matched = true;
        return true;
    }

    return false;
}

void deploy_runtime_files(const fs::path& exe_dir, const fs::path& repo_dir) {
    if (exe_dir.empty()) return;
    std::error_code ec;
    fs::create_directories(exe_dir, ec);
    if (!repo_dir.empty()) {
        fs::path db_src = repo_dir / "runtime" / "gamecontrollerdb.txt";
        fs::path db_dst = exe_dir / "gamecontrollerdb.txt";
        if (file_exists(db_src)) {
            fs::copy_file(db_src, db_dst, fs::copy_options::overwrite_existing, ec);
        }
    }
}

GamePrefs read_game_prefs(const fs::path& user_root) {
    GamePrefs p;
    if (user_root.empty()) return p;
    std::ifstream in(user_root / "settings.ini");
    if (!in) return p;
    std::string line;
    while (std::getline(in, line)) {
        line = trim_cr(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_cr(line.substr(0, eq));
        std::string v = trim_cr(line.substr(eq + 1));
        if (k == "scale") p.scale = std::clamp(std::atoi(v.c_str()), 1, 4);
        else if (k == "fps") p.fps = std::atoi(v.c_str()) >= 45 ? 60 : 30;
        else if (k == "layout") p.layout = std::clamp(std::atoi(v.c_str()), -1, 1);
        else if (k == "gamepad_view") p.gamepad_view = (v != "0" && v != "false");
        else if (k == "gyro") p.gyro = (v != "0" && v != "false");
        else if (k == "gpu") p.gpu = v;
        else if (k == "gpu_name") p.gpu_name = v;
    }
    return p;
}

bool write_game_prefs(const fs::path& user_root, const GamePrefs& prefs) {
    if (user_root.empty()) return false;
    std::error_code ec;
    fs::create_directories(user_root, ec);
    fs::path path = user_root / "settings.ini";
    std::vector<std::string> lines;
    bool saw_scale = false, saw_fps = false, saw_layout = false, saw_gpv = false, saw_gyro = false, saw_gpu = false,
         saw_gpu_name = false;
    if (file_exists(path)) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::string clean = trim_cr(line);
            auto eq = clean.find('=');
            if (eq != std::string::npos && clean[0] != '#' && clean[0] != ';') {
                std::string k = trim_cr(clean.substr(0, eq));
                if (k == "scale") {
                    lines.push_back("scale=" + std::to_string(prefs.scale));
                    saw_scale = true;
                    continue;
                }
                if (k == "fps") {
                    lines.push_back("fps=" + std::to_string(prefs.fps));
                    saw_fps = true;
                    continue;
                }
                if (k == "layout") {
                    lines.push_back("layout=" + std::to_string(prefs.layout));
                    saw_layout = true;
                    continue;
                }
                if (k == "gamepad_view") {
                    lines.push_back(std::string("gamepad_view=") + (prefs.gamepad_view ? "1" : "0"));
                    saw_gpv = true;
                    continue;
                }
                if (k == "gpu") {
                    lines.push_back("gpu=" + prefs.gpu);
                    saw_gpu = true;
                    continue;
                }
                if (k == "gpu_name") {
                    lines.push_back("gpu_name=" + prefs.gpu_name);
                    saw_gpu_name = true;
                    continue;
                }
                if (k == "gyro") {
                    lines.push_back(std::string("gyro=") + (prefs.gyro ? "1" : "0"));
                    saw_gyro = true;
                    continue;
                }
            }
            lines.push_back(clean);
        }
    }
    if (!saw_scale) lines.push_back("scale=" + std::to_string(prefs.scale));
    if (!saw_fps) lines.push_back("fps=" + std::to_string(prefs.fps));
    if (!saw_layout) lines.push_back("layout=" + std::to_string(prefs.layout));
    if (!saw_gpv) lines.push_back(std::string("gamepad_view=") + (prefs.gamepad_view ? "1" : "0"));
    if (!saw_gyro) lines.push_back(std::string("gyro=") + (prefs.gyro ? "1" : "0"));
    if (!saw_gpu) lines.push_back("gpu=" + prefs.gpu);
    if (!saw_gpu_name) lines.push_back("gpu_name=" + prefs.gpu_name);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (const auto& l : lines) out << l << "\n";
    return static_cast<bool>(out);
}

}  // namespace setupwizard
