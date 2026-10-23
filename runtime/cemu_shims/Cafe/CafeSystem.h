// BotW-Native shim: the running title.
#pragma once
#include <stdint.h>

#include <atomic>
#include <string>

namespace CafeSystem {
inline uint64_t GetForegroundTitleId() { return 0x00050000101C9400ull; }  // BotW (USA)
inline bool IsTitleRunning() { return true; }
std::string GetMlcStoragePath(uint64_t titleId);
}  // namespace CafeSystem
extern std::atomic_bool g_isGPUInitFinished;
