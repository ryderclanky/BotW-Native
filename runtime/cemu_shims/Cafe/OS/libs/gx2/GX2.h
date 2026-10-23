// BotW-Native shim: what the vendored GPU code takes from Cemu's GX2.h. The GX2 library itself
// lives in runtime/src/gx2 (own environment).
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "GX2_Surface.h"
#define GX2_SPECIAL_STATE_COUNT 9
#define GX2_INVALID_COUNTER_VALUE_U64 0xFFFFFFFFFFFFFFFFull
namespace GX2 {
extern uint32 sGX2MainCoreIndex;
}
