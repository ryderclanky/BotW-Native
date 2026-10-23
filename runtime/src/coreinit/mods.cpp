#include "mods.h"

#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace mods {
namespace {

namespace fs = std::filesystem;

struct Node {
    std::string name;
    std::string host;
    bool file = false;
    std::map<std::string, Node> kids;
};

std::mutex g_case_mu;
std::mutex g_open_mu;
Node g_content;
Node g_aoc;
std::string g_content_base;
std::string g_aoc_base;
uint64_t g_title = 0x0005000C101C9400ull;
bool g_title_from_meta = false;
bool g_pack = false;
bool g_inited = false;
std::unordered_set<uint16_t> g_owned;
std::unordered_set<std::string> g_opened;
std::map<std::string, std::map<std::string, std::string>> g_case_cache;
size_t g_files = 0;
size_t g_replacing = 0;
size_t g_added = 0;

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

bool valid_folder(const std::string& s) {
    if (s.empty() || s == "." || s == "..") return false;
    if (s.front() == ' ' || s.back() == ' ' || s.back() == '.') return false;
    for (unsigned char c : s) {
        if (c < 32) return false;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

fs::path utf8_path(std::string_view s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string read_text(const fs::path& p, size_t cap) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return {};
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.size() > cap) s.resize(cap);
    return s;
}

std::vector<std::string> split_rel(std::string_view rel) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&] {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return true;
        }
        if (cur == "..") return false;
        parts.push_back(cur);
        cur.clear();
        return true;
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            if (!flush()) return {};
        } else {
            cur.push_back(c);
        }
    }
    if (!flush()) return {};
    return parts;
}

struct GuestVol {
    enum class Id { Other, Content, Aoc, Code, Meta } id = Id::Other;
    std::string rel;
};

GuestVol parse_guest(std::string guest) {
    for (char& c : guest)
        if (c == '\\') c = '/';
    std::string cur;
    std::vector<std::string> parts;
    auto flush = [&] {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return;
        }
        if (cur == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (char c : guest) {
        if (c == '/') flush();
        else cur.push_back(c);
    }
    flush();
    GuestVol v;
    if (parts.size() < 2 || parts[0] != "vol") return v;
    auto join_from = [&](size_t i) {
        std::string r;
        for (; i < parts.size(); i++) {
            if (!r.empty()) r += '/';
            r += parts[i];
        }
        return r;
    };
    if (parts[1] == "content") {
        v.id = GuestVol::Id::Content;
        v.rel = join_from(2);
        return v;
    }
    if (parts[1] == "code") {
        v.id = GuestVol::Id::Code;
        v.rel = join_from(2);
        return v;
    }
    if (parts[1] == "meta") {
        v.id = GuestVol::Id::Meta;
        v.rel = join_from(2);
        return v;
    }
    if (parts[1].compare(0, 3, "aoc") == 0) {
        size_t skip = 2;
        const std::string& name = parts[1];
        if (name.size() == 3 + 16) {
            bool hex = true;
            for (size_t i = 3; i < name.size(); i++) {
                unsigned char c = static_cast<unsigned char>(name[i]);
                if (!std::isxdigit(c)) hex = false;
            }
            if (!hex) return v;
        } else if (name.size() != 3) {
            return v;
        }
        v.id = GuestVol::Id::Aoc;
        v.rel = join_from(skip);
        return v;
    }
    return v;
}

const Node* walk(const Node& root, const std::string& rel, bool& file, bool& occluded) {
    file = false;
    occluded = false;
    if (rel.empty()) return root.kids.empty() ? nullptr : &root;
    const Node* n = &root;
    auto parts = split_rel(rel);
    if (parts.empty() && !rel.empty()) {
        occluded = true;
        return nullptr;
    }
    for (size_t i = 0; i < parts.size(); i++) {
        auto it = n->kids.find(lower_ascii(parts[i]));
        if (it == n->kids.end()) return nullptr;
        n = &it->second;
        if (n->file && i + 1 < parts.size()) {
            occluded = true;
            return nullptr;
        }
    }
    file = n->file;
    return n;
}

std::string lookup_component(const std::string& dir, const std::string& want_lower) {
    std::lock_guard<std::mutex> lock(g_case_mu);
    auto& cached = g_case_cache[dir];
    if (cached.empty()) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (it->is_symlink(sec)) continue;
            std::string name = it->path().filename().string();
            cached.emplace(lower_ascii(name), name);
        }
        cached.emplace("", "");  // mark the directory as cached even when it is empty
    }
    auto it = cached.find(want_lower);
    if (it == cached.end()) return {};
    return it->second;
}

std::string case_path(const std::string& base, const std::string& rel) {
    if (base.empty()) return {};
    fs::path exact(base);
    auto parts = split_rel(rel);
    for (const auto& part : parts) exact /= part;
    std::error_code ec;
    auto st = fs::status(exact, ec);
    if (!ec && (fs::is_regular_file(st) || fs::is_directory(st))) return exact.string();
    if (rel.empty()) return {};
    std::string cur = base;
    for (const auto& part : parts) {
        fs::path next = fs::path(cur) / part;
        st = fs::status(next, ec);
        if (!ec && (fs::is_regular_file(st) || fs::is_directory(st))) {
            cur = next.string();
            continue;
        }
        std::string actual = lookup_component(cur, lower_ascii(part));
        if (actual.empty()) return {};
        cur = (fs::path(cur) / actual).string();
    }
    return cur;
}

bool is_file(const std::string& host, uint64_t& size) {
    if (host.empty()) return false;
    std::error_code ec;
    auto st = fs::status(fs::path(host), ec);
    if (ec || !fs::is_regular_file(st)) return false;
    auto n = fs::file_size(fs::path(host), ec);
    if (ec) return false;
    size = static_cast<uint64_t>(n);
    return true;
}

bool is_dir(const std::string& host) {
    if (host.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::path(host), ec);
}

void insert_file(Node& root, const std::string& rel, const std::string& host) {
    auto parts = split_rel(rel);
    if (parts.empty()) return;
    Node* n = &root;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        std::string key = lower_ascii(parts[i]);
        Node& child = n->kids[key];
        if (child.name.empty()) child.name = parts[i];
        child.file = false;
        child.host.clear();
        n = &child;
    }
    std::string key = lower_ascii(parts.back());
    Node& leaf = n->kids[key];
    leaf.name = parts.back();
    leaf.host = host;
    leaf.file = true;
    leaf.kids.clear();
    if (lower_ascii(rel) == "0010/pack/aocmainfield.pack") g_pack = true;
}

size_t add_tree(Node& root, const fs::path& host_root) {
    std::error_code ec;
    if (host_root.empty() || !fs::is_directory(host_root, ec)) return 0;
    size_t nfiles = 0;
    fs::recursive_directory_iterator it(host_root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_symlink(sec)) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(sec)) continue;
        fs::path rel = it->path().lexically_relative(host_root);
        std::string generic = rel.generic_string();
        if (generic.empty() || generic.rfind("..", 0) == 0) continue;
        insert_file(root, generic, it->path().string());
        nfiles++;
    }
    return nfiles;
}

fs::path aoc_content_root(const fs::path& folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return {};
    bool content = fs::is_directory(folder / "content", ec);
    bool direct = fs::is_directory(folder / "0010", ec);
    if (content && direct) {
        if (fs::is_directory(folder / "content" / "0010", ec)) return folder / "content";
        return folder;
    }
    if (content) return folder / "content";
    return folder;
}

void tally(const Node& node, const std::string& base, const std::string& prefix) {
    for (const auto& [key, child] : node.kids) {
        std::string rel = prefix.empty() ? key : prefix + "/" + key;
        if (child.file) {
            g_files++;
            uint64_t sz = 0;
            if (is_file(case_path(base, rel), sz)) g_replacing++;
            else g_added++;
        } else {
            tally(child, base, rel);
        }
    }
}

void collect_owned(const Node& root) {
    for (const auto& [key, child] : root.kids) {
        if (child.file || key.size() != 4) continue;
        bool hex = true;
        for (char c : key)
            if (!std::isxdigit(static_cast<unsigned char>(c))) hex = false;
        if (!hex) continue;
        g_owned.insert(static_cast<uint16_t>(std::strtoul(key.c_str(), nullptr, 16)));
    }
}

void collect_owned_base(const std::string& base) {
    if (base.empty()) return;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(base), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec) || it->is_symlink(sec)) continue;
        std::string name = lower_ascii(it->path().filename().string());
        if (name.size() != 4) continue;
        bool hex = true;
        for (char c : name)
            if (!std::isxdigit(static_cast<unsigned char>(c))) hex = false;
        if (!hex) continue;
        g_owned.insert(static_cast<uint16_t>(std::strtoul(name.c_str(), nullptr, 16)));
    }
}

uint64_t parse_title(const std::string& game) {
    if (game.empty()) return 0;
    fs::path root(game);
    std::string text = read_text(root / "meta" / "meta.xml", 64 * 1024);
    if (text.empty()) text = read_text(root / "code" / "app.xml", 64 * 1024);
    std::string low = lower_ascii(text);
    auto at = low.find("title_id");
    if (at == std::string::npos) return 0;
    std::string hex;
    for (size_t i = at; i < low.size(); i++) {
        unsigned char c = static_cast<unsigned char>(low[i]);
        if (std::isxdigit(c)) {
            hex.push_back(static_cast<char>(c));
            if (hex.size() == 16) break;
        } else if (!hex.empty()) {
            hex.clear();
        }
    }
    if (hex.size() != 16) return 0;
    return std::strtoull(hex.c_str(), nullptr, 16);
}

struct ModMeta {
    std::string name;
    std::string warning;
};

ModMeta read_mod_ini(const fs::path& dir, const std::string& folder) {
    ModMeta m;
    m.name = folder;
    std::string text = read_text(dir / "mod.ini", 16 * 1024);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower_ascii(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));
        for (char& c : val)
            if (c == '\r' || c == '\n') c = ' ';
        if (key == "name" && !val.empty()) m.name = val;
        else if (key == "warning") m.warning = val;
    }
    return m;
}

std::string base_for(GuestVol::Id id) {
    if (id == GuestVol::Id::Content) return g_content_base;
    if (id == GuestVol::Id::Aoc) return g_aoc_base;
    return {};
}

const Node& tree_for(GuestVol::Id id) { return id == GuestVol::Id::Aoc ? g_aoc : g_content; }

void reset() {
    g_content = Node{};
    g_aoc = Node{};
    g_content_base.clear();
    g_aoc_base.clear();
    g_title = 0x0005000C101C9400ull;
    g_title_from_meta = false;
    g_pack = false;
    g_owned.clear();
    g_opened.clear();
    g_case_cache.clear();
    g_files = g_replacing = g_added = 0;
}

}  // namespace

std::vector<IniLine> parse_ini(std::string_view text) {
    std::vector<IniLine> lines;
    std::string cur;
    auto flush = [&] {
        std::string line = trim(cur);
        cur.clear();
        if (line.empty() || line[0] == '#') return;
        bool on = true;
        if (line[0] == '+') {
            on = true;
            line = trim(line.substr(1));
        } else if (line[0] == '-') {
            on = false;
            line = trim(line.substr(1));
        }
        if (!valid_folder(line)) return;
        lines.push_back({line, on});
    };
    for (char c : text) {
        if (c == '\n') flush();
        else cur.push_back(c);
    }
    flush();
    return lines;
}

std::string format_ini(const std::vector<IniLine>& lines) {
    std::string o = "# BotW Recomp mods. + is on, - is off. Later lines win when files overlap.\n";
    for (const auto& line : lines) {
        o.push_back(line.enabled ? '+' : '-');
        o += line.folder;
        o.push_back('\n');
    }
    return o;
}

bool base_nonempty(const std::string& base) {
    if (!is_dir(base)) return false;
    std::error_code ec;
    fs::directory_iterator it(fs::path(base), ec);
    return !ec && it != fs::directory_iterator();
}

void init(const std::string& game_root, const std::string& user_root) {
    reset();
    g_inited = true;
    if (!game_root.empty()) {
        fs::path game(game_root);
        g_content_base = (game / "content").string();
        fs::path aoc = aoc_content_root(game / "aoc");
        if (!aoc.empty()) g_aoc_base = aoc.string();
        uint64_t base = parse_title(game_root);
        if (base != 0) {
            g_title = (base & 0xFFFFFFFFULL) | 0x0005000C00000000ULL;
            g_title_from_meta = true;
        }
    }
    fs::path user = user_root.empty() ? fs::path() : fs::path(user_root);
    fs::path user_aoc = user.empty() ? fs::path() : aoc_content_root(user / "aoc");
    size_t user_aoc_files = 0;
    if (!user_aoc.empty()) user_aoc_files = add_tree(g_aoc, user_aoc);

    fs::path mods_dir = user.empty() ? fs::path() : user / "mods";
    std::string ini = read_text(mods_dir / "mods.ini", 256 * 1024);
    auto lines = parse_ini(ini);
    if (lines.empty() && ini.empty()) LOG("mods: none (no mods.ini)");
    for (const auto& line : lines) {
        fs::path dir = mods_dir / utf8_path(line.folder);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            LOG("mod missing: %s", line.folder.c_str());
            continue;
        }
        ModMeta meta = read_mod_ini(dir, line.folder);
        if (!line.enabled) {
            LOG("mod off: %s", meta.name.c_str());
            continue;
        }
        size_t n = add_tree(g_content, dir / "content");
        n += add_tree(g_aoc, aoc_content_root(dir / "aoc"));
        const char* word = n == 1 ? "file" : "files";
        if (!meta.warning.empty()) LOG("mod on: %s (%zu %s) - %s", meta.name.c_str(), n, word, meta.warning.c_str());
        else LOG("mod on: %s (%zu %s)", meta.name.c_str(), n, word);
    }
    if (user_aoc_files)
        LOG("mods: user DLC folder %s (%zu %s, below mods)", user_aoc.string().c_str(), user_aoc_files,
            user_aoc_files == 1 ? "file" : "files");

    uint64_t pack_sz = 0;
    if (is_file(case_path(g_aoc_base, "0010/Pack/AocMainField.pack"), pack_sz)) g_pack = true;
    tally(g_content, g_content_base, "");
    tally(g_aoc, g_aoc_base, "");
    collect_owned(g_aoc);
    collect_owned_base(g_aoc_base);
    if (g_pack) g_owned.insert(0x0010);

    LOG("mods: %zu overlay files, %zu replace a base file, %zu new", g_files, g_replacing, g_added);
    char mount[40];
    snprintf(mount, sizeof mount, "/vol/aoc%016llx", static_cast<unsigned long long>(g_title));
    if (g_pack) {
        LOG("DLC: installed as %s (AocMainField.pack found%s)", mount, g_title_from_meta ? "" : ", title id assumed USA");
    } else if (g_aoc.kids.empty() && !base_nonempty(g_aoc_base)) {
        LOG("DLC: not installed");
    } else {
        LOG("DLC: not installed. Add-on files were found, but AocMainField.pack was not, so the game is told the DLC is missing");
    }
}

void log_shutdown() {
    if (!g_inited) return;
    static bool once = false;
    if (once) return;
    once = true;
    size_t opened = 0;
    {
        std::lock_guard<std::mutex> lock(g_open_mu);
        opened = g_opened.size();
    }
    LOG("mods: %zu base files overridden, %zu new files, %zu distinct overlay files opened", g_replacing, g_added, opened);
}

Query query(const std::string& guest) {
    Query q;
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return q;
    bool file = false, occluded = false;
    const Node* node = walk(tree_for(vol.id), vol.rel, file, occluded);
    if (occluded) {
        q.kind = Query::Kind::Missing;
        return q;
    }
    if (node && file) {
        uint64_t sz = 0;
        if (is_file(node->host, sz)) {
            q.kind = Query::Kind::File;
            q.host = node->host;
            q.overlay = true;
            q.size = sz;
            return q;
        }
    }
    if (node && !file) {
        q.kind = Query::Kind::Dir;
        q.overlay = true;
        q.host = case_path(base_for(vol.id), vol.rel);
        if (!is_dir(q.host)) q.host.clear();
        return q;
    }
    std::string host = case_path(base_for(vol.id), vol.rel);
    uint64_t sz = 0;
    if (is_file(host, sz)) {
        q.kind = Query::Kind::File;
        q.host = host;
        q.size = sz;
        return q;
    }
    if (is_dir(host) || (vol.rel.empty() && is_dir(base_for(vol.id)))) {
        q.kind = Query::Kind::Dir;
        q.host = host.empty() && vol.rel.empty() ? base_for(vol.id) : host;
        return q;
    }
    q.kind = Query::Kind::Missing;
    return q;
}

ListStatus list_dir(const std::string& guest, std::vector<DirItem>& out) {
    out.clear();
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return ListStatus::NotVolume;
    bool file = false, occluded = false;
    const Node* node = walk(tree_for(vol.id), vol.rel, file, occluded);
    if (occluded) return ListStatus::Missing;
    if (node && file) return ListStatus::NotDir;
    std::string base_dir = case_path(base_for(vol.id), vol.rel);
    if (vol.rel.empty()) base_dir = base_for(vol.id);
    bool base_is_dir = is_dir(base_dir);
    bool base_is_file = false;
    if (!base_is_dir) {
        uint64_t sz = 0;
        base_is_file = is_file(base_dir, sz);
    }
    if (base_is_file) return ListStatus::NotDir;
    bool overlay_dir = node && !file;
    if (!base_is_dir && !overlay_dir) return ListStatus::Missing;

    std::map<std::string, DirItem> merged;
    if (base_is_dir) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(base_dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (it->is_symlink(sec)) continue;
            std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            DirItem item;
            item.name = name;
            uint64_t sz = 0;
            if (is_file(it->path().string(), sz)) {
                item.dir = false;
                item.size = sz;
            } else if (is_dir(it->path().string())) {
                item.dir = true;
            } else {
                continue;
            }
            merged[lower_ascii(name)] = item;
        }
    }
    if (overlay_dir) {
        for (const auto& [key, child] : node->kids) {
            DirItem item;
            item.name = child.name.empty() ? key : child.name;
            item.overlay = true;
            if (child.file) {
                uint64_t sz = 0;
                if (!is_file(child.host, sz)) continue;
                item.dir = false;
                item.size = sz;
            } else {
                item.dir = true;
            }
            merged[key] = item;
        }
    }
    out.reserve(merged.size());
    for (auto& [_, item] : merged) out.push_back(std::move(item));
    std::sort(out.begin(), out.end(), [](const DirItem& a, const DirItem& b) { return lower_ascii(a.name) < lower_ascii(b.name); });
    return ListStatus::Ok;
}

bool read_only_volume(const std::string& guest) {
    GuestVol vol = parse_guest(guest);
    return vol.id == GuestVol::Id::Content || vol.id == GuestVol::Id::Aoc || vol.id == GuestVol::Id::Code || vol.id == GuestVol::Id::Meta;
}

void note_opened(const std::string& guest) {
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return;
    std::string key = (vol.id == GuestVol::Id::Aoc ? "aoc:" : "content:") + lower_ascii(vol.rel);
    std::lock_guard<std::mutex> lock(g_open_mu);
    g_opened.insert(key);
}

bool dlc_ready() { return g_pack; }

uint64_t aoc_title_id() { return g_title; }

std::string aoc_mount_path() {
    char buf[40];
    snprintf(buf, sizeof buf, "/vol/aoc%016llx", static_cast<unsigned long long>(g_title));
    return buf;
}

bool owns_content(uint16_t id) { return g_pack && g_owned.count(id) != 0; }

size_t index_files() { return g_files; }
size_t replacing_files() { return g_replacing; }
size_t added_files() { return g_added; }
size_t opened_files() {
    std::lock_guard<std::mutex> lock(g_open_mu);
    return g_opened.size();
}

}  // namespace mods
