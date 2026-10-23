// controls.ini shared by BotW-Native and BotWLauncher. No SDL and no Win32: the game resolves
// the names at startup, and the launcher writes the same text.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace controlsfile {

// stick: 0 = GamePad button, 1 = left X, 2 = left Y, 3 = right X, 4 = right Y.
// sign: stick direction in player space (+X right, +Y up). 0 for buttons.
struct Action {
    const char* id;
    const char* label;
    int stick;
    int sign;
    const char* default_key;   // SDL_GetScancodeName
    const char* default_key2;  // optional second key, empty if none
    const char* default_pad;   // canonical pad token (see canonical_pad)
};

struct Binding {
    std::string id;
    std::string key;
    std::string key2;
    std::string pad;
};

const Action* actions(int* count);
const Action* find_action(const std::string& id);

std::vector<Binding> defaults();

enum class LoadStatus { Missing, Error, Ok };

// Ok with an empty list means the file was empty or only comments.
LoadStatus load(const std::filesystem::path& path, std::vector<Binding>& out, std::string& err);
bool save(const std::filesystem::path& path, const std::vector<Binding>& rows, std::string& err);

// File missing -> the built-in defaults. File present -> one row per action, and an action
// the file does not mention is unbound (empty key and pad).
std::vector<Binding> editor_rows(bool file_present, const std::vector<Binding>& file_rows);

// Empty token is unbound and returns true with canonical cleared.
// Face buttons are positions: east/south/north/west, not Wii U letters.
bool canonical_pad(const std::string& token, std::string& canonical, std::string& err);
std::string pad_label(const std::string& canonical);
std::string key_label(const std::string& sdl_name);
std::string keys_label(const Binding& b);

}  // namespace controlsfile
