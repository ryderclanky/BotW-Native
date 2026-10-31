// BotWRecomp shim: the running title.
#pragma once
#include <stdint.h>

#include <atomic>
#include <string>

extern uint64_t g_foreground_title_id;

namespace CafeSystem {
inline uint64_t GetForegroundTitleId() { return g_foreground_title_id; }  // from the dump (region.cpp)
inline bool IsTitleRunning() { return true; }
std::string GetMlcStoragePath(uint64_t titleId);
}  // namespace CafeSystem
extern std::atomic_bool g_isGPUInitFinished;
