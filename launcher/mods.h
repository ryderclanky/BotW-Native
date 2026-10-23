// Find a Wii U BotW mod (UKMM/BCML content tree or a Cemu graphic pack) and install it under
// <user>/mods. The runtime reads mods.ini. This does not touch the game dump.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace modman {

struct Found {
    bool recognized = false;
    std::string display_name;
    std::string warning;
    bool has_files = false;  // content/ or aoc/ replacement files
    bool patches = false;
    bool shaders = false;
    bool textures = false;
    bool switch_mod = false;
    std::filesystem::path content;  // empty if none; directory to copy as content/
    std::filesystem::path aoc;
    std::filesystem::path rules;  // rules.txt file, or empty
};

struct Row {
    std::string folder;
    std::string name;
    std::string warning;
    bool enabled = false;
};

struct Result {
    bool ok = false;
    bool enabled = false;
    std::string message;
    std::string warning;
};

Found inspect(const std::filesystem::path& input);

std::vector<Row> list_mods(const std::filesystem::path& user_root);

// `source_label` is the zip or folder name the person dropped, without the extension.
Result install_tree(const std::filesystem::path& user_root, const std::filesystem::path& root, const std::string& source_label);
Result remove_mod(const std::filesystem::path& user_root, const std::string& folder);
Result set_enabled(const std::filesystem::path& user_root, const std::string& folder, bool on);
Result disable_all(const std::filesystem::path& user_root);

}  // namespace modman
