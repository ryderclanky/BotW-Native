#include "mods.h"

#include "coreinit/mods.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace modman {
namespace {

namespace fs = std::filesystem;

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim(std::string s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

std::string read_text(const fs::path& p, size_t cap) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.size() > cap) s.resize(cap);
    return s;
}

void write_text(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

fs::path utf8_path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string utf8_name(const fs::path& p) {
    auto u = p.filename().u8string();
    return std::string(u.begin(), u.end());
}

std::string one_line(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return trim(s);
}

std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
}

bool is_reserved(std::string s) {
    s = lower_ascii(s);
    auto dot = s.find('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s == "con" || s == "prn" || s == "aux" || s == "nul" || s == "com1" || s == "com2" || s == "com3" || s == "com4" ||
           s == "lpt1" || s == "lpt2" || s == "lpt3";
}

std::string folder_id(const std::string& name) {
    std::string o;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' ||
            c == '(' || c == ')' || c == '.')
            o.push_back(static_cast<char>(c));
        else if (c >= 32)
            o.push_back('_');
    }
    o = trim(o);
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    while (!o.empty() && o.front() == '.') o.erase(o.begin());
    if (o.empty()) o = "mod";
    if (o.size() > 80) o.resize(80);
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    if (is_reserved(o)) o += "_mod";
    return o;
}

fs::path mods_root(const fs::path& user) { return user / "mods"; }

fs::path mod_dir(const fs::path& user, const std::string& folder) { return mods_root(user) / utf8_path(folder); }

bool direct_child(const fs::path& root, const fs::path& child) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(root, ec);
    if (ec) a = fs::absolute(root, ec);
    fs::path b = fs::weakly_canonical(child, ec);
    if (ec) b = fs::absolute(child, ec);
    if (b.parent_path() != a) return false;
    return true;
}

struct ModIni {
    std::string name;
    std::string warning;
};

ModIni read_mod_ini(const fs::path& dir, const std::string& folder) {
    ModIni m;
    m.name = folder;
    std::istringstream in(read_text(dir / "mod.ini", 16 * 1024));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower_ascii(trim(line.substr(0, eq)));
        std::string val = one_line(line.substr(eq + 1));
        if (key == "name" && !val.empty()) m.name = val;
        else if (key == "warning") m.warning = val;
    }
    return m;
}

void write_mod_ini(const fs::path& dir, const std::string& name, const std::string& warning) {
    std::string body = "name=" + one_line(name) + "\n";
    if (!warning.empty()) body += "warning=" + one_line(warning) + "\n";
    write_text(dir / "mod.ini", body);
}

std::vector<mods::IniLine> read_lines(const fs::path& user) {
    return mods::parse_ini(read_text(mods_root(user) / "mods.ini", 256 * 1024));
}

bool write_lines(const fs::path& user, const std::vector<mods::IniLine>& lines, std::string& err) {
    std::error_code ec;
    fs::create_directories(mods_root(user), ec);
    if (ec) {
        err = "Could not create the mods folder.";
        return false;
    }
    fs::path ini = mods_root(user) / "mods.ini";
    std::ofstream out(ini, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "Could not write mods.ini.";
        return false;
    }
    out << mods::format_ini(lines);
    if (!out) {
        err = "Could not write mods.ini.";
        return false;
    }
    return true;
}

std::vector<std::string> extra_folders(const fs::path& user, const std::vector<mods::IniLine>& lines) {
    std::vector<std::string> extra;
    std::error_code ec;
    fs::path root = mods_root(user);
    if (!fs::is_directory(root, ec)) return extra;
    for (fs::directory_iterator it(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec) || it->is_symlink(sec)) continue;
        std::string name = utf8_name(it->path());
        if (mods::parse_ini("+" + name).empty()) continue;
        bool known = false;
        for (const auto& line : lines)
            if (line.folder == name) known = true;
        if (!known) extra.push_back(name);
    }
    std::sort(extra.begin(), extra.end());
    return extra;
}

// parse_ini("+" + name) is non-empty when name is a legal folder. The double negative above is
// hard to read; this helper is what the loop means.
bool legal_folder(const std::string& name) { return !mods::parse_ini("+" + name).empty(); }

std::string warning_text(const Found& f) {
    std::string w;
    auto add = [&](const char* sentence) {
        if (!w.empty()) w += " ";
        w += sentence;
    };
    if (f.switch_mod) add("This mod is for the Switch version.");
    if (f.patches) add("This mod needs code patches, which this version can't run.");
    if (f.shaders) add("This mod replaces shaders, which this version can't run.");
    if (f.textures) add("This mod changes textures through Cemu rules, which this version can't run.");
    if (f.has_files && !f.switch_mod && (f.patches || f.shaders || f.textures)) add("Its replacement files are still used.");
    return w;
}

bool name_is(const fs::path& p, const char* lower) { return lower_ascii(utf8_name(p)) == lower; }

bool switch_dir_name(const std::string& lower) { return lower == "exefs" || lower == "atmosphere" || lower == "romfs"; }

void scan_rules(const fs::path& rules, Found& f) {
    std::string text = read_text(rules, 256 * 1024);
    if (text.empty() && !fs::is_regular_file(rules)) return;
    f.rules = rules;
    std::string low = lower_ascii(text);
    if (low.find("[shader") != std::string::npos || low.find("vertexshader") != std::string::npos ||
        low.find("pixelshader") != std::string::npos)
        f.shaders = true;
    if (low.find("[textureredefine") != std::string::npos) f.textures = true;
    bool wiiu = low.find("00050000101c9") != std::string::npos || low.find("0005000e101c9") != std::string::npos ||
                low.find("0005000c101c9") != std::string::npos;
    bool sw = low.find("01007ef00011e") != std::string::npos;
    if (sw && !wiiu) f.switch_mod = true;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower_ascii(trim(line.substr(0, eq)));
        if (key != "name") continue;
        std::string val = one_line(unquote(line.substr(eq + 1)));
        if (!val.empty()) f.display_name = val;
        break;
    }
}

void scan_patches(const fs::path& root, Found& f) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    int seen = 0;
    for (; !ec && it != end && seen < 4000; it.increment(ec), seen++) {
        if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code sec;
        if (it->is_symlink(sec)) {
            it.disable_recursion_pending();
            continue;
        }
        std::string name = lower_ascii(utf8_name(it->path()));
        if (it->is_directory(sec)) {
            if (name == "content" || name == "aoc") it.disable_recursion_pending();
            if (switch_dir_name(name)) f.switch_mod = true;
            continue;
        }
        if (!it->is_regular_file(sec)) continue;
        auto ext = lower_ascii(it->path().extension().string());
        if (ext == ".asm" || name.rfind("patch_", 0) == 0) f.patches = true;
        if (ext == ".glsl" || ext == ".hlsl" || ext == ".spv") f.shaders = true;
    }
}

struct Candidate {
    fs::path dir;
    int depth = 0;
    int score = 0;
};

int score_dir(const fs::path& dir) {
    std::error_code ec;
    int score = 0;
    if (fs::is_directory(dir / "content", ec)) score += 2;
    if (fs::is_directory(dir / "aoc", ec)) score += 2;
    if (fs::is_regular_file(dir / "rules.txt", ec)) score += 1;
    return score;
}

Found inspect_dir(const fs::path& input) {
    Found f;
    std::error_code ec;
    if (!fs::is_directory(input, ec)) return f;
    if (name_is(input, "content")) {
        f.recognized = true;
        f.has_files = true;
        f.content = input;
        f.display_name = "content";
        return f;
    }
    if (name_is(input, "aoc")) {
        f.recognized = true;
        f.has_files = true;
        f.aoc = input;
        f.display_name = "aoc";
        return f;
    }

    Candidate best;
    std::vector<std::pair<fs::path, int>> q;
    q.push_back({input, 0});
    for (size_t i = 0; i < q.size(); i++) {
        fs::path dir = q[i].first;
        int depth = q[i].second;
        int score = score_dir(dir);
        if (score > best.score || (score == best.score && score > 0 && depth < best.depth)) best = {dir, depth, score};
        if (depth >= 2) continue;
        for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (!it->is_directory(sec) || it->is_symlink(sec)) continue;
            std::string name = lower_ascii(utf8_name(it->path()));
            if (switch_dir_name(name)) f.switch_mod = true;
            q.push_back({it->path(), depth + 1});
        }
    }
    if (best.score > 0) {
        f.recognized = true;
        std::error_code sec;
        if (fs::is_directory(best.dir / "content", sec)) f.content = best.dir / "content";
        if (fs::is_directory(best.dir / "aoc", sec)) f.aoc = best.dir / "aoc";
        f.has_files = !f.content.empty() || !f.aoc.empty();
        if (fs::is_regular_file(best.dir / "rules.txt", sec)) scan_rules(best.dir / "rules.txt", f);
        scan_patches(best.dir, f);
        if (f.display_name.empty()) f.display_name = utf8_name(best.dir);
    } else if (f.switch_mod) {
        f.recognized = true;
        f.display_name = utf8_name(input);
        scan_patches(input, f);
    }
    if (f.recognized) f.warning = warning_text(f);
    return f;
}

bool copy_tree(const fs::path& src, const fs::path& dst, uint64_t& bytes, int& files, std::string& err) {
    std::error_code ec;
    if (!fs::is_directory(src, ec)) return true;
    fs::create_directories(dst, ec);
    if (ec) {
        err = "Could not create the mod folder.";
        return false;
    }
    fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_symlink(sec)) {
            it.disable_recursion_pending();
            continue;
        }
        fs::path rel = it->path().lexically_relative(src);
        if (rel.empty() || rel.generic_string().rfind("..", 0) == 0) continue;
        fs::path target = dst / rel;
        if (it->is_directory(sec)) {
            fs::create_directories(target, ec);
            continue;
        }
        if (!it->is_regular_file(sec)) continue;
        auto sz = it->file_size(sec);
        if (sec) continue;
        bytes += static_cast<uint64_t>(sz);
        files++;
        if (files > 100000 || bytes > (8ull << 30)) {
            err = "That mod is too large to add.";
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "Could not copy " + rel.generic_string();
            return false;
        }
    }
    if (ec && files == 0 && bytes == 0) {
        err = "Could not read the mod folder.";
        return false;
    }
    return true;
}

}  // namespace

Found inspect(const fs::path& input) { return inspect_dir(input); }

std::vector<Row> list_mods(const fs::path& user_root) {
    auto lines = read_lines(user_root);
    for (const auto& extra : extra_folders(user_root, lines)) lines.push_back({extra, false});
    std::vector<Row> rows;
    for (const auto& line : lines) {
        if (!legal_folder(line.folder)) continue;
        fs::path dir = mod_dir(user_root, line.folder);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        ModIni ini = read_mod_ini(dir, line.folder);
        rows.push_back({line.folder, ini.name, ini.warning, line.enabled});
    }
    return rows;
}

Result install_tree(const fs::path& user_root, const fs::path& root, const std::string& source_label) {
    Result r;
    if (user_root.empty()) {
        r.message = "Set the user data folder first.";
        return r;
    }
    Found found = inspect_dir(root);
    if (!found.recognized) {
        r.message = "That is not a mod. It needs a content folder, an aoc folder, or a rules.txt.";
        return r;
    }
    std::string display = found.display_name.empty() ? source_label : found.display_name;
    if (display.empty()) display = "mod";
    std::string id = folder_id(source_label.empty() ? display : source_label);
    if (id == "content" || id == "aoc" || id == "mod") {
        std::string from_rules = folder_id(display);
        if (from_rules != "content" && from_rules != "aoc" && from_rules != "mod") id = from_rules;
    }
    std::error_code ec;
    std::string unique = id;
    for (int n = 2; fs::exists(mod_dir(user_root, unique), ec); n++) unique = id + "-" + std::to_string(n);
    fs::path dest = mod_dir(user_root, unique);
    fs::create_directories(dest, ec);
    if (ec) {
        r.message = "Could not create the mod folder.";
        return r;
    }
    uint64_t bytes = 0;
    int files = 0;
    std::string err;
    bool copied = true;
    if (!found.content.empty()) copied = copy_tree(found.content, dest / "content", bytes, files, err);
    if (copied && !found.aoc.empty()) copied = copy_tree(found.aoc, dest / "aoc", bytes, files, err);
    if (copied && !found.rules.empty()) {
        fs::copy_file(found.rules, dest / "rules.txt", fs::copy_options::overwrite_existing, ec);
        if (ec) {
            copied = false;
            err = "Could not copy rules.txt.";
        }
    }
    if (!copied) {
        fs::remove_all(dest, ec);
        r.message = err.empty() ? "Could not copy the mod." : err;
        return r;
    }
    bool enable = found.has_files && !found.switch_mod;
    write_mod_ini(dest, display, found.warning);
    auto lines = read_lines(user_root);
    lines.push_back({unique, enable});
    if (!write_lines(user_root, lines, err)) {
        fs::remove_all(dest, ec);
        r.message = err;
        return r;
    }
    r.ok = true;
    r.enabled = enable;
    r.warning = found.warning;
    if (!enable && !found.warning.empty()) r.message = "Added \"" + display + "\" turned off. " + found.warning;
    else if (!enable) r.message = "Added \"" + display + "\" turned off. It has no replacement files.";
    else if (!found.warning.empty()) r.message = "Added \"" + display + "\". It is on. " + found.warning;
    else r.message = std::string("Added \"") + display + "\". It is on.";
    return r;
}

Result remove_mod(const fs::path& user_root, const std::string& folder) {
    Result r;
    if (!legal_folder(folder)) {
        r.message = "That mod name cannot be removed.";
        return r;
    }
    fs::path root = mods_root(user_root);
    fs::path dir = mod_dir(user_root, folder);
    std::error_code ec;
    if (fs::exists(dir, ec) && !direct_child(root, dir)) {
        r.message = "That mod is not inside the mods folder.";
        return r;
    }
    fs::remove_all(dir, ec);
    if (ec) {
        r.message = "Could not delete the mod folder.";
        return r;
    }
    auto lines = read_lines(user_root);
    std::vector<mods::IniLine> kept;
    for (const auto& line : lines)
        if (line.folder != folder) kept.push_back(line);
    std::string err;
    if (!write_lines(user_root, kept, err)) {
        r.message = err;
        return r;
    }
    r.ok = true;
    r.message = "Removed the mod. The game dump was not changed.";
    return r;
}

Result set_enabled(const fs::path& user_root, const std::string& folder, bool on) {
    Result r;
    if (!legal_folder(folder)) {
        r.message = "That mod name is not valid.";
        return r;
    }
    auto lines = read_lines(user_root);
    bool found = false;
    for (auto& line : lines) {
        if (line.folder == folder) {
            line.enabled = on;
            found = true;
        }
    }
    if (!found) lines.push_back({folder, on});
    std::string err;
    if (!write_lines(user_root, lines, err)) {
        r.message = err;
        return r;
    }
    r.ok = true;
    r.enabled = on;
    r.message = on ? "Mod on." : "Mod off.";
    return r;
}

Result disable_all(const fs::path& user_root) {
    Result r;
    auto rows = list_mods(user_root);
    std::vector<mods::IniLine> lines;
    for (const auto& row : rows) lines.push_back({row.folder, false});
    std::string err;
    if (!write_lines(user_root, lines, err)) {
        r.message = err;
        return r;
    }
    r.ok = true;
    r.message = "All mods are off. The game will run vanilla.";
    return r;
}

}  // namespace modman
