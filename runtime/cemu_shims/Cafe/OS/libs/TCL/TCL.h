// BotW-Native shim: TCL owns the GPU ring buffer in Cemu. Here the ring is a host queue filled by
// latte::submit() (runtime/cemu_shims/latte_glue.cpp) and drained by Cemu's command processor.
#pragma once
namespace TCL {
bool TCLGPUReadRBWord(uint32& cmdWord);
void TCLGPUWaitForWork(int max_us);  // idle GPU thread: sleep until latte::submit() adds words (or timeout)
void TCLGPUNotifyNewRetirementTimestamp();
}  // namespace TCL
