// BotW-Native shim: per-game profile values Cemu ships for BotW (accurate MUL on).
#pragma once
#include "config/CemuConfig.h"

class GameProfile {
   public:
    AccurateShaderMulOption GetAccurateShaderMul() const { return AccurateShaderMulOption::True; }
    PositionInvariance GetPositionInvariance() const { return PositionInvariance::Auto; }
    MetalBufferCacheMode GetBufferCacheMode() const { return MetalBufferCacheMode::Auto; }
    bool GetShaderFastMath() const { return true; }
};
extern GameProfile* g_current_game_profile;
