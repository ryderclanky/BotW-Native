#include "bench.h"

#include "botw_target.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#include <pthread.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../input/input.h"
#include "../log.h"
#include "../os.h"
#include "../thread.h"
#include "../utf8_path.h"

extern std::atomic<pthread_t> g_gpu_thread;
void LatteTiming_setCustomVsyncFrequency(int32_t frequency);

namespace bench {
namespace {
using Clock = std::chrono::steady_clock;

enum class Phase { Off, Boot, Settle, Measure, Done };

Options g_opt{};
std::atomic<Phase> g_phase{Phase::Off};
Clock::time_point g_boot_t0{}, g_phase_t0{};
std::atomic<int> g_draws{0};
int g_gameplay_secs = 0;

std::mutex g_lock;
Clock::time_point g_last_swap{};
std::vector<double> g_frame_ms;

std::atomic<bool> g_prof_run{false};
std::thread g_prof_thread;
std::unordered_map<uint64_t, uint64_t> g_rip_counts;
std::unordered_map<std::string, std::unordered_map<uint64_t, uint64_t>> g_thread_rips;
std::unordered_map<std::string, uint64_t> g_mod_counts;
uint64_t g_prof_ticks = 0;

double elapsed_s(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

#if defined(_WIN32)
void sample_handle(HANDLE h, const std::string& name, uintptr_t base, uintptr_t end) {
    if (!h || GetThreadId(h) == GetCurrentThreadId()) return;
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = GetThreadContext(h, &ctx);
    ResumeThread(h);
    if (!ok || !ctx.Rip) return;
    uint64_t pc = ctx.Rip;
    if (pc >= base && pc < end) {
        uint64_t rva = pc - base;
        g_rip_counts[rva]++;
        g_thread_rips[name][rva]++;
        g_mod_counts["BotWRecomp.exe"]++;
    } else {
        HMODULE mod = nullptr;
        char mname[64] = "other";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)pc, &mod) &&
            mod) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(mod, path, MAX_PATH)) {
                const char* p = path;
                for (const char* q = path; *q; q++)
                    if (*q == '\\' || *q == '/') p = q + 1;
                snprintf(mname, sizeof mname, "%s", p);
            }
        }
        g_mod_counts[mname]++;
    }
}

void profiler_loop() {
    uintptr_t base = botw_image_base();
    uintptr_t end = base;
    if (base) {
        auto* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = (const IMAGE_NT_HEADERS*)((uintptr_t)base + dos->e_lfanew);
            end = base + nt->OptionalHeader.SizeOfImage;
        }
    }
    std::vector<std::pair<void*, std::string>> live;
    uint64_t iter = 0;
    while (g_prof_run.load(std::memory_order_relaxed)) {
        if ((iter++ % 50) == 0) live = threads::snapshot_live_handles();
        pthread_t gpu = g_gpu_thread.load(std::memory_order_relaxed);
        if (gpu) sample_handle((HANDLE)pthread_gethandle(gpu), "LatteThread", base, end);
        for (auto& [h, name] : live) sample_handle((HANDLE)h, name, base, end);
        g_prof_ticks++;
        Sleep(2);
    }
}
#endif

void start_profiler() {
#if defined(_WIN32)
    if (!g_opt.profile || g_prof_run) return;
    g_prof_run = true;
    g_prof_thread = std::thread(profiler_loop);
#endif
}

void stop_profiler() {
#if defined(_WIN32)
    if (!g_prof_run) return;
    g_prof_run = false;
    if (g_prof_thread.joinable()) g_prof_thread.join();
    std::string path = g_user_root + "/bench_profile.txt";
    FILE* f = botw::fopen_utf8(path, "w");
    if (!f) return;
    fprintf(f, "ticks=%llu\n", (unsigned long long)g_prof_ticks);
    std::vector<std::pair<uint64_t, std::string>> mods;
    for (auto& [k, v] : g_mod_counts) mods.push_back({v, k});
    std::sort(mods.rbegin(), mods.rend());
    fprintf(f, "\n[modules]\n");
    for (auto& [v, k] : mods) fprintf(f, "%8llu  %s\n", (unsigned long long)v, k.c_str());

    std::vector<std::pair<uint64_t, uint64_t>> rips;
    for (auto& [rva, c] : g_rip_counts) rips.push_back({c, rva});
    std::sort(rips.rbegin(), rips.rend());
    fprintf(f, "\n[top_rva]\n");
    for (size_t i = 0; i < std::min<size_t>(rips.size(), 400); i++)
        fprintf(f, "%8llu  0x%llx\n", (unsigned long long)rips[i].first, (unsigned long long)rips[i].second);

    for (auto& [tname, map] : g_thread_rips) {
        std::vector<std::pair<uint64_t, uint64_t>> trips;
        uint64_t tot = 0;
        for (auto& [rva, c] : map) {
            trips.push_back({c, rva});
            tot += c;
        }
        std::sort(trips.rbegin(), trips.rend());
        fprintf(f, "\n[thread %s total=%llu]\n", tname.c_str(), (unsigned long long)tot);
        for (size_t i = 0; i < std::min<size_t>(trips.size(), 80); i++)
            fprintf(f, "%8llu  0x%llx\n", (unsigned long long)trips[i].first, (unsigned long long)trips[i].second);
    }
    fclose(f);
    LOG("[bench] profile written to %s (%llu ticks)", path.c_str(), (unsigned long long)g_prof_ticks);
#endif
}

void write_report(bool timed_out) {
    std::vector<double> ms;
    {
        std::lock_guard<std::mutex> l(g_lock);
        ms = g_frame_ms;
    }
    // Drop the first frame (boundary into the window) if we have enough samples.
    if (ms.size() > 4) ms.erase(ms.begin());
    size_t n = ms.size();
    double sum = std::accumulate(ms.begin(), ms.end(), 0.0);
    double avg_ms = n ? sum / n : 0.0;
    double avg_fps = sum > 0.0 ? (n * 1000.0 / sum) : 0.0;

    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());  // ascending frame time (fastest first, slowest last)
    auto pct_ms = [&](double p) -> double {
        if (sorted.empty()) return 0.0;
        size_t idx = std::min(sorted.size() - 1, (size_t)std::floor(p * sorted.size()));
        return sorted[idx];
    };
    double p50_ms = pct_ms(0.50);
    double p99_ms = pct_ms(0.99);
    // 1% low FPS = average FPS of the slowest 1% of frames (standard 1% low definition)
    size_t worst_n = std::max<size_t>(1, n / 100);
    double worst_sum = 0.0;
    for (size_t i = n > worst_n ? n - worst_n : 0; i < n; i++) worst_sum += sorted[i];
    double worst_avg_ms = n ? worst_sum / worst_n : 0.0;
    double low1_fps = worst_avg_ms > 0.0 ? 1000.0 / worst_avg_ms : 0.0;
    double min_ms = sorted.empty() ? 0.0 : sorted.front();
    double max_ms = sorted.empty() ? 0.0 : sorted.back();

    LOG("[bench] %sframes=%llu wall=%.2fs avg_fps=%.2f 1pct_low_fps=%.2f avg_ms=%.3f p50_ms=%.3f p99_ms=%.3f min_ms=%.3f max_ms=%.3f uncap_hz=%d",
        timed_out ? "TIMEOUT " : "", (unsigned long long)n, sum / 1000.0, avg_fps, low1_fps, avg_ms, p50_ms, p99_ms, min_ms, max_ms,
        g_opt.uncap_hz);

    std::string path = g_user_root + "/bench.txt";
    FILE* f = botw::fopen_utf8(path, "w");
    if (!f) return;
    fprintf(f, "status=%s\n", timed_out ? "timeout" : "ok");
    fprintf(f, "seconds=%d\n", g_opt.seconds);
    fprintf(f, "settle=%d\n", g_opt.settle);
    fprintf(f, "uncap_hz=%d\n", g_opt.uncap_hz);
    fprintf(f, "frames=%zu\n", n);
    fprintf(f, "wall_s=%.4f\n", sum / 1000.0);
    fprintf(f, "avg_fps=%.3f\n", avg_fps);
    fprintf(f, "low1_fps=%.3f\n", low1_fps);
    fprintf(f, "avg_ms=%.4f\n", avg_ms);
    fprintf(f, "p50_ms=%.4f\n", p50_ms);
    fprintf(f, "p99_ms=%.4f\n", p99_ms);
    fprintf(f, "worst1pct_ms=%.4f\n", worst_avg_ms);
    fprintf(f, "min_ms=%.4f\n", min_ms);
    fprintf(f, "max_ms=%.4f\n", max_ms);
    fclose(f);
}
}  // namespace

void configure(const Options& o) {
    g_opt = o;
    if (o.seconds > 0) {
        g_phase = Phase::Boot;
        g_boot_t0 = Clock::now();
    }
}

bool active() { return g_phase.load(std::memory_order_relaxed) != Phase::Off; }

void start() {
    if (!active()) return;
    LOG("[bench] configured: measure=%ds settle=%ds timeout=%ds uncap_hz=%d min_draws=%d profile=%d",
        g_opt.seconds, g_opt.settle, g_opt.timeout, g_opt.uncap_hz, g_opt.min_draws, (int)g_opt.profile);
}

void on_swap() {
    if (g_phase.load(std::memory_order_relaxed) != Phase::Measure) return;
    auto now = Clock::now();
    std::lock_guard<std::mutex> l(g_lock);
    if (g_last_swap.time_since_epoch().count() != 0) {
        double ms = std::chrono::duration<double, std::milli>(now - g_last_swap).count();
        threads::diag_on_frame(ms, g_frame_ms.size());
        g_frame_ms.push_back(ms);
    }
    g_last_swap = now;
}

void on_stats(int draws) {
    g_draws.store(draws, std::memory_order_relaxed);
    if (g_phase.load(std::memory_order_relaxed) == Phase::Boot) {
        if (draws >= g_opt.min_draws) g_gameplay_secs++;
        else g_gameplay_secs = 0;
    }
}

uint32_t buttons() {
    if (g_phase.load(std::memory_order_relaxed) != Phase::Boot) return 0;
    // Wait a few seconds for the title menu to come up, then tap A for 150 ms every 800 ms.
    double s = elapsed_s(g_boot_t0);
    if (s < 6.0) return 0;
    uint64_t ms = (uint64_t)(s * 1000.0);
    return (ms % 800) < 150 ? input::BTN_A : 0;
}

bool tick() {
    Phase p = g_phase.load(std::memory_order_relaxed);
    if (p == Phase::Off || p == Phase::Done) return p == Phase::Done;
    if (p == Phase::Boot) {
        if (g_opt.min_draws <= 0 || g_gameplay_secs >= 3) {
            g_phase = Phase::Settle;
            g_phase_t0 = Clock::now();
            LOG("[bench] gameplay reached after %.1fs (%d draws/frame); settling for %ds",
                elapsed_s(g_boot_t0), g_draws.load(), g_opt.settle);
        } else if (elapsed_s(g_boot_t0) > g_opt.timeout) {
            LOG("[bench] timeout (%ds) waiting for gameplay (last draws=%d)", g_opt.timeout, g_draws.load());
            write_report(true);
            g_phase = Phase::Done;
            return true;
        }
    } else if (p == Phase::Settle) {
        if (elapsed_s(g_phase_t0) >= g_opt.settle) {
            {
                std::lock_guard<std::mutex> l(g_lock);
                g_frame_ms.clear();
                g_frame_ms.reserve(g_opt.seconds * 240);
                g_last_swap = Clock::now();
            }
            if (g_opt.uncap_hz > 0) LatteTiming_setCustomVsyncFrequency(g_opt.uncap_hz);
            threads::main_wait_report();  // reset wait counters at the start of the window
            g_phase_t0 = Clock::now();
            g_phase = Phase::Measure;
            start_profiler();
            LOG("[bench] measuring for %ds (uncap_hz=%d)", g_opt.seconds, g_opt.uncap_hz);
        }
    } else if (p == Phase::Measure) {
        if (elapsed_s(g_phase_t0) >= g_opt.seconds) {
            stop_profiler();
            LOG("[bench] main thread waits over window: %s", threads::main_wait_report().c_str());
            threads::diag_dump_threads();
            write_report(false);
            g_phase = Phase::Done;
            return true;
        }
    }
    return false;
}
}  // namespace bench
