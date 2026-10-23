// BotW-Native shim: the GPU code only needs a few PPC-side services.
#pragma once
#include "Cafe/HW/Espresso/Const.h"
struct PPCInterpreter_t;
uint64 PPCInterpreter_getMainCoreCycleCounter();
uint32 PPCInterpreter_getCurrentCoreIndex();
