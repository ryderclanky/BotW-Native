// Find a Wii U / Cemu BotW save, judge whether this 1.5.0 runtime can use it, and swap it
// into <user root>/save/80000001 without touching that folder while the caller still has to
// refuse the operation if BotWRecomp.exe is running.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace saveimport {

enum class Endian { Unknown, WiiU, Switch };
enum class Age { Unknown, Older, Exact150, Newer };

struct SavCheck {
    std::filesystem::path file;
    Endian endian = Endian::Unknown;
    Age age = Age::Unknown;
    uint32_t version = 0;
    std::string version_name;
    bool header_ok = false;
};

struct Located {
    std::filesystem::path account_dir;
    std::filesystem::path common_dir;  // empty if the dump has no user/common
    std::string account_name;          // "80000001" when the folder is a persistent id
    std::string title_id;              // "101c9400" when the dump has one, else empty
    int slots = 0;
    bool has_option = false;
    bool dangerous = false;  // Switch, newer than 1.5.0, mixed, unreadable, or not BotW
    bool caution = false;    // older than 1.5.0, or no slot folders
    std::string warnings;
    std::string note;  // does not by itself require a warning, for example an account rename
    std::vector<SavCheck> files;
};

struct SetInfo {
    std::filesystem::path dir;
    std::string title;
    std::string kind;  // "backup" or "import"
    std::string created;
    std::string warnings;
    std::string source;
    int slots = 0;
};

struct OpResult {
    bool ok = false;
    std::string message;
    std::filesystem::path backup;
    std::filesystem::path imported;
};

// input may be a directory or a file inside one (option.sav, a slot's game_data.sav).
// Zips are extracted by the caller first.
bool locate(const std::filesystem::path& input, Located& out, std::string& err);

std::filesystem::path live_account_dir(const std::filesystem::path& user_root);
std::filesystem::path sets_dir(const std::filesystem::path& user_root);

// Copies the save into save-sets, backs up the live account, then installs it as 80000001.
OpResult import_account(const std::filesystem::path& user_root, const Located& src, const std::string& source_label);
OpResult switch_to(const std::filesystem::path& user_root, const std::filesystem::path& set_dir);
OpResult delete_set(const std::filesystem::path& user_root, const std::filesystem::path& set_dir);
std::vector<SetInfo> list_sets(const std::filesystem::path& user_root);

// Older builds wrote slot 1 with std::to_string, so the folder was save/2147483649.
// decimal_legacy_folder is that folder. decimal_legacy_present is the same when 80000001
// does not already look like a save.
bool decimal_legacy_folder(const std::filesystem::path& user_root);
bool decimal_legacy_present(const std::filesystem::path& user_root);
// Fills warnings for the decimal folder. False when it is not a save.
bool peek_decimal(const std::filesystem::path& user_root, Located& out);
// Copies save/2147483649 onto save/80000001. Backs up a non-empty live folder first.
// The decimal folder is left in place.
OpResult migrate_decimal(const std::filesystem::path& user_root);

}  // namespace saveimport
