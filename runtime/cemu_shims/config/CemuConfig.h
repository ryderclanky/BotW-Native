// BotW-Native shim: the subset of Cemu's configuration the vendored GPU code reads, fixed defaults.
#pragma once
#include <stdint.h>
#include <array>
#include <string>

template <typename T>
struct ConfigValue {
    T v;
    ConfigValue(T x = T{}) : v(x) {}
    const T& GetValue() const { return v; }
    operator const T&() const { return v; }
};
enum class PrecompiledShaderOption { Auto, Enable, Disable };
enum class AccurateShaderMulOption { False = 0, True = 1 };
enum class MetalBufferCacheMode { Auto, DevicePrivate, DeviceShared, Host };
enum class PositionInvariance { Auto, False, True };
enum class ScreenPosition { kDisabled = 0, kTopLeft, kTopCenter, kTopRight, kBottomLeft, kBottomCenter, kBottomRight };
enum FullscreenScaling { kKeepAspectRatio = 0, kStretch, kFillCrop };  // kFillCrop: BotW-Native (cover, crop overflow)
enum UpscalingFilter { kLinearFilter = 0, kBicubicFilter, kBicubicHermiteFilter, kNearestNeighborFilter };
enum class GraphicAPI { kOpenGL = 0, kVulkan, kMetal };

struct CemuConfig {
    bool async_compile = true;
    bool vk_accurate_barriers = false;
    bool play_boot_sound = false;
    float userDisplayGamma = 2.2f;
    int upscale_filter = kBicubicFilter;
    int downscale_filter = kLinearFilter;
    int fullscreen_scaling = kKeepAspectRatio;
    ConfigValue<bool> framebuffer_fetch{true};
    ConfigValue<bool> force_mesh_shaders{false};
    ConfigValue<std::string> gpu_capture_dir{std::string()};
    struct {
        ScreenPosition position = ScreenPosition::kDisabled;
    } overlay;
    GraphicAPI graphic_api = GraphicAPI::kMetal;
    std::array<uint8_t, 16> vk_graphic_device_uuid{};
    ConfigValue<int> vsync{0};  // 0 immediate, 1 fifo, 2 mailbox, 3 sync-and-limit
    uint64_t mtl_graphic_device_uuid = 0;
};
CemuConfig& GetConfig();
