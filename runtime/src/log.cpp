#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <mutex>

#include "utf8_path.h"

int g_log_level = 0;
static FILE* g_log_file = nullptr;
static std::recursive_mutex g_log_lock;  // recursive: the watchdog's stack-dump signal handler logs

void log_set_file(const char* path) { g_log_file = botw::fopen_utf8(path ? path : "", "w"); }

void log_write(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static double t0 = ts.tv_sec + ts.tv_nsec * 1e-9;
    double t = ts.tv_sec + ts.tv_nsec * 1e-9 - t0;
    std::lock_guard<std::recursive_mutex> l(g_log_lock);
    fprintf(stderr, "[%8.3f] %s\n", t, buf);
    if (g_log_file) {
        fprintf(g_log_file, "[%8.3f] %s\n", t, buf);
        fflush(g_log_file);
    }
}
