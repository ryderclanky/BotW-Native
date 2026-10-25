#include "gpu_list.h"

#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstring>

namespace gpulist {

std::vector<gpupick::Gpu> enumerate() {
    std::vector<gpupick::Gpu> out;
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) return out;
    auto gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
    auto create = gipa ? (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance") : nullptr;
    if (!create) { FreeLibrary(lib); return out; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "BotWLauncher";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (create(&ci, nullptr, &inst) != VK_SUCCESS) { FreeLibrary(lib); return out; }
    auto destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    auto enumdev = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    auto props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    auto props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    auto memp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    uint32_t n = 0;
    if (enumdev && props && memp && enumdev(inst, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> devs(n);
        enumdev(inst, &n, devs.data());
        devs.resize(n);
        for (auto d : devs) {
            gpupick::Gpu g;
            VkPhysicalDeviceProperties p{};
            if (props2) {
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                p2.pNext = &id;
                props2(d, &p2);
                p = p2.properties;
                memcpy(g.uuid, id.deviceUUID, VK_UUID_SIZE);
            } else {
                props(d, &p);
            }
            g.name = p.deviceName;
            g.type = (int)p.deviceType;
            VkPhysicalDeviceMemoryProperties m{};
            memp(d, &m);
            for (uint32_t h = 0; h < m.memoryHeapCount; h++)
                if (m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) g.vram += m.memoryHeaps[h].size;
            out.push_back(g);
        }
    }
    if (destroy) destroy(inst, nullptr);
    FreeLibrary(lib);
    return out;
}

}  // namespace gpulist
