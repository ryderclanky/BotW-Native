#include "game_build.h"

#include "game_info.h"
#include "setup_wizard.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gamebuild {

namespace {
bool is_file(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring q(const fs::path& p) {
    std::wstring s = p.wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();  // keep the closing quote
    return L"\"" + s + L"\"";
}
}  // namespace

Toolchain detect_toolchain() {
    Toolchain t;
    std::vector<fs::path> roots;
    wchar_t sys[8] = {};
    for (wchar_t d = L'C'; d <= L'Z'; d++) {
        swprintf(sys, 8, L"%c:\\", d);
        if (GetDriveTypeW(sys) != DRIVE_FIXED) continue;
        roots.push_back(fs::path(sys) / L"msys64");
        roots.push_back(fs::path(sys) / L"msys2");
    }
    for (auto& r : roots) {
        fs::path bin = r / L"ucrt64" / L"bin";
        if (!is_file(bin / L"g++.exe") && !is_file(r / L"usr" / L"bin" / L"pacman.exe")) continue;
        t.msys_root = r;
        t.ucrt_bin = bin;
        t.gxx = is_file(bin / L"g++.exe");
        t.cmake = is_file(bin / L"cmake.exe");
        t.ninja = is_file(bin / L"ninja.exe");
        t.python = is_file(bin / L"python3.exe") || is_file(bin / L"python.exe");
        break;
    }
    return t;
}

bool is_source_dir(const fs::path& dir) {
    return !dir.empty() && is_file(dir / "tools" / "recompile.py") && is_file(dir / "CMakeLists.txt");
}

fs::path find_source(const fs::path& start) {
    fs::path cur = start;
    for (int i = 0; i < 4 && !cur.empty(); i++) {
        if (is_source_dir(cur)) return cur;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(cur, ec))
            if (e.is_directory(ec) && is_source_dir(e.path())) return e.path();
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

int auto_jobs(int* cores_out, double* ram_gb_out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = (int)std::max<DWORD>(1, si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    double gb = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    double avail = ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
    // One compiler process on the generated game code peaks around 0.6-0.7 GB. A full -j16 build on a
    // 16 GB PC pushed it into heavy paging, so leave 4 GB of the total for the system and also stay
    // inside the memory that is actually free right now (other apps keep running during the build).
    int by_total = (int)((gb - 4.0) / 1.0);
    int by_free = (int)((avail - 1.5) / 0.75);
    int by_ram = std::max(1, std::min(by_total, by_free));
    if (cores_out) *cores_out = cores;
    if (ram_gb_out) *ram_gb_out = gb;
    return std::clamp(std::min(cores, by_ram), 1, 64);
}

const wchar_t* pacman_packages() {
    return L"mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja "
           L"mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-vulkan-headers "
           L"mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-libpng "
           L"mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-glm mingw-w64-ucrt-x86_64-boost "
           L"mingw-w64-ucrt-x86_64-fmt mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-glslang";
}

const wchar_t* stage_name(Stage s) {
    switch (s) {
    case Stage::Prepare: return L"Checking the game code";
    case Stage::Recompile: return L"Recompiling game code to C++";
    case Stage::Configure: return L"Configuring the build";
    case Stage::Compile: return L"Compiling";
    case Stage::Install: return L"Copying the game program";
    case Stage::Done: return L"Finished";
    case Stage::Failed: return L"Failed";
    case Stage::Cancelled: return L"Stopped";
    default: return L"Not started";
    }
}
int stage_index(Stage s) {
    switch (s) {
    case Stage::Prepare: return 1;
    case Stage::Recompile: return 2;
    case Stage::Configure: return 3;
    case Stage::Compile: return 4;
    case Stage::Install: return 5;
    default: return 0;
    }
}

bool codegen_complete(const fs::path& source, const std::string& sha1) {
    const char* probes = std::getenv("BOTW_PROBES");
    return gameinfo::codegen_cache_matches(source, sha1, probes ? probes : "");
}

// --- PE import walk ---
static std::vector<std::string> pe_imports(const fs::path& file) {
    std::vector<std::string> out;
    std::ifstream f(file, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    auto rd32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? *(uint32_t*)&d[o] : 0; };
    auto rd16 = [&](size_t o) -> uint16_t { return o + 2 <= d.size() ? *(uint16_t*)&d[o] : 0; };
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return out;
    size_t pe = rd32(0x3c);
    if (rd32(pe) != 0x4550) return out;
    size_t coff = pe + 4;
    int nsec = rd16(coff + 2);
    size_t opt = coff + 20;
    uint16_t magic = rd16(opt);
    size_t dd = opt + (magic == 0x20b ? 112 : 96);
    uint32_t imp_rva = rd32(dd + 8);
    size_t sec = opt + rd16(coff + 16);
    auto rva2off = [&](uint32_t rva) -> size_t {
        for (int i = 0; i < nsec; i++) {
            size_t s = sec + i * 40;
            uint32_t va = rd32(s + 12), sz = std::max(rd32(s + 8), rd32(s + 16)), raw = rd32(s + 20);
            if (rva >= va && rva < va + sz) return raw + (rva - va);
        }
        return 0;
    };
    size_t o = rva2off(imp_rva);
    for (; o && o + 20 <= d.size(); o += 20) {
        uint32_t name = rd32(o + 12);
        if (!name) break;
        size_t no = rva2off(name);
        std::string n;
        while (no && no < d.size() && d[no]) n.push_back(d[no++]);
        out.push_back(n);
    }
    return out;
}

static bool is_system_runtime(const std::string& name) {
    std::string folded = name;
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (folded.rfind("api-ms-win-", 0) == 0 || folded.rfind("ext-ms-win-", 0) == 0 ||
        folded == "vulkan-1.dll" || folded == "libvulkan-1.dll")
        return true;
    wchar_t system_dir[MAX_PATH + 1] = {};
    UINT n = GetSystemDirectoryW(system_dir, MAX_PATH + 1);
    if (!n || n >= MAX_PATH + 1) return false;
    std::error_code ec;
    return fs::is_regular_file(fs::path(std::wstring(system_dir, n)) / fs::path(name), ec);
}

// The launcher loads the same MinGW runtime DLLs from its own folder, so
// overwriting them in place fails with "Permission denied". Skip identical
// files, and otherwise move the locked one aside first (Windows allows
// renaming a DLL that is in use) and copy the new one in its place.
static bool same_file_contents(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::is_regular_file(b, ec)) return false;
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec || sa != sb) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::istreambuf_iterator<char> ia(fa), ib(fb), end;
    return std::equal(ia, end, ib, end);
}

static bool copy_dll(const fs::path& src, const fs::path& dst, std::error_code& ec) {
    ec.clear();
    if (same_file_contents(src, dst)) return true;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (!ec) return true;
    std::error_code ec2;
    if (!fs::exists(dst, ec2)) return false;
    // A previous .old may itself still be loaded, so fall back to numbered names.
    for (int i = 0; i < 100; ++i) {
        fs::path old = dst;
        old += i == 0 ? std::string(".old") : ".old" + std::to_string(i);
        fs::remove(old, ec2);
        if (fs::exists(old, ec2)) continue;
        fs::rename(dst, old, ec2);
        if (ec2) return false;
        ec.clear();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }
    return false;
}

bool install_with_dlls(const fs::path& exe, const fs::path& dll_dir, const fs::path& dest, std::string& err,
                       std::function<void(const std::string&)> note) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    std::set<std::string> seen;
    std::vector<fs::path> todo{exe};
    while (!todo.empty()) {
        fs::path f = todo.back();
        todo.pop_back();
        for (auto& n : pe_imports(f)) {
            std::string low = n;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (!seen.insert(low).second) continue;
            fs::path src = dll_dir / n;
            if (!is_file(src)) {
                if (is_system_runtime(n)) continue;
                err = "required runtime DLL is missing: " + n + " (searched " + narrow(dll_dir.wstring()) + ")";
                return false;
            }
            if (!copy_dll(src, dest / n, ec)) { err = "could not copy " + n + ": " + ec.message(); return false; }
            note("copied " + n);
            todo.push_back(src);
        }
    }
    fs::path tmp = dest / "BotWRecomp.exe.new";
    fs::copy_file(exe, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "could not copy BotWRecomp.exe: " + ec.message(); return false; }
    if (is_file(dest / "BotWRecomp.exe")) fs::rename(dest / "BotWRecomp.exe", dest / "BotWRecomp.old.exe", ec);
    fs::rename(tmp, dest / "BotWRecomp.exe", ec);
    if (ec) { err = "could not place BotWRecomp.exe: " + ec.message(); return false; }
    note("copied BotWRecomp.exe");
    return true;
}

// --- runner ---
Runner::~Runner() {
    cancel();
    if (th_.joinable()) th_.join();
    if (job_) CloseHandle(job_);
}

void Runner::log(const std::string& s) {
    std::lock_guard<std::mutex> l(mu_);
    log_ += s + "\r\n";
}
std::string Runner::take_log() {
    std::lock_guard<std::mutex> l(mu_);
    std::string s;
    s.swap(log_);
    return s;
}

void Runner::start(const Options& o) {
    if (running_) return;
    if (th_.joinable()) th_.join();
    cancel_ = false;
    running_ = true;
    percent_ = 0;
    { std::lock_guard<std::mutex> l(mu_); err_.clear(); detail_.clear(); }
    th_ = std::thread([this, o] { work(o); });
}

void Runner::cancel() {
    cancel_ = true;
    std::lock_guard<std::mutex> l(mu_);
    if (job_) TerminateJobObject(job_, 1);
}

bool Runner::run(const std::wstring& cmd, const fs::path& cwd, const Options& o) {
    log("> " + narrow(cmd));
    // Environment: UCRT64 first on PATH, never MINGW64.
    std::wstring env;
    std::wstring extra = o.tc.ucrt_bin.wstring() + L";" + (o.tc.msys_root / L"usr" / L"bin").wstring() + L";";
    bool saw = false;
    if (wchar_t* cur = GetEnvironmentStringsW()) {
        for (wchar_t* p = cur; *p;) {
            std::wstring e = p;
            p += e.size() + 1;
            if (_wcsnicmp(e.c_str(), L"PATH=", 5) == 0) { e = L"PATH=" + extra + e.substr(5); saw = true; }
            else if (_wcsnicmp(e.c_str(), L"MSYSTEM=", 8) == 0) continue;
            env += e;
            env.push_back(0);
        }
        FreeEnvironmentStringsW(cur);
    }
    if (!saw) { env += L"PATH=" + extra; env.push_back(0); }
    env += L"MSYSTEM=UCRT64";
    env.push_back(0);
    env += L"PYTHONUNBUFFERED=1";
    env.push_back(0);
    env.push_back(0);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mc(cmd.begin(), cmd.end());
    mc.push_back(0);
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!job_) {
            job_ = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
        }
    }
    BOOL ok = CreateProcessW(nullptr, mc.data(), nullptr, nullptr, TRUE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
                             env.data(), cwd.wstring().c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        log("could not start: error " + std::to_string(GetLastError()));
        return false;
    }
    AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string buf;
    char chunk[4096];
    DWORD n = 0;
    setupwizard::BuildProgress prog;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n > 0) {
        buf.append(chunk, n);
        size_t pos;
        while ((pos = buf.find_first_of("\r\n")) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;
            log(line);
            if (setupwizard::parse_progress_line(line, prog) && prog.matched) {
                if (stage_ == Stage::Recompile || stage_ == Stage::Compile) percent_ = prog.stage_percent;
                std::lock_guard<std::mutex> l(mu_);
                detail_ = prog.sub_total ? std::to_string(prog.sub_done) + " / " + std::to_string(prog.sub_total) : "";
            }
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code != 0 && !cancel_) log("exited with code " + std::to_string(code));
    return code == 0 && !cancel_;
}

void Runner::work(Options o) {
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> l(mu_);
        err_ = e;
    };
    auto finish = [&](Stage s) { stage_ = s; running_ = false; };
    fs::path py = is_file(o.tc.ucrt_bin / L"python3.exe") ? o.tc.ucrt_bin / L"python3.exe" : o.tc.ucrt_bin / L"python.exe";
    fs::path cmake = o.tc.ucrt_bin / L"cmake.exe";
    fs::path bdir = o.source / L"build";

    // 1. Prepare
    stage_ = Stage::Prepare;
    percent_ = 0;
    log("Checking " + narrow(o.rpx.filename().wstring()) + " ...");
    gameinfo::RpxVerify v = gameinfo::verify_rpx(o.rpx);
    if (!v.exists) { fail("U-King.rpx was not found in the game folder."); return finish(Stage::Failed); }
    log(v.summary);
    if (!v.hash_match && !o.dry_run) {
        fail("U-King.rpx does not match the supported v1.5.0 executable. " + v.summary);
        return finish(Stage::Failed);
    }
    percent_ = 100;
    if (cancel_) return finish(Stage::Cancelled);

    // 2. Recompile (skipped when a complete run for this exact RPX is already there)
    stage_ = Stage::Recompile;
    percent_ = 0;
    if (o.dry_run) {
        fs::path tmp = fs::temp_directory_path() / L"botw-native-demo-gen";
        if (!run(q(py) + L" -I " + q(o.source / "tools" / "recompile.py") + L" --demo-progress " + q(tmp), o.source, o)) {
            if (cancel_) return finish(Stage::Cancelled);
            fail("The recompiler stopped with an error. See the log.");
            return finish(Stage::Failed);
        }
    } else if (codegen_complete(o.source, v.sha1)) {
        log("Recompiled code from an earlier run is complete - reusing it.");
    } else {
        std::error_code ec;
        fs::remove(o.source / "generated" / kCodegenMarker, ec);
        const char* probes_env = std::getenv("BOTW_PROBES");
        std::string probes = probes_env ? probes_env : "";
        std::string inputs_before = gameinfo::codegen_fingerprint(o.source, probes);
        if (!run(q(py) + L" -I " + q(o.source / "tools" / "recompile.py") + L" " + q(o.rpx) + L" generated", o.source, o)) {
            if (cancel_) return finish(Stage::Cancelled);
            fail("The recompiler stopped with an error. See the log.");
            return finish(Stage::Failed);
        }
        const char* current_probes = std::getenv("BOTW_PROBES");
        if (!inputs_before.empty() &&
            inputs_before == gameinfo::codegen_fingerprint(o.source, current_probes ? current_probes : "") &&
            v.sha1 == gameinfo::sha1_file(o.rpx) && is_file(o.source / "generated" / "table.cpp")) {
            std::ofstream(o.source / "generated" / kCodegenMarker) << v.sha1 << "\n" << inputs_before << "\n";
        } else {
            log("Generation inputs changed or could not be checked; generated code will not be reused.");
        }
    }
    percent_ = 100;
    if (cancel_) return finish(Stage::Cancelled);

    // 3. Configure
    stage_ = Stage::Configure;
    percent_ = 0;
    if (!run(q(cmake) + L" -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo", o.source, o)) {
        if (cancel_) return finish(Stage::Cancelled);
        fail("CMake could not configure the build. A package may be missing - see the log.");
        return finish(Stage::Failed);
    }
    percent_ = 100;

    // 4. Compile (Ninja keeps finished objects, so a stopped build continues where it left off)
    stage_ = Stage::Compile;
    percent_ = 0;
    std::wstring bcmd = q(cmake) + L" --build build --target BotWRecomp -j " + std::to_wstring(o.jobs);
    if (o.dry_run) bcmd += L" -- -n";
    if (!run(bcmd, o.source, o)) {
        if (cancel_) return finish(Stage::Cancelled);
        fail("Compiling failed. See the log for the first error.");
        return finish(Stage::Failed);
    }
    percent_ = 100;

    // 5. Install
    stage_ = Stage::Install;
    percent_ = 0;
    if (o.dry_run) {
        log("Dry run: nothing was compiled or installed.");
    } else {
        std::string err;
        if (!install_with_dlls(bdir / "BotWRecomp.exe", o.tc.ucrt_bin, o.install_dir, err, [this](const std::string& s) { log(s); })) {
            fail(err);
            return finish(Stage::Failed);
        }
        setupwizard::deploy_runtime_files(o.install_dir, o.source);
    }
    percent_ = 100;
    finish(Stage::Done);
}

}  // namespace gamebuild
