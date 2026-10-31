#include "region.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace fs = std::filesystem;
void log_write(const char*, ...) {}

static void put(const fs::path& path, const char* data) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "botw_region_test";
    fs::remove_all(root);
    const auto eu = root / "europe";
    put(eu / "meta" / "meta.xml", "<menu><title_id>00050000101C9500</title_id></menu>");
    put(eu / "content" / "Pack" / "Bootup_EUde.pack", "de");
    region::init(eu.string(), "de");
    if (region::get().code != region::EUR || region::get().language != 3 ||
        region::get().country != 78 || region::get().title_id != 0x00050000101c9500ull) return 1;
    region::init(eu.string(), "en");
    if (region::get().language != 3) return 2;

    const auto us = root / "usa";
    put(us / "code" / "app.xml", "<app><title_id>0005000E101C9400</title_id></app>");
    put(us / "content" / "Pack" / "Bootup_USen.pack", "en");
    region::init(us.string(), "en");
    if (region::get().code != region::USA || region::get().title_id != 0x00050000101c9400ull) return 3;

    const auto overlay = root / "overlay";
    put(overlay / "code" / "app.xml", "<app><title_id>0005000E101C9400</title_id></app>");
    put(overlay / "content" / "Pack" / "Bootup_EUfr.pack", "fr");
    region::init(overlay.string(), "fr");
    if (region::get().code != region::EUR || region::get().language != 2 ||
        region::get().title_id != 0x00050000101c9500ull) return 4;

    put(us / "code" / "U-King.rpx", "not a game");
    if (!region::check_rpx((us / "code" / "U-King.rpx").string(), true)) return 5;
    fs::remove_all(root);
    std::puts("region tests passed");
}
