// Unit test for the GPU choice (runtime/cemu_shims/gpu_pick.h). Built with BOTW_BUILD_TESTS.
#include <cstdio>
#include <cstring>

#include "gpu_pick.h"

using namespace gpupick;
static int fails = 0;
static void expect(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}
static Gpu mk(const char* name, int type, uint64_t gb, uint8_t id, bool usable = true) {
    Gpu g;
    g.name = name;
    g.type = type;
    g.vram = gb << 30;
    memset(g.uuid, id, 16);
    g.usable = usable;
    return g;
}

int main() {
    const auto igpu = mk("Intel(R) UHD Graphics", kIntegrated, 1, 1);
    const auto dgpu = mk("NVIDIA GeForce RTX 4060 Laptop GPU", kDiscrete, 8, 2);
    const auto amd = mk("AMD Radeon RX 7800 XT", kDiscrete, 16, 3);
    const auto soft = mk("llvmpipe (LLVM 17.0.6, 256 bits)", kCpu, 0, 4);
    std::vector<Gpu> laptop{igpu, dgpu};

    expect(pick({igpu}, "", "", "").index == 0, "single integrated GPU is used");
    expect(pick(laptop, "", "", "").index == 1, "laptop: discrete preferred over integrated listed first");
    expect(pick({dgpu, igpu}, "", "", "").index == 0, "laptop: order does not matter");
    expect(pick({dgpu, amd}, "", "", "").index == 1, "two discrete: most VRAM wins");
    expect(pick({soft, igpu}, "", "", "").index == 1, "software renderer ranked last");
    expect(pick({igpu, mk("dGPU", kDiscrete, 8, 2, false)}, "", "", "").index == 0, "unusable discrete GPU skipped");
    expect(pick({mk("x", kDiscrete, 1, 1, false)}, "", "", "").index == -1, "no usable GPU gives -1");
    expect(pick({}, "", "", "").index == -1, "no GPUs gives -1");

    expect(pick(laptop, uuid_hex(igpu.uuid), igpu.name, "").index == 0, "saved UUID honoured");
    expect(pick(laptop, "AABBCC", igpu.name, "").index == 0, "saved UUID gone, name still matches");
    auto c = pick(laptop, "00ff", "Old GPU", "");
    expect(c.index == 1 && c.reason.find("not found") != std::string::npos, "saved GPU missing falls back to best");

    expect(pick(laptop, "", "", "0").index == 0, "BOTW_GPU index");
    expect(pick(laptop, uuid_hex(igpu.uuid), "", "nvidia").index == 1, "BOTW_GPU name beats saved GPU");
    expect(pick(laptop, "", "", "7").index == 1, "BOTW_GPU bad index falls back");
    expect(pick(laptop, "", "", "radeon").index == 1, "BOTW_GPU no match falls back");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
