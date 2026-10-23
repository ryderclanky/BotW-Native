// BotW-Native shim: guest callbacks from vendored code (not used by the GPU path).
#pragma once
uint32 cemu_core_callback(uint32 fn, std::initializer_list<uint32> args);
