// BotW-Native shim: time helpers used by the GPU code.
#pragma once
#include "Cafe/HW/Espresso/Const.h"
namespace coreinit {
uint64 OSGetSystemTime();
uint64 OSGetTime();
namespace EspressoTime {
typedef sint64 TimerTicks;
constexpr sint64 GetCoreClock() { return Espresso::CORE_CLOCK; }
constexpr sint64 GetBusClock() { return Espresso::BUS_CLOCK; }
constexpr sint64 GetTimerClock() { return Espresso::TIMER_CLOCK; }
inline TimerTicks ConvertNsToTimerTicks(uint64 ns) {
    return static_cast<TimerTicks>((static_cast<uint64>(GetTimerClock()) / 31250ULL) * (ns) / 32000ULL);
}
inline TimerTicks ConvertMsToTimerTicks(uint64 ms) { return static_cast<TimerTicks>(ms * static_cast<uint64>(GetTimerClock()) / 1000ULL); }
}  // namespace EspressoTime
}  // namespace coreinit
