#include "input/controls_file.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace controlsfile {
namespace {

const Action kActions[] = {
    {"a", "A", 0, 0, "E", "", "auto"},
    {"b", "B", 0, 0, "Left Shift", "Right Shift", "auto"},
    {"x", "X", 0, 0, "Space", "", "auto"},
    {"y", "Y", 0, 0, "F", "", "auto"},
    {"l", "L", 0, 0, "1", "", "leftshoulder"},
    {"r", "R", 0, 0, "3", "", "rightshoulder"},
    {"zl", "ZL", 0, 0, "Q", "", "lefttrigger"},
    {"zr", "ZR", 0, 0, "R", "", "righttrigger"},
    {"plus", "Plus (+)", 0, 0, "Return", "", "start"},
    {"minus", "Minus (-)", 0, 0, "Tab", "", "back"},
    {"up", "D-pad up", 0, 0, "Up", "", "dpup"},
    {"down", "D-pad down", 0, 0, "Down", "", "dpdown"},
    {"left", "D-pad left", 0, 0, "Left", "", "dpleft"},
    {"right", "D-pad right", 0, 0, "Right", "", "dpright"},
    {"stickl", "Left stick click", 0, 0, "C", "", "leftstick"},
    {"stickr", "Right stick click", 0, 0, "V", "", "rightstick"},
    {"lstick_left", "Left stick left", 1, -1, "A", "", "leftx-"},
    {"lstick_right", "Left stick right", 1, +1, "D", "", "leftx+"},
    {"lstick_up", "Left stick up", 2, +1, "W", "", "lefty+"},
    {"lstick_down", "Left stick down", 2, -1, "S", "", "lefty-"},
    {"rstick_left", "Right stick left", 3, -1, "J", "", "rightx-"},
    {"rstick_right", "Right stick right", 3, +1, "L", "", "rightx+"},
    {"rstick_up", "Right stick up", 4, +1, "I", "", "righty+"},
    {"rstick_down", "Right stick down", 4, -1, "K", "", "righty-"},
    {"home", "Home", 0, 0, "H", "Home", "guide"},
    {"sync", "Sync", 0, 0, "G", "", "misc2"},
    {"tv", "TV", 0, 0, "T", "", "misc1"},
};

std::string path_text(const std::filesystem::path& path) {
    auto u = path.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string trim(std::string s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

const char* kPads[] = {
    "auto", "south", "east", "west", "north", "back", "start", "guide", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "misc2", "misc3", "misc4", "misc5", "misc6",
    "touchpad", "paddle1", "paddle2", "paddle3", "paddle4", "lefttrigger", "righttrigger", "leftx-", "leftx+",
    "lefty-", "lefty+", "rightx-", "rightx+", "righty-", "righty+",
};

const char* alias_pad(const std::string& t) {
    if (t == "dpad_up" || t == "dup") return "dpup";
    if (t == "dpad_down" || t == "ddown") return "dpdown";
    if (t == "dpad_left" || t == "dleft") return "dpleft";
    if (t == "dpad_right" || t == "dright") return "dpright";
    if (t == "left_stick" || t == "l3" || t == "stick_l") return "leftstick";
    if (t == "right_stick" || t == "r3" || t == "stick_r") return "rightstick";
    if (t == "lshoulder" || t == "lb" || t == "l1") return "leftshoulder";
    if (t == "rshoulder" || t == "rb" || t == "r1") return "rightshoulder";
    if (t == "lt" || t == "l2" || t == "zl") return "lefttrigger";
    if (t == "rt" || t == "r2" || t == "zr") return "righttrigger";
    if (t == "share" || t == "capture") return "misc1";
    if (t == "plus") return "start";
    if (t == "minus") return "back";
    return nullptr;
}

}  // namespace

const Action* actions(int* count) {
    if (count) *count = static_cast<int>(sizeof kActions / sizeof kActions[0]);
    return kActions;
}

const Action* find_action(const std::string& id) {
    for (const Action& a : kActions)
        if (id == a.id) return &a;
    return nullptr;
}

std::vector<Binding> defaults() {
    std::vector<Binding> rows;
    rows.reserve(sizeof kActions / sizeof kActions[0]);
    for (const Action& a : kActions) rows.push_back({a.id, a.default_key, a.default_key2, a.default_pad});
    return rows;
}

bool canonical_pad(const std::string& token, std::string& canonical, std::string& err) {
    std::string t = lower(trim(token));
    canonical.clear();
    if (t.empty() || t == "none" || t == "-" || t == "unbound") return true;
    if (const char* aliased = alias_pad(t)) t = aliased;
    for (const char* p : kPads) {
        if (t == p) {
            canonical = p;
            return true;
        }
    }
    err = "unknown controller binding \"" + token + "\"";
    return false;
}

std::string pad_label(const std::string& canonical) {
    std::string t = lower(trim(canonical));
    if (t.empty()) return "(none)";
    if (t == "auto") return "Auto (this controller)";
    if (t == "east") return "Right face (East, Xbox B)";
    if (t == "south") return "Bottom face (South, Xbox A)";
    if (t == "north") return "Top face (North, Xbox Y)";
    if (t == "west") return "Left face (West, Xbox X)";
    if (t == "leftshoulder") return "Left shoulder";
    if (t == "rightshoulder") return "Right shoulder";
    if (t == "lefttrigger") return "Left trigger";
    if (t == "righttrigger") return "Right trigger";
    if (t == "start") return "Start";
    if (t == "back") return "Back";
    if (t == "guide") return "Guide";
    if (t == "leftstick") return "Left stick click";
    if (t == "rightstick") return "Right stick click";
    if (t == "dpup") return "D-pad up";
    if (t == "dpdown") return "D-pad down";
    if (t == "dpleft") return "D-pad left";
    if (t == "dpright") return "D-pad right";
    if (t == "misc1") return "Share / Capture";
    if (t == "misc2") return "Extra button";
    if (t == "touchpad") return "Touchpad";
    if (t == "leftx-") return "Left stick left";
    if (t == "leftx+") return "Left stick right";
    if (t == "lefty+") return "Left stick up";
    if (t == "lefty-") return "Left stick down";
    if (t == "rightx-") return "Right stick left";
    if (t == "rightx+") return "Right stick right";
    if (t == "righty+") return "Right stick up";
    if (t == "righty-") return "Right stick down";
    return canonical;
}

std::string key_label(const std::string& sdl_name) {
    if (sdl_name == "Return") return "Enter";
    if (sdl_name.empty()) return "(none)";
    return sdl_name;
}

std::string keys_label(const Binding& b) {
    if (b.key.empty() && b.key2.empty()) return "(none)";
    if (b.key.empty()) return key_label(b.key2);
    if (b.key2.empty()) return key_label(b.key);
    return key_label(b.key) + " or " + key_label(b.key2);
}

std::vector<Binding> editor_rows(bool file_present, const std::vector<Binding>& file_rows) {
    std::vector<Binding> rows = defaults();
    if (!file_present) return rows;
    for (Binding& row : rows) {
        const Binding* found = nullptr;
        for (const Binding& f : file_rows)
            if (f.id == row.id) found = &f;
        if (found) row = *found;
        else {
            row.key.clear();
            row.key2.clear();
            row.pad.clear();
        }
    }
    return rows;
}

LoadStatus load(const std::filesystem::path& path, std::vector<Binding>& out, std::string& err) {
    out.clear();
    std::ifstream in(path);
    if (!in) {
        err = "missing";
        return LoadStatus::Missing;
    }
    std::string line;
    int lineno = 0;
    bool first = true;
    while (std::getline(in, line)) {
        lineno++;
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            err = path_text(path) + ":" + std::to_string(lineno) + ": expected id=key:...,pad:...";
            return LoadStatus::Error;
        }
        Binding b;
        b.id = lower(trim(line.substr(0, eq)));
        if (!find_action(b.id)) {
            err = path_text(path) + ":" + std::to_string(lineno) + ": unknown action \"" + b.id + "\"";
            return LoadStatus::Error;
        }
        std::string rest = line.substr(eq + 1);
        std::stringstream ss(rest);
        std::string part;
        while (std::getline(ss, part, ',')) {
            part = trim(part);
            if (part.empty()) continue;
            auto colon = part.find(':');
            if (colon == std::string::npos) {
                err = path_text(path) + ":" + std::to_string(lineno) + ": expected name:value";
                return LoadStatus::Error;
            }
            std::string name = lower(trim(part.substr(0, colon)));
            std::string value = trim(part.substr(colon + 1));
            if (name == "key") b.key = value;
            else if (name == "key2") b.key2 = value;
            else if (name == "pad") {
                std::string canon, perr;
                if (!canonical_pad(value, canon, perr)) {
                    err = path_text(path) + ":" + std::to_string(lineno) + ": " + perr;
                    return LoadStatus::Error;
                }
                b.pad = canon;
            }
        }
        bool replaced = false;
        for (Binding& existing : out) {
            if (existing.id == b.id) {
                existing = b;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.push_back(std::move(b));
    }
    if (in.bad()) {
        err = "could not read " + path_text(path);
        return LoadStatus::Error;
    }
    return LoadStatus::Ok;
}

bool save(const std::filesystem::path& path, const std::vector<Binding>& rows, std::string& err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "could not write " + path_text(path);
        return false;
    }
    out << "# BotW-Native controls. One action per line. Delete this file to use the built-in map.\n"
           "# key and key2 are SDL scancode names (SDL_GetScancodeName), for example \"Left Shift\" or Return.\n"
           "# pad is a controller source. Face buttons are positions, not the Wii U letters:\n"
           "#   east = right face (Xbox B / Circle), south = bottom (Xbox A / Cross),\n"
           "#   north = top (Xbox Y / Triangle), west = left (Xbox X / Square).\n"
           "# Sticks use player directions: leftx- leftx+ lefty+ (up) lefty- (down), and the same for right*.\n"
           "# Triggers: lefttrigger, righttrigger. Empty key or pad unbinds that side.\n"
           "# The in-game \"A = bottom / A = right\" option does not change a file that is present.\n";
    for (const Binding& b : rows) {
        if (!find_action(b.id)) continue;
        std::string pad, perr;
        if (!canonical_pad(b.pad, pad, perr)) {
            err = b.id + ": " + perr;
            return false;
        }
        out << b.id << "=key:" << b.key;
        if (!b.key2.empty()) out << ",key2:" << b.key2;
        out << ",pad:" << pad << "\n";
    }
    if (!out) {
        err = "could not write " + path_text(path);
        return false;
    }
    return true;
}

}  // namespace controlsfile
