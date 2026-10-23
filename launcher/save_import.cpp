#include "save_import.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <queue>
#include <sstream>

namespace saveimport {
namespace {

constexpr uint32_t kVer150 = 0x471B;

std::string lower_copy(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string utf8(const std::filesystem::path& p) {
    auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string filename_utf8(const std::filesystem::path& p) {
    auto u = p.filename().u8string();
    return lower_copy(std::string(u.begin(), u.end()));
}

bool is_digits(const std::string& s, int max_len) {
    if (s.empty() || static_cast<int>(s.size()) > max_len) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

bool is_hex8(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool is_persistent_id(const std::string& s) { return s.size() == 8 && s.rfind("800000", 0) == 0 && is_hex8(s); }

bool is_botw_title(const std::string& s) { return s == "101c9300" || s == "101c9400" || s == "101c9500"; }

bool dir_empty(const std::filesystem::path& p) {
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(p, ec);
    return ec || it == std::filesystem::directory_iterator();
}

bool exists_file(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

bool exists_dir(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

const char* known_version(uint32_t v) {
    switch (v) {
        case 0x24E2: return "1.0.0";
        case 0x24EE: return "1.1.0";
        case 0x2588: return "1.2.0";
        case 0x29C0: return "1.3.0";
        case 0x2A46: return "1.3.1";
        case 0x3EF8: return "1.3.3";
        case 0x3EF9: return "1.3.4";
        case 0x471A: return "1.4.0/1.4.1";
        case 0x471B: return "1.5.0";
        case 0x471E: return "1.6.0";
        default: return nullptr;
    }
}

uint32_t read_be(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint32_t read_le(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool plausible(uint32_t v) { return v >= 0x2000 && v <= 0xFFFF; }

SavCheck inspect_file(const std::filesystem::path& file) {
    SavCheck c;
    c.file = file;
    std::ifstream in(file, std::ios::binary);
    uint8_t b[12];
    if (!in || !in.read(reinterpret_cast<char*>(b), 12) || in.gcount() != 12) return c;
    bool marker = b[4] == 0xFF && b[5] == 0xFF && b[6] == 0xFF && b[7] == 0xFF;
    uint32_t be = read_be(b), le = read_le(b);
    uint32_t be_u = read_be(b + 8), le_u = read_le(b + 8);
    bool be_ok = marker && be_u == 1 && plausible(be);
    bool le_ok = marker && le_u == 1 && plausible(le);
    if (be_ok == le_ok) return c;
    c.header_ok = true;
    c.endian = be_ok ? Endian::WiiU : Endian::Switch;
    c.version = be_ok ? be : le;
    if (const char* name = known_version(c.version)) c.version_name = name;
    else {
        char buf[32];
        snprintf(buf, sizeof buf, "0x%04X", c.version);
        c.version_name = buf;
    }
    if (c.version == kVer150) c.age = Age::Exact150;
    else if (c.version > kVer150) c.age = Age::Newer;
    else c.age = Age::Older;
    return c;
}

bool slot_payload(const std::filesystem::path& dir) {
    return exists_file(dir / "game_data.sav") || exists_file(dir / "caption.sav") || exists_file(dir / "caption.jpg");
}

bool is_slot_name(const std::string& name) {
    if (!is_digits(name, 2)) return false;
    int n = std::atoi(name.c_str());
    return n >= 0 && n <= 31;
}

struct Shape {
    int slots = 0;
    bool option = false;
};

Shape shape_of(const std::filesystem::path& dir) {
    Shape s;
    s.option = exists_file(dir / "option.sav");
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_symlink(sec)) continue;
        if (!it->is_directory(sec)) continue;
        if (is_slot_name(filename_utf8(it->path())) && slot_payload(it->path())) s.slots++;
    }
    return s;
}

bool looks_like_account(const std::filesystem::path& dir) {
    Shape s = shape_of(dir);
    return s.option || s.slots > 0;
}

std::string title_from(const std::filesystem::path& account) {
    if (filename_utf8(account.parent_path()) != "user") return {};
    std::string title = filename_utf8(account.parent_path().parent_path());
    return is_hex8(title) ? title : std::string();
}

int score_of(const std::filesystem::path& dir, int depth) {
    Shape s = shape_of(dir);
    int score = s.slots * 10 + (s.option ? 25 : 0);
    std::string name = filename_utf8(dir);
    if (is_persistent_id(name)) {
        score += 80;
        if (name == "80000001") score += 30;
    }
    if (filename_utf8(dir.parent_path()) == "user") score += 40;
    std::string title = title_from(dir);
    if (is_botw_title(title)) score += 40;
    else if (!title.empty()) score += 5;
    if (depth < 0) depth = -depth;
    score -= depth;
    return score;
}

bool skip_dir_name(const std::string& name) {
    return name == "save-sets" || name == "cache" || name == "shadercache" || name == ".git" || name == "generated" ||
           name == "build" || name == "80000001.incoming" || name == "80000001.replaced" || name == "tracker";
}

void collect_savs(const std::filesystem::path& dir, std::vector<SavCheck>& out, int depth) {
    if (depth > 6 || out.size() > 64) return;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_symlink(sec)) continue;
        std::string name = filename_utf8(it->path());
        if (it->is_directory(sec)) {
            if (name == "tracker") continue;
            collect_savs(it->path(), out, depth + 1);
        } else if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".sav") == 0) {
            out.push_back(inspect_file(it->path()));
        }
    }
}

std::string hex4(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%04X", v);
    return buf;
}

void judge(Located& loc) {
    collect_savs(loc.account_dir, loc.files, 0);
    int wiiu = 0, sw = 0, unknown = 0, older = 0, newer = 0;
    uint32_t newer_ver = 0, older_ver = 0;
    std::string newer_name, older_name;
    for (const auto& f : loc.files) {
        if (!f.header_ok) {
            unknown++;
            continue;
        }
        if (f.endian == Endian::Switch) sw++;
        else if (f.endian == Endian::WiiU) wiiu++;
        if (f.age == Age::Newer) {
            newer++;
            if (f.version >= newer_ver) {
                newer_ver = f.version;
                newer_name = f.version_name;
            }
        } else if (f.age == Age::Older) {
            older++;
            if (older_ver == 0 || f.version <= older_ver) {
                older_ver = f.version;
                older_name = f.version_name;
            }
        }
    }
    std::vector<std::string> warns;
    if (sw && wiiu) warns.push_back("Some .sav files are Switch (little-endian) and some are Wii U (big-endian).");
    if (sw) {
        warns.push_back(
            "This is a Switch save (little-endian). BotWRecomp reads Wii U big-endian saves. Convert it before playing; "
            "importing it unchanged will not load.");
    }
    if (newer) {
        warns.push_back("This save is from game version " + newer_name + " (save version " + hex4(newer_ver) +
                        "), newer than 1.5.0 (0x471B). This recompile may refuse it.");
    }
    if (older && !newer) {
        warns.push_back("This save is from " + older_name + " (save version " + hex4(older_ver) +
                        "), older than 1.5.0. The game may not load it.");
    }
    if (!loc.files.empty() && wiiu == 0 && sw == 0) {
        warns.push_back(
            "No .sav file had a recognizable BotW header (a version, marker FFFFFFFF, and a 1 at offset 8).");
    }
    if (!loc.title_id.empty() && !is_botw_title(loc.title_id)) {
        std::string id = loc.title_id;
        for (char& c : id)
            if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
        warns.push_back("Title id " + id +
                        " is not a Breath of the Wild Wii U id (101C9300 JP, 101C9400 US, 101C9500 EU).");
    }
    if (loc.slots == 0) warns.push_back("No numbered slot folders with game_data.sav or caption.sav were found.");
    for (size_t i = 0; i < warns.size(); i++) {
        if (i) loc.warnings += "\n";
        loc.warnings += warns[i];
    }
    bool bad_title = !loc.title_id.empty() && !is_botw_title(loc.title_id);
    bool unknown_only = !loc.files.empty() && wiiu == 0 && sw == 0;
    loc.dangerous = sw || newer || (sw && wiiu) || unknown_only || bad_title;
    loc.caution = !loc.dangerous && (older || loc.slots == 0);
    if (!loc.account_name.empty() && loc.account_name != "80000001") {
        std::string shown = loc.account_name;
        for (char& c : shown)
            if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
        loc.note = "Account folder " + shown +
                   " will be installed as 80000001, which is the account the game reads (nn::act slot 1).";
    }
}

struct Candidate {
    std::filesystem::path dir;
    int score = 0;
    int depth = 0;
};

std::string local_stamp(std::tm* tm_out) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    if (tm_out) *tm_out = tm;
    char buf[32];
    snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec);
    return buf;
}

std::string pretty_time(const std::tm& tm) {
    char buf[40];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec);
    return buf;
}

void write_kv(const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& kv) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const auto& [k, v] : kv) {
        std::string line = v;
        for (char& c : line)
            if (c == '\n' || c == '\r') c = ' ';
        out << k << "=" << line << "\n";
    }
}

std::string kv_get(const std::filesystem::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

bool copy_tree(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err, int depth, int& files) {
    if (depth > 12) {
        err = "save folder is nested too deeply";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    if (ec) {
        err = "could not create " + utf8(dst);
        return false;
    }
    for (auto it = std::filesystem::directory_iterator(src, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_symlink(sec)) continue;
        if (++files > 8000) {
            err = "save folder has too many files";
            return false;
        }
        auto to = dst / it->path().filename();
        if (it->is_directory(sec)) {
            if (!copy_tree(it->path(), to, err, depth + 1, files)) return false;
        } else if (it->is_regular_file(sec)) {
            std::filesystem::copy_file(it->path(), to, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                err = "could not copy " + utf8(it->path());
                return false;
            }
        }
    }
    if (ec) {
        err = "could not read " + utf8(src);
        return false;
    }
    return true;
}

bool copy_tree(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err) {
    int files = 0;
    return copy_tree(src, dst, err, 0, files);
}

std::filesystem::path new_set_dir(const std::filesystem::path& user_root, const std::string& kind, std::tm& tm) {
    std::string stamp = local_stamp(&tm);
    std::filesystem::path root = sets_dir(user_root);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) return {};
    std::filesystem::path dir = root / (stamp + "-" + kind);
    for (int n = 2; exists_dir(dir); n++) dir = root / (stamp + "-" + kind + "-" + std::to_string(n));
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    return dir;
}

bool path_inside(const std::filesystem::path& root, const std::filesystem::path& child) {
    std::error_code ec;
    auto base = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    auto c = std::filesystem::weakly_canonical(child, ec);
    if (ec) return false;
    auto b = base.u8string();
    auto p = c.u8string();
    if (p == b) return true;
    if (p.size() <= b.size() || p.compare(0, b.size(), b) != 0) return false;
    char sep = static_cast<char>(p[b.size()]);
    return sep == '/' || sep == '\\';
}

std::string backup_live(const std::filesystem::path& user_root, std::filesystem::path& out_dir, std::string& err) {
    out_dir.clear();
    auto live = live_account_dir(user_root);
    if (!exists_dir(live) || dir_empty(live)) return {};
    std::tm tm{};
    auto dir = new_set_dir(user_root, "backup", tm);
    if (dir.empty()) {
        err = "could not create a backup folder under save-sets";
        return {};
    }
    if (!copy_tree(live, dir / "account", err)) {
        std::error_code rec;
        std::filesystem::remove_all(dir, rec);
        return {};
    }
    auto common = user_root / "save" / "common";
    if (exists_dir(common) && !dir_empty(common)) {
        if (!copy_tree(common, dir / "common", err)) {
            std::error_code rec;
            std::filesystem::remove_all(dir, rec);
            return {};
        }
    }
    Shape s = shape_of(live);
    write_kv(dir / "set.txt", {{"kind", "backup"},
                               {"title", "Backup " + pretty_time(tm)},
                               {"created", pretty_time(tm)},
                               {"source", utf8(live)},
                               {"warnings", "none"},
                               {"slots", std::to_string(s.slots)}});
    out_dir = dir;
    return pretty_time(tm);
}

// Move the account into place. A crash mid-copy leaves the previous directory as
// 80000001.replaced and the new one as 80000001.incoming, both under save/.
bool install_account(const std::filesystem::path& user_root, const std::filesystem::path& account, std::string& err) {
    auto live = live_account_dir(user_root);
    auto save = user_root / "save";
    std::error_code ec;
    std::filesystem::create_directories(save, ec);
    auto staging = save / "80000001.incoming";
    auto replaced = save / "80000001.replaced";
    std::filesystem::remove_all(staging, ec);
    std::filesystem::remove_all(replaced, ec);
    if (!copy_tree(account, staging, err)) return false;
    if (exists_dir(live)) {
        std::filesystem::rename(live, replaced, ec);
        if (ec) {
            std::error_code rec;
            std::filesystem::remove_all(staging, rec);
            err = "could not move the current save aside (" + ec.message() + "). Close the game and try again.";
            return false;
        }
    }
    std::filesystem::rename(staging, live, ec);
    if (ec) {
        std::error_code ec2;
        if (exists_dir(replaced)) std::filesystem::rename(replaced, live, ec2);
        std::filesystem::remove_all(staging, ec2);
        err = "could not install the save (" + ec.message() + ").";
        if (!ec2 && exists_dir(live)) err += " The previous save was put back.";
        return false;
    }
    std::filesystem::remove_all(replaced, ec);
    return true;
}

bool install_common(const std::filesystem::path& user_root, const std::filesystem::path& common, std::string& err) {
    if (common.empty() || !exists_dir(common)) return true;
    auto save = user_root / "save";
    auto dest = save / "common";
    auto staging = save / "common.incoming";
    auto replaced = save / "common.replaced";
    std::error_code ec;
    std::filesystem::create_directories(save, ec);
    std::filesystem::remove_all(staging, ec);
    std::filesystem::remove_all(replaced, ec);
    if (!copy_tree(common, staging, err)) return false;
    if (exists_dir(dest)) {
        std::filesystem::rename(dest, replaced, ec);
        if (ec) {
            std::error_code rec;
            std::filesystem::remove_all(staging, rec);
            err = "could not move the current common save aside (" + ec.message() + ").";
            return false;
        }
    }
    std::filesystem::rename(staging, dest, ec);
    if (ec) {
        std::error_code ec2;
        if (exists_dir(replaced)) std::filesystem::rename(replaced, dest, ec2);
        std::filesystem::remove_all(staging, ec2);
        err = "could not install user/common (" + ec.message() + ").";
        return false;
    }
    std::filesystem::remove_all(replaced, ec);
    return true;
}

OpResult activate(const std::filesystem::path& user_root, const std::filesystem::path& set_dir, bool backup) {
    OpResult r;
    if (!exists_dir(set_dir / "account")) {
        r.message = "That save set has no account folder.";
        return r;
    }
    std::string err;
    if (backup) {
        std::filesystem::path backup_dir;
        backup_live(user_root, backup_dir, err);
        if (!err.empty()) {
            r.message = err;
            return r;
        }
        r.backup = backup_dir;
    }
    if (!install_account(user_root, set_dir / "account", err)) {
        r.message = err;
        return r;
    }
    if (exists_dir(set_dir / "common") && !install_common(user_root, set_dir / "common", err)) {
        r.message = "The account save was installed, but user/common was not: " + err;
        r.ok = true;
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace

std::filesystem::path live_account_dir(const std::filesystem::path& user_root) { return user_root / "save" / "80000001"; }

std::filesystem::path sets_dir(const std::filesystem::path& user_root) { return user_root / "save-sets"; }

bool locate(const std::filesystem::path& input, Located& out, std::string& err) {
    out = Located{};
    std::error_code ec;
    std::filesystem::path start = input;
    if (std::filesystem::is_regular_file(start, ec)) start = start.parent_path();
    if (!exists_dir(start)) {
        err = "that path is not a folder or a file inside a save";
        return false;
    }
    std::vector<Candidate> cands;
    auto consider = [&](const std::filesystem::path& dir, int depth) {
        if (!exists_dir(dir) || !looks_like_account(dir)) return;
        cands.push_back({dir, score_of(dir, depth), depth});
    };
    std::filesystem::path cur = start;
    for (int i = 0; i < 8; i++) {
        consider(cur, -i);
        auto parent = cur.parent_path();
        if (parent == cur || parent.empty()) break;
        cur = parent;
    }
    struct Node {
        std::filesystem::path dir;
        int depth;
    };
    std::queue<Node> q;
    q.push({start, 0});
    int seen = 0;
    while (!q.empty() && seen < 5000) {
        Node n = q.front();
        q.pop();
        if (n.depth > 8) continue;
        std::error_code dec;
        for (auto it = std::filesystem::directory_iterator(n.dir, dec); !dec && it != std::filesystem::directory_iterator();
             it.increment(dec)) {
            if (dec || seen++ > 5000) break;
            std::error_code sec;
            if (it->is_symlink(sec) || !it->is_directory(sec)) continue;
            std::string name = filename_utf8(it->path());
            if (skip_dir_name(name)) continue;
            consider(it->path(), n.depth + 1);
            q.push({it->path(), n.depth + 1});
        }
    }
    if (cands.empty()) {
        err = "no BotW save found. Expected option.sav and/or slot folders 0, 1, ... with game_data.sav, "
              "or a Cemu mlc01/usr/save/00050000/<title>/user/80000001 folder.";
        return false;
    }
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        int da = a.depth < 0 ? -a.depth : a.depth;
        int db = b.depth < 0 ? -b.depth : b.depth;
        if (da != db) return da < db;
        return utf8(a.dir) < utf8(b.dir);
    });
    out.account_dir = cands.front().dir;
    Shape s = shape_of(out.account_dir);
    out.slots = s.slots;
    out.has_option = s.option;
    out.account_name = filename_utf8(out.account_dir);
    if (!is_persistent_id(out.account_name)) out.account_name.clear();
    out.title_id = title_from(out.account_dir);
    auto common = out.account_dir.parent_path() / "common";
    if (exists_dir(common) && !dir_empty(common)) out.common_dir = common;
    judge(out);
    return true;
}

OpResult import_account(const std::filesystem::path& user_root, const Located& src, const std::string& source_label) {
    OpResult r;
    std::error_code ec;
    if (exists_dir(live_account_dir(user_root))) {
        auto live = std::filesystem::weakly_canonical(live_account_dir(user_root), ec);
        auto from = std::filesystem::weakly_canonical(src.account_dir, ec);
        if (!ec && live == from) {
            r.message = "That folder is already the live save (save/80000001).";
            return r;
        }
    }
    std::tm tm{};
    auto set = new_set_dir(user_root, "import", tm);
    if (set.empty()) {
        r.message = "could not create a folder under save-sets";
        return r;
    }
    std::string err;
    if (!copy_tree(src.account_dir, set / "account", err)) {
        std::error_code rec;
        std::filesystem::remove_all(set, rec);
        r.message = err;
        return r;
    }
    if (!src.common_dir.empty() && !copy_tree(src.common_dir, set / "common", err)) {
        std::error_code rec;
        std::filesystem::remove_all(set, rec);
        r.message = err;
        return r;
    }
    write_kv(set / "set.txt", {{"kind", "import"},
                               {"title", "Imported " + pretty_time(tm)},
                               {"created", pretty_time(tm)},
                               {"source", source_label},
                               {"warnings", src.warnings.empty() ? "none" : src.warnings},
                               {"slots", std::to_string(src.slots)}});
    r = activate(user_root, set, true);
    r.imported = set;
    if (!r.ok) return r;
    std::string msg = "Imported " + std::to_string(src.slots) + " slot(s)";
    if (src.has_option) msg += " and option.sav";
    msg += " into save/80000001.";
    if (!r.backup.empty()) msg += "\nPrevious save backed up as " + utf8(r.backup.filename()) + ".";
    else msg += "\nThere was no previous save to back up.";
    msg += "\nKept a copy as " + utf8(set.filename()) + ".";
    if (!src.note.empty()) msg += "\n" + src.note;
    r.message = msg;
    r.ok = true;
    return r;
}

OpResult switch_to(const std::filesystem::path& user_root, const std::filesystem::path& set_dir) {
    if (!path_inside(sets_dir(user_root), set_dir)) {
        OpResult r;
        r.message = "That folder is not one of the saved sets.";
        return r;
    }
    OpResult r = activate(user_root, set_dir, true);
    if (!r.ok) return r;
    std::string title = kv_get(set_dir / "set.txt", "title");
    if (title.empty()) title = utf8(set_dir.filename());
    r.message = "Switched to \"" + title + "\".";
    if (!r.backup.empty()) r.message += "\nPrevious save backed up as " + utf8(r.backup.filename()) + ".";
    r.ok = true;
    return r;
}

OpResult delete_set(const std::filesystem::path& user_root, const std::filesystem::path& set_dir) {
    OpResult r;
    if (!path_inside(sets_dir(user_root), set_dir) || set_dir == sets_dir(user_root)) {
        r.message = "Refusing to delete a folder that is not a saved set.";
        return r;
    }
    std::error_code ec;
    std::filesystem::remove_all(set_dir, ec);
    if (ec) {
        r.message = "could not delete " + utf8(set_dir.filename()) + " (" + ec.message() + ")";
        return r;
    }
    r.ok = true;
    r.message = "Deleted " + utf8(set_dir.filename()) + ".";
    return r;
}

std::vector<SetInfo> list_sets(const std::filesystem::path& user_root) {
    std::vector<SetInfo> out;
    std::error_code ec;
    auto root = sets_dir(user_root);
    if (!exists_dir(root)) return out;
    for (auto it = std::filesystem::directory_iterator(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_symlink(sec) || !it->is_directory(sec)) continue;
        SetInfo s;
        s.dir = it->path();
        auto meta = it->path() / "set.txt";
        s.kind = kv_get(meta, "kind");
        s.title = kv_get(meta, "title");
        s.created = kv_get(meta, "created");
        s.warnings = kv_get(meta, "warnings");
        s.source = kv_get(meta, "source");
        s.slots = std::atoi(kv_get(meta, "slots").c_str());
        std::string folder = utf8(it->path().filename());
        if (s.kind.empty()) {
            if (folder.find("-backup") != std::string::npos) s.kind = "backup";
            else if (folder.find("-import") != std::string::npos) s.kind = "import";
            else s.kind = "set";
        }
        if (s.title.empty()) s.title = folder;
        if (s.warnings == "none") s.warnings.clear();
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SetInfo& a, const SetInfo& b) { return utf8(a.dir) > utf8(b.dir); });
    return out;
}

bool decimal_legacy_folder(const std::filesystem::path& user_root) {
    return looks_like_account(user_root / "save" / "2147483649");
}

bool decimal_legacy_present(const std::filesystem::path& user_root) {
    if (!decimal_legacy_folder(user_root)) return false;
    return !looks_like_account(live_account_dir(user_root));
}

bool peek_decimal(const std::filesystem::path& user_root, Located& out) {
    out = Located{};
    auto legacy = user_root / "save" / "2147483649";
    if (!looks_like_account(legacy)) return false;
    out.account_dir = legacy;
    Shape s = shape_of(legacy);
    out.slots = s.slots;
    out.has_option = s.option;
    out.account_name = "2147483649";
    judge(out);
    return true;
}

OpResult migrate_decimal(const std::filesystem::path& user_root) {
    OpResult r;
    Located src;
    if (!peek_decimal(user_root, src)) {
        r.message = "save/2147483649 does not contain a BotW save.";
        return r;
    }
    std::tm tm{};
    auto set = new_set_dir(user_root, "migrate", tm);
    if (set.empty()) {
        r.message = "could not create a folder under save-sets";
        return r;
    }
    std::string err;
    if (!copy_tree(src.account_dir, set / "account", err)) {
        std::error_code rec;
        std::filesystem::remove_all(set, rec);
        r.message = err;
        return r;
    }
    write_kv(set / "set.txt", {{"kind", "migrate"},
                               {"title", "Old decimal save " + pretty_time(tm)},
                               {"created", pretty_time(tm)},
                               {"source", "save/2147483649"},
                               {"warnings", src.warnings.empty() ? "none" : src.warnings},
                               {"slots", std::to_string(src.slots)}});
    r = activate(user_root, set, true);
    r.imported = set;
    if (!r.ok) return r;
    std::string msg = "Copied save/2147483649 to save/80000001. The old folder was left in place.";
    if (!r.backup.empty()) msg += "\nPrevious save/80000001 backed up as " + utf8(r.backup.filename()) + ".";
    msg += "\nKept a copy as " + utf8(set.filename()) + ".";
    if (!src.note.empty()) msg += "\n" + src.note;
    if (!src.warnings.empty()) msg += "\n" + src.warnings;
    r.message = msg;
    r.ok = true;
    return r;
}

}  // namespace saveimport
