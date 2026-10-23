// BOTW_DIAG=1 counters: where draws land, whether the command processor dropped any,
// whether AX voices reach the mixer, and whether VPADRead keeps being called.
// title_diag_draw / title_diag_log and the other counters are no-ops unless BOTW_DIAG
// is set. title_diag_event() is the exception: the first few unique skip/format events
// are written even when the variable is unset, so a cutscene log still has the lines.
//
// GPU functions are called on the GPU thread only. AX and VPAD functions are called
// from their own threads; title_diag_log() (GPU thread, about every 10 seconds) prints
// and resets the interval.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Passed to title_diag_event. Each kind keeps its own small set of unique lines.
enum {
    TITLE_DIAG_SCISSOR = 1,       // color target smaller than the scissor box
    TITLE_DIAG_COPY_RATIO = 2,    // texture/surface copy dropped, scale ratios differ
    TITLE_DIAG_COPY_BPP = 3,      // format-conversion copy dropped, bpp differs
    TITLE_DIAG_PIPE_FAIL = 4,     // vkCreateGraphicsPipelines failed
    TITLE_DIAG_SKIP_SHADER = 5,   // draw sequence skipped, shader failed to compile
    TITLE_DIAG_SKIP_RT = 6,       // draw sequence skipped, rendertarget invalid
    TITLE_DIAG_SKIP_ATTACH = 7,   // draw sequence skipped, no color or depth attachment
    TITLE_DIAG_SKIP_RAST = 8,     // draw sequence skipped, rasterizer kill
    TITLE_DIAG_SKIP_PIPE = 9,     // draw skipped, graphics pipeline handle is null
    TITLE_DIAG_TEX_DIM = 10,      // shader dim disagreed with the bound texture; null texture bound
    TITLE_DIAG_RES_CLEAR = 11,    // graphic-pack resolution overwrite cleared a slice instead of uploading
    TITLE_DIAG_RES_CLEAR_FONT = 12, // same, for R8 / R4G4 / BC1-BC5
    TITLE_DIAG_BLEND_BOTH = 13,   // blend factor 0x0B or 0x0C emulated
    TITLE_DIAG_BLEND_BAD = 14,    // blend factor outside the GX2 table
    TITLE_DIAG_DEPTH_OFF = 15,    // depth/stencil forced off because the pass has no depth attachment
    TITLE_DIAG_RECT = 16,         // RECT primitive pipeline compiled
    TITLE_DIAG_VIEWPORT = 17      // viewport min/max depth outside [0,1]
};

// color_base / color_size are CB_COLOR0_BASE and CB_COLOR0_SIZE at the draw.
void title_diag_draw(uint32_t color_base, uint32_t color_size);
// prim is VGT_PRIMITIVE_TYPE. depth_on is Z_ENABLE. blend_mask is CB blend enable.
// count is the guest index/vertex count for this draw.
void title_diag_draw_state(uint32_t prim, int depth_on, uint32_t blend_mask, uint32_t count);
// why: 1 = indexed draw with a null index buffer (not counted as a draw),
//      2 = auto draw skipped as a compute shader.
void title_diag_draw_skip(int why);
// Unique, rate-limited line for a skipped or unsupported GPU op. Always logs the
// first matches; interval totals are printed only when BOTW_DIAG=1.
void title_diag_event(int kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f);
// GX2SwapScanBuffers. Closes the "draws after the scanout copy" window.
void title_diag_swap(void);
// One indirect buffer, including the per-submit command buffer and display lists.
void title_diag_indirect(uint32_t dwords);
void title_diag_scanout(uint32_t addr, uint32_t width, uint32_t height, uint32_t format);
void title_diag_clear(uint32_t mask, uint32_t width, uint32_t height);

// playing: voices whose playback state is running.
// linked_ppc / linked_dsp: voices actually chained into that mixer list.
// mask_tv: linked PPC voices with a non-zero TV device mix mask.
// peak_mix: max abs sample in the 32 kHz mix buffer (before the 32->48 step).
// peak_lr / peak_other: max abs sample in the 48 kHz buffer sent to the host,
// stereo channels 0-1 vs the other four.
// flags bit 0: TV final-mix callback set. bit 1: frame callback. bit 2: an app frame callback.
void title_diag_ax(uint32_t playing, uint32_t linked_ppc, uint32_t linked_dsp, uint32_t mask_tv,
                   uint32_t peak_mix, uint32_t peak_lr, uint32_t peak_other, uint32_t flags);

void title_diag_vpad_read(uint32_t samples, uint32_t buttons);
void title_diag_vpad_callback(uint32_t addr);
void title_diag_vpad_invoked(void);

void title_diag_log(void);

#ifdef __cplusplus
}
#endif
