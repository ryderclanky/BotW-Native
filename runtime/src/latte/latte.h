// The emulated Latte GPU: a host thread that executes PM4 command buffers from guest memory.
// GX2 (runtime/src/gx2) writes the command buffers; this side keeps the register file and turns
// draws into Metal work (renderer.h). GPU addresses are guest virtual addresses.
#pragma once
#include <stdint.h>

#include <functional>

namespace latte {
constexpr uint32_t kRegCount = 0x10000;

// PM4 type-3 opcodes (Cemu LattePM4.h) and this runtime's HLE packets. Skipped when Cemu's
// LattePM4.h (which #defines the same names) is already included.
#ifndef IT_SET_PREDICATION
enum : uint32_t {
    IT_SET_PREDICATION = 0x20, IT_DRAW_INDEX_2 = 0x27, IT_CONTEXT_CONTROL = 0x28, IT_INDEX_TYPE = 0x2A,
    IT_DRAW_INDEX_AUTO = 0x2D, IT_DRAW_INDEX_IMMD = 0x2E, IT_NUM_INSTANCES = 0x2F, IT_INDIRECT_BUFFER_PRIV = 0x32,
    IT_STRMOUT_BUFFER_UPDATE = 0x34, IT_MEM_SEMAPHORE = 0x39, IT_WAIT_REG_MEM = 0x3C, IT_MEM_WRITE = 0x3D,
    IT_SURFACE_SYNC = 0x43, IT_EVENT_WRITE = 0x46, IT_EVENT_WRITE_EOP = 0x47, IT_LOAD_CONFIG_REG = 0x60,
    IT_LOAD_CONTEXT_REG = 0x61, IT_LOAD_ALU_CONST = 0x62, IT_LOAD_BOOL_CONST = 0x63, IT_LOAD_LOOP_CONST = 0x64,
    IT_LOAD_RESOURCE = 0x65, IT_LOAD_SAMPLER = 0x66, IT_SET_CONFIG_REG = 0x68, IT_SET_CONTEXT_REG = 0x69,
    IT_SET_ALU_CONST = 0x6A, IT_SET_BOOL_CONST = 0x6B, IT_SET_LOOP_CONST = 0x6C, IT_SET_RESOURCE = 0x6D,
    IT_SET_SAMPLER = 0x6E, IT_SET_CTL_CONST = 0x6F, IT_STRMOUT_BASE_UPDATE = 0x72, IT_SET_ALL_CONTEXTS = 0x74,
    IT_HLE_COPY_SURFACE_NEW = 0xEE, IT_HLE_SYNC_ASYNC_OPERATIONS = 0xEF, IT_HLE_REQUEST_SWAP_BUFFERS = 0xF0,
    IT_HLE_WAIT_FOR_FLIP = 0xF1, IT_HLE_BOTTOM_OF_PIPE_CB = 0xF2, IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER = 0xF3,
    IT_HLE_CLEAR_COLOR_DEPTH_STENCIL = 0xF5, IT_HLE_SAMPLE_TIMER = 0xF7, IT_HLE_TRIGGER_SCANBUFFER_SWAP = 0xF8,
    IT_HLE_SPECIAL_STATE = 0xF9, IT_HLE_BEGIN_OCCLUSION_QUERY = 0xFA, IT_HLE_END_OCCLUSION_QUERY = 0xFB,
};
#endif
inline uint32_t pm4_type3(uint32_t it, uint32_t ndwords) { return 0xC0000000u | (it << 8) | ((ndwords - 1) << 16); }

// Which native handle `create_renderer` receives. Cocoa's surface is an NSView/UIView.
// X11's display is Display* and its surface is the Window id. Wayland's display is wl_display*
// and its surface is wl_surface*. Windows' surface is an HWND.
enum class HostSurface { Cocoa, X11, Wayland, Windows };

// Select the graphics backend before window::create. "vulkan" or "metal"; nullptr keeps the
// default (Metal when this build has it, otherwise Vulkan).
void select_backend(const char* name);
bool using_vulkan();

// Called before create_renderer so the Vulkan backend knows which surface extension to use.
// `surface` is the same pointer then passed to create_renderer.
void set_host_surface(HostSurface kind, void* display, void* surface);

// Main thread, before the game starts: create the renderer on the native window.
void create_renderer(void* native_view, int width, int height);
// The window changed size. `width`/`height` are points on Metal and pixels on Vulkan.
void resize_window(int width, int height);
// After the device and swapchain exist: bring the renderer up and present one empty frame.
// Used when no game is loaded, so a machine without a dump can still prove the backend.
bool present_empty_frame();
// GX2SetTVBuffer / GX2SetDRCBuffer: whether the scan-out buffer is sRGB (output gamma-encodes)
void set_scanout_srgb(bool tv, bool srgb);
// How the image fills the window: 0 keep aspect (letterbox), 1 stretch, 2 fill and crop
void set_display_mode(int mode);
// Output scaling filter: 0 bilinear, 1 bicubic, 2 hermite, 3 nearest
void set_output_filter(int filter);
// Show the GamePad screen next to the TV image (Vulkan backend only; ignored elsewhere)
void set_gamepad_view(bool on);
void init();
// --capture N: save the TV image every N frames as PNG into dir
void set_capture(int every, const char* dir);
// Queue a command buffer (guest address, length in dwords). Returns its timestamp.
uint64_t submit(uint32_t addr, uint32_t ndwords);
uint64_t last_submitted();
uint64_t retired();
void wait_retired(uint64_t ts);

// swap / vsync bookkeeping used by GX2
uint32_t flip_requests();
uint32_t flips_executed();
void add_flip_request();
void wait_for_flip();
void wait_for_vsync();
uint64_t vsync_count();
void wait_vsync_count(uint64_t target);  // until the vblank counter reaches target (200 ms max)
uint64_t last_flip_time();
uint64_t last_vsync_time();
uint32_t swap_interval();
void set_swap_interval(uint32_t);

// GX2 event callbacks (GX2SetEventCallback): the GPU raises, GX2 dispatches on a guest thread
enum class Event : uint32_t { TIMESTAMP_TOP = 0, TIMESTAMP_BOTTOM = 1, VSYNC = 2, FLIP = 3, DL_OVERRUN = 4 };
void set_event_sink(std::function<void(Event)> sink);

// register file (GPU thread)
extern uint32_t g_regs[kRegCount];
}  // namespace latte
