// Mod overlay and DLC visibility. The game dump is never written. Enabled mods are listed in
// <user>/mods/mods.ini (+ on, - off, later lines win). Each mod folder may contain content/
// and aoc/. Lookup is an in-memory index built at startup.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mods {

struct IniLine {
    std::string folder;
    bool enabled = false;
};

std::vector<IniLine> parse_ini(std::string_view text);
std::string format_ini(const std::vector<IniLine>& lines);

struct Query {
    enum class Kind { Other, Missing, File, Dir };
    Kind kind = Kind::Other;
    std::string host;
    bool overlay = false;
    uint64_t size = 0;
};

struct DirItem {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    bool overlay = false;
};

enum class ListStatus { NotVolume, Missing, NotDir, Ok };

void init(const std::string& game_root, const std::string& user_root);
void log_shutdown();

// `guest` is an absolute Cafe path (/vol/content/..., /vol/aoc...). Other means not content or aoc.
Query query(const std::string& guest);
ListStatus list_dir(const std::string& guest, std::vector<DirItem>& out);
// True for /vol/content, /vol/code, /vol/meta, and /vol/aoc. Those volumes are never created or
// overwritten on the host.
bool read_only_volume(const std::string& guest);

void note_opened(const std::string& guest);

bool dlc_ready();
uint64_t aoc_title_id();
std::string aoc_mount_path();
bool owns_content(uint16_t id);

size_t index_files();
size_t replacing_files();
size_t added_files();
size_t opened_files();

}  // namespace mods
