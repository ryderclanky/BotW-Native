// BotW-Native shim: Cemu's ActiveSettings (paths and debug switches) for the vendored GPU code.
#pragma once
#include <filesystem>
#include <fmt/format.h>

namespace fs = std::filesystem;
fs::path botw_cache_root();  // ~/Library/Application Support/BotWRecomp/cache
fs::path botw_user_root();

class ActiveSettings {
   public:
    template <typename... A>
    static fs::path GetCachePath(fmt::format_string<A...> f, A&&... a) {
        return botw_cache_root() / fmt::format(f, std::forward<A>(a)...);
    }
    static fs::path GetCachePath() { return botw_cache_root(); }
    template <typename... A>
    static fs::path GetUserDataPath(fmt::format_string<A...> f, A&&... a) {
        return botw_user_root() / fmt::format(f, std::forward<A>(a)...);
    }
    static bool DumpShadersEnabled();
    static bool DumpTexturesEnabled() { return false; }
    static bool FlushGPUCacheOnSwap() { return false; }
    static bool RenderUpsideDownEnabled() { return false; }
    static float GetTVGamma() { return 2.2f; }
    static float GetDRCGamma() { return 2.2f; }
    static uint32_t GetPersistentId() { return 0x80000001; }
    static bool ShaderPreventInfiniteLoopsEnabled() { return false; }
    static bool ForceSamplerRoundToPrecision() { return false; }
    static bool WaitForGX2DrawDoneEnabled() { return true; }
};
#include "config/CemuConfig.h"
