// Reads what the launcher shows about a game folder: title and version from meta\meta.xml
// and the icon from meta\iconTex.tga. Plain C++, no Win32, so it can be tested anywhere.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gameinfo {

struct Icon {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // top-down rows, 4 bytes per pixel, straight alpha
    bool empty() const { return width <= 0 || height <= 0 || bgra.empty(); }
};

struct Info {
    bool folder_exists = false;
    bool has_rpx = false;          // code\U-King.rpx
    bool has_content = false;      // content\ directory
    bool has_meta = false;         // meta\meta.xml parsed
    bool has_dlc = false;          // aoc\0010\Pack\AocMainField.pack present
    std::string long_name;         // longname_en, lines joined by '\n'
    std::string title_id;          // upper-case hex, 16 chars when present
    uint32_t title_version = 0;
    bool has_title_version = false;
    Icon icon;
};

// The update this build was made for. v1.5.0 is title version 208.
constexpr uint32_t kExpectedTitleVersion = 208;
constexpr const char* kUsaTitleId = "00050000101C9400";
constexpr const char* kUsaUpdateTitleId = "0005000E101C9400";
constexpr const char* kUsaDlcTitleId = "0005000C101C9400";
constexpr const char* kUsaV208RpxSha1 = "5650b6a81832778fa5c5b31d5a1efb9c48a652b5";
constexpr const char* kUsaV208RpxSha256 = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30";

struct RpxVerify {
    bool exists = false;
    uint64_t size = 0;
    std::string sha1;
    bool hash_match = false;       // true when sha1 matches kUsaV208RpxSha1
    bool meta_usa_v208 = false;    // true when meta.xml is USA and title_version == 208
    std::string region;
    uint32_t title_version = 0;
    bool has_title_version = false;
    std::string summary;           // human-readable verification message
};

struct DumpLayout {
    std::filesystem::path merged_game;    // single folder already containing Base + Update v208
    std::filesystem::path base_folder;    // 00050000... base game dump
    std::filesystem::path update_folder;  // 0005000E... v1.5.0 (v208) update dump
    std::filesystem::path dlc_folder;     // 0005000C... or folder with 0010/Pack/AocMainField.pack
    Info base_info;
    Info update_info;
    bool needs_merge = false;
    std::string validation_error;

    bool ready() const {
        if (!validation_error.empty()) return false;
        if (!merged_game.empty()) return true;
        return !base_folder.empty() && !update_folder.empty();
    }
    std::string summary() const;
};

Info read(const std::filesystem::path& game_folder);

// "v1.5.0" for known BotW title versions, else "v<number>".
std::string version_name(uint32_t title_version);
// "USA", "Europe", "Japan" from the BotW title id, else empty.
std::string region_name(const std::string& title_id);

// SHA-1 helpers (lowercase 40-char hex) for U-King.rpx verification.
std::string sha1_bytes(const void* data, size_t len);
std::string sha1_file(const std::filesystem::path& file_path);
RpxVerify verify_rpx(const std::filesystem::path& game_or_rpx);

// Fingerprint every local input used to generate guest code. Empty means an input could not be read.
std::string codegen_fingerprint(const std::filesystem::path& source, const std::string& probes);
bool codegen_cache_matches(const std::filesystem::path& source, const std::string& rpx_sha1,
                           const std::string& probes);

// Auto-discovers Merged or separate Base + Update (v208) + DLC (v80) folders under `root`.
DumpLayout discover_dumps(const std::filesystem::path& root);
bool validate_dump_pair(const std::filesystem::path& base_dir,
                        const std::filesystem::path& update_dir,
                        const std::filesystem::path& dlc_dir, std::string& err);

// Merges separate Base + Update (+ optional DLC) folders into `target_game_dir` using a staging
// folder, hardlinks on the same volume, and file copies across volumes. The target must not overlap
// any source folder.
using MergeProgressFn = std::function<void(int done, int total, const std::string& label)>;
bool merge_dumps(const std::filesystem::path& base_dir,
                 const std::filesystem::path& update_dir,
                 const std::filesystem::path& dlc_dir,
                 const std::filesystem::path& target_game_dir,
                 std::string& err,
                 MergeProgressFn progress = {});

// Exposed for tests.
bool parse_meta(const std::string& xml, Info& out);
bool decode_tga(const std::vector<uint8_t>& file, Icon& out);

}  // namespace gameinfo
