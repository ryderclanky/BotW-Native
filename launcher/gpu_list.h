// Lists the Vulkan GPUs for the launcher. vulkan-1.dll is loaded at runtime, so the launcher
// still opens on a PC without Vulkan (the list is then empty).
#pragma once
#include <vector>

#include "../runtime/cemu_shims/gpu_pick.h"

namespace gpulist {
std::vector<gpupick::Gpu> enumerate();
}
