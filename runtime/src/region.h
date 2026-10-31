// Which release of the game is in the dump (USA, Europe, Japan) and the system settings it sees.
#pragma once
#include <stdint.h>

#include <string>

namespace region {
enum Code : uint32_t { JPN = 1, USA = 2, EUR = 4 };

struct Info {
    uint64_t title_id = 0x00050000101C9400ull;
    uint32_t code = USA;    // MCP game/platform region
    uint32_t language = 1;  // cafe.language
    uint32_t country = 49;  // cafe.cntry_reg
};

// Reads title_id and region from <game>/meta/meta.xml (or code/app.xml). The language comes from
// `language_override` ("en", "de", "ja", ...), else BOTW_LANGUAGE, else the region's default.
void init(const std::string& game_root, const std::string& language_override);
const Info& get();
const char* name(uint32_t code);

// Checks U-King.rpx against the supported v1.5.0 build. Returns false when the game should not start.
bool check_rpx(const std::string& path, bool warn_only);
}  // namespace region
