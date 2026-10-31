#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// Full-fledged mod overlay, Cemu graphic pack engine ([Definition], [Default], [Preset],
// [TextureRedefine]), arithmetic/comparison expression evaluator, patches.txt / patch_*.txt
// memory & function entry patcher, custom GLSL shader replacement loader, smart mod root
// detector (direct Actor/Pack/... folders, nested wrapper unwrapping, BCML/UKMM), UTF-8 path
// support, live hot-reload/reordering, and .zip/folder installer.
#include "coreinit/mods.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "log.h"

namespace fs = std::filesystem;

FILE* mods::fopen_utf8(const std::string& utf8_path, const char* mode) {
    if (utf8_path.empty() || !mode) return nullptr;
#if defined(_WIN32)
    try {
        fs::path p(std::u8string_view(reinterpret_cast<const char8_t*>(utf8_path.data()), utf8_path.size()));
        std::wstring wmode;
        for (const char* m = mode; *m; ++m) wmode.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*m)));
        return _wfopen(p.c_str(), wmode.c_str());
    } catch (...) {
        return fopen(utf8_path.c_str(), mode);
    }
#else
    return fopen(utf8_path.c_str(), mode);
#endif
}

namespace {

constexpr int kUserDlc = -1;
constexpr size_t kConflictLines = 50;

struct Node {
    std::string name;
    std::string host;
    bool file = false;
    bool base = false;  // the game dump has a file at this path
    int owner = kUserDlc;
    std::map<std::string, Node> kids;  // key: lowercase name
};

struct Conflict {
    std::string path;  // "content/..." or "aoc/..."
    int loser = 0, winner = 0;
};

struct RawTextureRule {
    std::string width;
    std::string height;
    std::string depth;
    std::string formats;
    std::string formats_excluded;
    std::string tile_modes;
    std::string tile_modes_excluded;
    std::string overwrite_width;
    std::string overwrite_height;
    std::string overwrite_depth;
    std::string overwrite_format;
    std::string overwrite_lod_bias;
    std::string overwrite_anisotropy;
};

struct RulesFile {
    bool has_definition = false;
    std::string name;
    std::string description;
    std::string version;
    std::vector<std::string> title_ids;
    std::vector<std::pair<std::string, std::string>> default_vars;
    std::vector<mods::PresetDef> presets;
    std::vector<RawTextureRule> texture_rules;
};

struct SavedByte {
    uint32_t addr = 0;
    uint8_t orig = 0;
};

std::recursive_mutex g_mods_mtx;
std::mutex g_case_mu;
std::mutex g_open_mu;
std::mutex g_disc_mu;

Node g_content;
Node g_aoc;
std::string g_content_base;
std::string g_aoc_base;
uint64_t g_title = 0x0005000C101C9400ull;
bool g_title_from_meta = false;
bool g_pack = false;
bool g_inited = false;
bool g_no_mods = false;
bool g_no_dlc = false;
uint64_t g_base_title_id = 0;
std::string g_game_path;
std::string g_user_path;

std::vector<mods::DiscoveredMod> g_discovered;
bool g_pending_restart = false;
std::vector<mods::ModInfo> g_mods;
std::vector<Conflict> g_conflicts;
std::unordered_set<uint16_t> g_owned;
std::unordered_set<std::string> g_opened;
std::map<std::string, std::map<std::string, std::string>> g_case_cache;
size_t g_files = 0;
size_t g_replacing = 0;
size_t g_added = 0;

std::atomic<uint64_t> g_graphic_pack_version{1};
mods::PatchBackend g_patch_backend;
std::vector<SavedByte> g_saved_patch_bytes;

std::string trim(std::string_view sv) {
    size_t a = 0;
    size_t b = sv.size();
    while (a < b && (sv[a] == ' ' || sv[a] == '\t' || sv[a] == '\r' || sv[a] == '\n')) a++;
    while (b > a && (sv[b - 1] == ' ' || sv[b - 1] == '\t' || sv[b - 1] == '\r' || sv[b - 1] == '\n')) b--;
    return std::string(sv.substr(a, b - a));
}

std::string lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) return false;
    }
    return true;
}

bool iless(const std::string& a, const std::string& b) {
    std::string la = lower_ascii(a), lb = lower_ascii(b);
    return la != lb ? la < lb : a < b;
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

bool valid_folder(const std::string& s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (unsigned char c : s) {
        if (c < 32) return false;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

std::string path_to_utf8(const fs::path& p) {
    try {
        std::u8string u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (...) {
        try {
            return p.string();
        } catch (...) {
            return {};
        }
    }
}

fs::path utf8_to_path(std::string_view utf8) {
    try {
        std::u8string_view u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
        return fs::path(u8);
    } catch (...) {
        return fs::path(std::string(utf8));
    }
}

bool read_file_utf8(const fs::path& path, std::string& out, size_t cap = 16 * 1024 * 1024) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    std::string utf8 = path_to_utf8(path);
    FILE* f = mods::fopen_utf8(utf8, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return false;
    }
    rewind(f);
    size_t to_read = std::min(static_cast<size_t>(sz), cap);
    out.resize(to_read);
    if (to_read > 0) {
        size_t got = fread(out.data(), 1, to_read, f);
        out.resize(got);
    }
    fclose(f);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

std::string read_text(const fs::path& p, size_t cap = 256 * 1024) {
    std::string s;
    read_file_utf8(p, s, cap);
    return s;
}

// ============================================================================
// 1. Expression Evaluator for rules.txt and patches.txt
// ============================================================================
class ExprParser {
public:
    ExprParser(std::string_view src, const std::vector<std::pair<std::string, double>>& vars, bool unknown_zero)
        : s_(src), vars_(vars), unknown_zero_(unknown_zero) {}

    bool parse(double& out) {
        skip_ws();
        if (pos_ >= s_.size()) return false;
        double v = parse_ternary();
        skip_ws();
        if (!ok_ || pos_ != s_.size()) return false;
        out = v;
        return true;
    }

private:
    std::string_view s_;
    const std::vector<std::pair<std::string, double>>& vars_;
    bool unknown_zero_ = false;
    size_t pos_ = 0;
    bool ok_ = true;

    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) pos_++;
    }

    bool match(std::string_view tok) {
        skip_ws();
        if (s_.substr(pos_, tok.size()) == tok) {
            pos_ += tok.size();
            return true;
        }
        return false;
    }

    double parse_ternary() {
        double cond = parse_or();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '?') {
            pos_++;
            double t_val = parse_ternary();
            if (!match(":")) {
                ok_ = false;
                return 0.0;
            }
            double f_val = parse_ternary();
            return (std::fabs(cond) > 1e-12) ? t_val : f_val;
        }
        return cond;
    }

    double parse_or() {
        double lhs = parse_and();
        while (ok_) {
            skip_ws();
            if (s_.substr(pos_, 2) == "||") {
                pos_ += 2;
                double rhs = parse_and();
                lhs = (std::fabs(lhs) > 1e-12 || std::fabs(rhs) > 1e-12) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_and() {
        double lhs = parse_bit_or();
        while (ok_) {
            skip_ws();
            if (s_.substr(pos_, 2) == "&&") {
                pos_ += 2;
                double rhs = parse_bit_or();
                lhs = (std::fabs(lhs) > 1e-12 && std::fabs(rhs) > 1e-12) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_bit_or() {
        double lhs = parse_bit_xor();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '|' && s_.substr(pos_, 2) != "||") {
                pos_++;
                double rhs = parse_bit_xor();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) | static_cast<int64_t>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_bit_xor() {
        double lhs = parse_bit_and();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '^') {
                pos_++;
                double rhs = parse_bit_and();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) ^ static_cast<int64_t>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_bit_and() {
        double lhs = parse_eq();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '&' && s_.substr(pos_, 2) != "&&") {
                pos_++;
                double rhs = parse_eq();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) & static_cast<int64_t>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_eq() {
        double lhs = parse_rel();
        while (ok_) {
            skip_ws();
            if (match("==")) {
                double rhs = parse_rel();
                lhs = (std::fabs(lhs - rhs) <= 1e-9) ? 1.0 : 0.0;
            } else if (match("!=")) {
                double rhs = parse_rel();
                lhs = (std::fabs(lhs - rhs) > 1e-9) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_rel() {
        double lhs = parse_shift();
        while (ok_) {
            skip_ws();
            if (match("<=")) {
                double rhs = parse_shift();
                lhs = (lhs <= rhs + 1e-9) ? 1.0 : 0.0;
            } else if (match(">=")) {
                double rhs = parse_shift();
                lhs = (lhs + 1e-9 >= rhs) ? 1.0 : 0.0;
            } else if (match("<")) {
                double rhs = parse_shift();
                lhs = (lhs < rhs - 1e-9) ? 1.0 : 0.0;
            } else if (match(">")) {
                double rhs = parse_shift();
                lhs = (lhs > rhs + 1e-9) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_shift() {
        double lhs = parse_add();
        while (ok_) {
            skip_ws();
            if (match("<<")) {
                double rhs = parse_add();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) << static_cast<int>(rhs));
            } else if (match(">>")) {
                double rhs = parse_add();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) >> static_cast<int>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_add() {
        double lhs = parse_mul();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '+') {
                pos_++;
                lhs += parse_mul();
            } else if (pos_ < s_.size() && s_[pos_] == '-') {
                pos_++;
                lhs -= parse_mul();
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_mul() {
        double lhs = parse_unary();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '*') {
                pos_++;
                lhs *= parse_unary();
            } else if (pos_ < s_.size() && s_[pos_] == '/') {
                pos_++;
                double rhs = parse_unary();
                if (std::fabs(rhs) < 1e-15) {
                    ok_ = false;
                    return 0.0;
                }
                lhs /= rhs;
            } else if (pos_ < s_.size() && s_[pos_] == '%') {
                pos_++;
                double rhs = parse_unary();
                if (std::fabs(rhs) < 1e-15) {
                    ok_ = false;
                    return 0.0;
                }
                lhs = std::fmod(lhs, rhs);
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_unary() {
        skip_ws();
        if (pos_ < s_.size()) {
            if (s_[pos_] == '+') {
                pos_++;
                return parse_unary();
            }
            if (s_[pos_] == '-') {
                pos_++;
                return -parse_unary();
            }
            if (s_[pos_] == '!' && s_.substr(pos_, 2) != "!=") {
                pos_++;
                double v = parse_unary();
                return (std::fabs(v) <= 1e-12) ? 1.0 : 0.0;
            }
            if (s_[pos_] == '~') {
                pos_++;
                double v = parse_unary();
                return static_cast<double>(~static_cast<int64_t>(v));
            }
        }
        return parse_primary();
    }

    double lookup_var(std::string_view name) {
        if (!name.empty() && name[0] == '$') name.remove_prefix(1);
        for (auto it = vars_.rbegin(); it != vars_.rend(); ++it) {
            std::string_view k = it->first;
            if (!k.empty() && k[0] == '$') k.remove_prefix(1);
            if (k == name) return it->second;
        }
        if (unknown_zero_) return 0.0;
        ok_ = false;
        return 0.0;
    }

    double parse_primary() {
        skip_ws();
        if (pos_ >= s_.size()) {
            ok_ = false;
            return 0.0;
        }
        if (s_[pos_] == '(') {
            pos_++;
            double v = parse_ternary();
            if (!match(")")) {
                ok_ = false;
                return 0.0;
            }
            return v;
        }
        if (s_[pos_] == '$') {
            pos_++;
            size_t start = pos_;
            while (pos_ < s_.size() &&
                   (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) {
                pos_++;
            }
            if (pos_ == start) {
                ok_ = false;
                return 0.0;
            }
            std::string_view name = s_.substr(start, pos_ - start);
            // In Cemu rules.txt, variables can have a suffix like $var:int
            if (pos_ < s_.size() && s_[pos_] == ':') {
                size_t save = pos_;
                pos_++;
                size_t ts = pos_;
                while (pos_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[pos_]))) pos_++;
                std::string_view tag = s_.substr(ts, pos_ - ts);
                if (tag != "int" && tag != "float" && tag != "double") {
                    pos_ = save;
                }
            }
            return lookup_var(name);
        }
        if (std::isalpha(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_') {
            size_t start = pos_;
            while (pos_ < s_.size() &&
                   (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) {
                pos_++;
            }
            std::string ident = lower_ascii(s_.substr(start, pos_ - start));
            std::string_view raw_ident = s_.substr(start, pos_ - start);
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '(') {
                pos_++;
                std::vector<double> args;
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] != ')') {
                    while (ok_) {
                        args.push_back(parse_ternary());
                        skip_ws();
                        if (match(",")) continue;
                        break;
                    }
                }
                if (!match(")")) {
                    ok_ = false;
                    return 0.0;
                }
                if (ident == "floor" && args.size() == 1) return std::floor(args[0]);
                if (ident == "ceil" && args.size() == 1) return std::ceil(args[0]);
                if (ident == "round" && args.size() == 1) return std::round(args[0]);
                if (ident == "trunc" && args.size() == 1) return std::trunc(args[0]);
                if (ident == "abs" && args.size() == 1) return std::fabs(args[0]);
                if (ident == "sqrt" && args.size() == 1) return args[0] >= 0.0 ? std::sqrt(args[0]) : 0.0;
                if (ident == "min" && args.size() == 2) return std::min(args[0], args[1]);
                if (ident == "max" && args.size() == 2) return std::max(args[0], args[1]);
                if (ident == "clamp" && args.size() == 3) return std::clamp(args[0], args[1], args[2]);
                ok_ = false;
                return 0.0;
            }
            if (ident == "true") return 1.0;
            if (ident == "false") return 0.0;
            return lookup_var(raw_ident);
        }
        // Number literal (hex 0x... or decimal/float)
        if (pos_ + 2 <= s_.size() && s_[pos_] == '0' && (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X')) {
            pos_ += 2;
            size_t start = pos_;
            uint64_t val = 0;
            while (pos_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[pos_]))) {
                char c = s_[pos_++];
                uint64_t d = (c >= '0' && c <= '9') ? (c - '0')
                           : (c >= 'a' && c <= 'f') ? (c - 'a' + 10)
                                                    : (c - 'A' + 10);
                val = (val << 4) | d;
            }
            if (pos_ == start) {
                ok_ = false;
                return 0.0;
            }
            return static_cast<double>(val);
        }
        size_t start = pos_;
        bool seen_digit = false;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            seen_digit = true;
            pos_++;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            pos_++;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
                seen_digit = true;
                pos_++;
            }
        }
        if (!seen_digit) {
            ok_ = false;
            return 0.0;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            pos_++;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) pos_++;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) pos_++;
        }
        std::string num_str(s_.substr(start, pos_ - start));
        // Skip optional 'f' suffix on float literals
        if (pos_ < s_.size() && (s_[pos_] == 'f' || s_[pos_] == 'F')) pos_++;
        char* endptr = nullptr;
        double val = std::strtod(num_str.c_str(), &endptr);
        if (!endptr || *endptr != '\0') {
            ok_ = false;
            return 0.0;
        }
        return val;
    }
};

}  // namespace

bool mods::eval_expr(std::string_view expr, const std::vector<std::pair<std::string, double>>& vars,
                     double* out_val, bool unknown_var_zero) {
    ExprParser p(expr, vars, unknown_var_zero);
    double v = 0.0;
    if (!p.parse(v)) return false;
    if (out_val) *out_val = v;
    return true;
}
namespace {

void set_var(std::vector<std::pair<std::string, double>>& vars, std::string name, double val) {
    if (!name.empty() && name[0] == '$') name.erase(0, 1);
    auto colon = name.find(':');
    if (colon != std::string::npos) name.erase(colon);
    name = trim(name);
    if (name.empty()) return;
    for (auto& kv : vars) {
        if (kv.first == name) {
            kv.second = val;
            return;
        }
    }
    vars.emplace_back(std::move(name), val);
}

std::string strip_comment(std::string_view line) {
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') in_quotes = !in_quotes;
        if (!in_quotes) {
            if (c == '#') return std::string(line.substr(0, i));
            if (c == ';' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))))
                return std::string(line.substr(0, i));
            if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

// Calls fn(section_lowercase, key, value) for every key = value line.
template <class Fn>
void each_ini_line(std::string_view text, Fn fn) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);
    std::string section;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string line = trim(text.substr(at, nl - at));
        at = nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            section = lower_ascii(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        if (key.empty()) continue;
        fn(section, key, trim(line.substr(eq + 1)));
    }
}

std::vector<int32_t> parse_int_list(std::string_view sv,
                                    const std::vector<std::pair<std::string, double>>& vars) {
    std::vector<int32_t> out;
    size_t start = 0;
    while (start <= sv.size()) {
        size_t comma = sv.find(',', start);
        std::string item = trim(comma == std::string_view::npos ? sv.substr(start)
                                                                : sv.substr(start, comma - start));
        if (!item.empty()) {
            double v = 0.0;
            if (mods::eval_expr(item, vars, &v, true)) {
                out.push_back(static_cast<int32_t>(std::llround(v)));
            }
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

// ============================================================================
// 2. rules.txt Parser ([Definition], [Default], [Preset], [TextureRedefine])
// ============================================================================
RulesFile parse_rules_internal(std::string_view content) {
    RulesFile r;
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
        content.remove_prefix(3);
    }

    std::string section;
    mods::PresetDef cur_preset;
    bool in_preset = false;
    RawTextureRule cur_tex;
    bool in_tex = false;

    auto flush_section = [&]() {
        if (in_preset) {
            if (!cur_preset.name.empty()) r.presets.push_back(std::move(cur_preset));
            cur_preset = mods::PresetDef{};
            in_preset = false;
        }
        if (in_tex) {
            r.texture_rules.push_back(std::move(cur_tex));
            cur_tex = RawTextureRule{};
            in_tex = false;
        }
    };

    size_t at = 0;
    while (at <= content.size()) {
        size_t nl = content.find('\n', at);
        if (nl == std::string_view::npos) nl = content.size();
        std::string line_raw(content.substr(at, nl - at));
        at = nl + 1;

        std::string s = trim(strip_comment(line_raw));
        if (s.empty()) continue;
        if (s.front() == '[') {
            flush_section();
            size_t close = s.find(']');
            section = lower_ascii(trim(s.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            if (section == "definition") {
                r.has_definition = true;
            } else if (section == "preset") {
                in_preset = true;
            } else if (section == "textureredefine") {
                in_tex = true;
            }
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string raw_k = trim(std::string_view(s).substr(0, eq));
        std::string k = lower_ascii(raw_k);
        std::string v = unquote(trim(std::string_view(s).substr(eq + 1)));

        if (!raw_k.empty() && raw_k[0] == '$') {
            if (in_preset) {
                cur_preset.vars.emplace_back(raw_k, v);
            } else {
                r.default_vars.emplace_back(raw_k, v);
            }
            continue;
        }

        if (section == "definition") {
            r.has_definition = true;
            if (k == "name" && r.name.empty()) r.name = v;
            else if (k == "description" && r.description.empty()) r.description = v;
            else if (k == "version" && r.version.empty()) r.version = v;
            else if (k == "titleids" && r.title_ids.empty()) {
                std::string cur;
                for (char c : v + ",") {
                    if (c == ',') {
                        std::string t = lower_ascii(trim(cur));
                        if (!t.empty()) r.title_ids.push_back(t);
                        cur.clear();
                    } else {
                        cur.push_back(c);
                    }
                }
            }
        } else if (section == "default") {
            r.default_vars.emplace_back(raw_k, v);
        } else if (in_preset) {
            if (k == "name") cur_preset.name = v;
            else if (k == "category") cur_preset.category = v;
            else if (k == "default") cur_preset.is_default = (v == "1" || iequals(v, "true") || iequals(v, "yes"));
            else if (k == "condition") cur_preset.condition = v;
        } else if (in_tex) {
            if (k == "width") cur_tex.width = v;
            else if (k == "height") cur_tex.height = v;
            else if (k == "depth") cur_tex.depth = v;
            else if (k == "formats") cur_tex.formats = v;
            else if (k == "formatsexcluded") cur_tex.formats_excluded = v;
            else if (k == "tilemodes") cur_tex.tile_modes = v;
            else if (k == "tilemodesexcluded") cur_tex.tile_modes_excluded = v;
            else if (k == "overwritewidth") cur_tex.overwrite_width = v;
            else if (k == "overwriteheight") cur_tex.overwrite_height = v;
            else if (k == "overwritedepth") cur_tex.overwrite_depth = v;
            else if (k == "overwriteformat") cur_tex.overwrite_format = v;
            else if (k == "overwritelodbias") cur_tex.overwrite_lod_bias = v;
            else if (k == "overwriteanisotropy") cur_tex.overwrite_anisotropy = v;
        }
    }
    flush_section();

    // Cemu writes "\n" in descriptions as a literal backslash-n; keep it on one line.
    for (std::string* str_p : {&r.name, &r.description}) {
        std::string o;
        for (size_t i = 0; i < str_p->size(); i++) {
            if ((*str_p)[i] == '\\' && i + 1 < str_p->size() && (*str_p)[i + 1] == 'n') {
                o += ' ';
                i++;
            } else {
                o += (*str_p)[i];
            }
        }
        *str_p = trim(o);
    }
    return r;
}

void evaluate_rules_for_mod(const std::string& folder, const RulesFile& rf, const mods::Config& cfg,
                            std::vector<mods::PresetCategory>& out_categories,
                            std::vector<std::pair<std::string, double>>& out_vars,
                            std::vector<mods::TextureRedefineRule>& out_tex_rules) {
    out_categories.clear();
    out_vars.clear();
    out_tex_rules.clear();

    for (const auto& kv : rf.default_vars) {
        double val = 0.0;
        if (mods::eval_expr(kv.second, out_vars, &val, true)) {
            set_var(out_vars, kv.first, val);
        }
    }

    std::vector<std::string> cat_order;
    for (const auto& p : rf.presets) {
        if (std::find(cat_order.begin(), cat_order.end(), p.category) == cat_order.end()) {
            cat_order.push_back(p.category);
        }
    }

    for (const std::string& cat : cat_order) {
        std::vector<const mods::PresetDef*> matching;
        for (const auto& p : rf.presets) {
            if (p.category != cat) continue;
            if (!p.condition.empty()) {
                double cond_val = 0.0;
                if (!mods::eval_expr(p.condition, out_vars, &cond_val, true) ||
                    std::fabs(cond_val) <= 1e-12) {
                    continue;
                }
            }
            matching.push_back(&p);
        }
        if (matching.empty()) continue;

        mods::PresetCategory pc;
        pc.category = cat;
        const mods::PresetDef* chosen = nullptr;

        std::string saved_name = cfg.get_preset(folder, cat);
        if (saved_name.empty() && cat_order.size() == 1) {
            saved_name = cfg.get_preset(folder, "");
        }

        const mods::PresetDef* default_preset = nullptr;
        for (const auto* p : matching) {
            if (std::find(pc.options.begin(), pc.options.end(), p->name) == pc.options.end()) {
                pc.options.push_back(p->name);
            }
            if (!saved_name.empty() && iequals(p->name, saved_name) && !chosen) chosen = p;
            if (p->is_default && !default_preset) default_preset = p;
        }
        if (!chosen) chosen = default_preset ? default_preset : matching.front();
        pc.active = chosen->name;
        out_categories.push_back(std::move(pc));

        for (const auto& kv : chosen->vars) {
            double val = 0.0;
            if (mods::eval_expr(kv.second, out_vars, &val, true)) {
                set_var(out_vars, kv.first, val);
            }
        }
    }

    for (const auto& rt : rf.texture_rules) {
        mods::TextureRedefineRule tr;
        tr.width = parse_int_list(rt.width, out_vars);
        tr.height = parse_int_list(rt.height, out_vars);
        tr.depth = parse_int_list(rt.depth, out_vars);
        tr.formats = parse_int_list(rt.formats, out_vars);
        tr.formats_excluded = parse_int_list(rt.formats_excluded, out_vars);
        tr.tile_modes = parse_int_list(rt.tile_modes, out_vars);
        tr.tile_modes_excluded = parse_int_list(rt.tile_modes_excluded, out_vars);

        if (tr.width.empty() && tr.height.empty() && tr.depth.empty() && tr.formats.empty() &&
            tr.tile_modes.empty()) {
            continue;
        }

        auto eval_i32 = [&](const std::string& expr, int32_t& dst) {
            if (expr.empty()) return;
            double v = 0.0;
            if (mods::eval_expr(expr, out_vars, &v, true)) {
                dst = static_cast<int32_t>(std::llround(v));
            }
        };
        eval_i32(rt.overwrite_width, tr.overwrite_width);
        eval_i32(rt.overwrite_height, tr.overwrite_height);
        eval_i32(rt.overwrite_depth, tr.overwrite_depth);
        eval_i32(rt.overwrite_format, tr.overwrite_format);
        eval_i32(rt.overwrite_anisotropy, tr.overwrite_anisotropy);
        if (!rt.overwrite_lod_bias.empty()) {
            double v = 0.0;
            if (mods::eval_expr(rt.overwrite_lod_bias, out_vars, &v, true)) {
                tr.has_lod_bias = true;
                tr.overwrite_lod_bias = static_cast<float>(v);
            }
        }
        out_tex_rules.push_back(std::move(tr));
    }
}

// ============================================================================
// 3. Smart Mod Root Detection (Direct BotW folders, Nested wrappers, BCML)
// ============================================================================
bool is_botw_content_dir_name(std::string_view name) {
    static const char* kDirs[] = {
        "Actor",   "Animal",  "Camera",  "Demo",    "Effect",  "Event",   "Font",
        "Game",    "Layout",  "Mals",    "Map",     "Message", "Model",   "Movie",
        "NavMesh", "Pack",    "Physics", "Quest",   "Rumble",  "Shader",  "Sound",
        "System",  "Terrain", "Tips",    "Tracker", "UI",      "Vibration", "Voice",
    };
    for (const char* d : kDirs) {
        if (iequals(name, d)) return true;
    }
    return false;
}

bool is_patch_filename(std::string_view fname) {
    std::string low = lower_ascii(fname);
    if (low == "patches.txt") return true;
    if (low.size() > 6 && low.rfind("patch_", 0) == 0 && low.size() >= 10 &&
        low.compare(low.size() - 4, 4, ".txt") == 0) {
        return true;
    }
    return false;
}

bool parse_shader_filename(std::string_view fname, uint64_t& base_hash, uint64_t& aux_hash,
                           bool& has_aux, mods::ShaderStage& stage) {
    std::string low = lower_ascii(fname);
    std::string_view stem = low;
    if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".txt") {
        stem.remove_suffix(4);
    } else if (stem.size() > 5 && stem.substr(stem.size() - 5) == ".glsl") {
        stem.remove_suffix(5);
    } else {
        return false;
    }

    if (stem.size() > 3 && stem.substr(stem.size() - 3) == "_vs") {
        stage = mods::ShaderStage::Vertex;
        stem.remove_suffix(3);
    } else if (stem.size() > 3 && stem.substr(stem.size() - 3) == "_gs") {
        stage = mods::ShaderStage::Geometry;
        stem.remove_suffix(3);
    } else if (stem.size() > 3 && stem.substr(stem.size() - 3) == "_ps") {
        stage = mods::ShaderStage::Pixel;
        stem.remove_suffix(3);
    } else {
        return false;
    }

    if (stem.substr(0, 7) == "shader_") stem.remove_prefix(7);
    if (stem.empty()) return false;

    auto parse_hex64 = [](std::string_view s, uint64_t& out) -> bool {
        if (s.substr(0, 2) == "0x") s.remove_prefix(2);
        if (s.empty() || s.size() > 16) return false;
        uint64_t v = 0;
        for (char c : s) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            uint64_t d = (c >= '0' && c <= '9') ? (c - '0')
                       : (c >= 'a' && c <= 'f') ? (c - 'a' + 10)
                                                : (c - 'A' + 10);
            v = (v << 4) | d;
        }
        out = v;
        return true;
    };

    size_t under = stem.find('_');
    if (under != std::string_view::npos) {
        has_aux = true;
        return parse_hex64(stem.substr(0, under), base_hash) &&
               parse_hex64(stem.substr(under + 1), aux_hash);
    }
    has_aux = false;
    aux_hash = 0;
    return parse_hex64(stem, base_hash);
}

struct ResolvedModLayout {
    fs::path effective_root;
    fs::path rules_path;
    fs::path content_dir;
    std::vector<std::pair<std::string, fs::path>> direct_content_dirs;
    fs::path aoc_dir;
    std::vector<fs::path> patch_files;
    std::vector<fs::path> shader_files;
    bool valid = false;
};

bool inspect_mod_dir_direct(const fs::path& dir, ResolvedModLayout& out) {
    out = ResolvedModLayout{};
    out.effective_root = dir;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;

    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        std::string name = path_to_utf8(e.path().filename());
        if (name.empty() || name[0] == '.') continue;
        std::error_code ec2;
        if (e.is_directory(ec2)) {
            if (iequals(name, "content")) {
                out.content_dir = e.path();
            } else if (iequals(name, "aoc")) {
                out.aoc_dir = e.path();
            } else if (iequals(name, "0010") && out.aoc_dir.empty()) {
                out.aoc_dir = dir;
            } else if (iequals(name, "shaders")) {
                for (const fs::directory_entry& se : fs::directory_iterator(e.path(), ec2)) {
                    if (!se.is_regular_file(ec2)) continue;
                    std::string sname = path_to_utf8(se.path().filename());
                    uint64_t bh = 0, ah = 0;
                    bool ha = false;
                    mods::ShaderStage st;
                    if (parse_shader_filename(sname, bh, ah, ha, st)) {
                        out.shader_files.push_back(se.path());
                    }
                }
            } else if (is_botw_content_dir_name(name)) {
                out.direct_content_dirs.emplace_back(name, e.path());
            }
        } else if (e.is_regular_file(ec2)) {
            if (iequals(name, "rules.txt")) {
                out.rules_path = e.path();
            } else if (is_patch_filename(name)) {
                out.patch_files.push_back(e.path());
            } else {
                uint64_t bh = 0, ah = 0;
                bool ha = false;
                mods::ShaderStage st;
                if (parse_shader_filename(name, bh, ah, ha, st)) {
                    out.shader_files.push_back(e.path());
                }
            }
        }
    }

    std::sort(out.patch_files.begin(), out.patch_files.end());
    std::sort(out.shader_files.begin(), out.shader_files.end());
    std::sort(out.direct_content_dirs.begin(), out.direct_content_dirs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    out.valid = !out.content_dir.empty() || !out.direct_content_dirs.empty() ||
                !out.aoc_dir.empty() || !out.rules_path.empty() || !out.patch_files.empty() ||
                !out.shader_files.empty();
    return out.valid;
}

ResolvedModLayout resolve_mod_layout(const fs::path& mod_root) {
    ResolvedModLayout layout;
    if (inspect_mod_dir_direct(mod_root, layout)) return layout;

    fs::path cur = mod_root;
    for (int depth = 0; depth < 2; ++depth) {
        std::error_code ec;
        fs::path single_subdir;
        int subdir_count = 0;
        int file_count = 0;
        for (const fs::directory_entry& e : fs::directory_iterator(cur, ec)) {
            std::string name = path_to_utf8(e.path().filename());
            if (name.empty() || name[0] == '.' || iequals(name, "__MACOSX")) continue;
            std::error_code ec2;
            if (e.is_directory(ec2)) {
                single_subdir = e.path();
                subdir_count++;
            } else if (e.is_regular_file(ec2)) {
                std::string low = lower_ascii(name);
                if (low.find("readme") != std::string::npos || low.ends_with(".md") ||
                    low.ends_with(".url") || low.ends_with(".json"))
                    continue;
                file_count++;
            }
        }
        if (subdir_count == 1 && file_count == 0) {
            cur = single_subdir;
            if (inspect_mod_dir_direct(cur, layout)) return layout;
        } else {
            break;
        }
    }
    return layout;
}

std::string substitute_shader_vars(std::string_view src,
                                   const std::vector<std::pair<std::string, double>>& vars) {
    std::string out(src);
    std::vector<std::pair<std::string, double>> sorted_vars = vars;
    std::sort(sorted_vars.begin(), sorted_vars.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    for (const auto& [name, val] : sorted_vars) {
        std::string token = "$" + name;
        char val_buf[64];
        if (std::fabs(val - std::round(val)) < 1e-9) {
            std::snprintf(val_buf, sizeof(val_buf), "%lld", static_cast<long long>(std::llround(val)));
        } else {
            std::snprintf(val_buf, sizeof(val_buf), "%.8g", val);
        }
        std::string repl(val_buf);

        for (const std::string& pat : {token + ":int", token + ":float", token}) {
            size_t pos = 0;
            while ((pos = out.find(pat, pos)) != std::string::npos) {
                out.replace(pos, pat.size(), repl);
                pos += repl.size();
            }
        }
    }
    return out;
}

void load_custom_shaders_for_mod(const ResolvedModLayout& layout,
                                 const std::vector<std::pair<std::string, double>>& vars,
                                 std::vector<mods::CustomShader>& out_shaders) {
    out_shaders.clear();
    for (const fs::path& sp : layout.shader_files) {
        std::string fname = path_to_utf8(sp.filename());
        uint64_t base_hash = 0, aux_hash = 0;
        bool has_aux = false;
        mods::ShaderStage stage = mods::ShaderStage::Pixel;
        if (!parse_shader_filename(fname, base_hash, aux_hash, has_aux, stage)) continue;

        std::string raw_src;
        if (!read_file_utf8(sp, raw_src) || raw_src.empty()) continue;

        mods::CustomShader cs;
        cs.base_hash = base_hash;
        cs.aux_hash = aux_hash;
        cs.has_aux_hash = has_aux;
        cs.stage = stage;
        cs.host_path = path_to_utf8(sp);
        cs.source = substitute_shader_vars(raw_src, vars);
        out_shaders.push_back(std::move(cs));
    }
}
// ============================================================================
// 4. patches.txt / patch_*.txt Parser & Applicator
// ============================================================================
struct PatchWrite {
    uint32_t addr = 0;
    std::vector<uint8_t> bytes;
    bool is_code_instr = false;
    bool is_nop = false;
    bool is_blr = false;
    bool is_li_r3 = false;
    int32_t li_r3_imm = 0;
    bool is_b = false;
    uint32_t b_target = 0;
};

bool parse_reg_num(std::string_view tok, int& reg_out) {
    std::string s = lower_ascii(trim(tok));
    if (s.empty()) return false;
    if (s[0] == 'r') s.erase(0, 1);
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    int r = std::atoi(s.c_str());
    if (r < 0 || r > 31) return false;
    reg_out = r;
    return true;
}

void append_be16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void append_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void append_be64(std::vector<uint8_t>& buf, uint64_t v) {
    append_be32(buf, static_cast<uint32_t>(v >> 32));
    append_be32(buf, static_cast<uint32_t>(v & 0xFFFFFFFFu));
}

bool module_matches_botw(std::string_view val) {
    std::string low = lower_ascii(trim(val));
    if (low.empty()) return true;
    // BotW v1.5.0 / v208 CRC is 0x6fd41a61
    return low.find("0x6fd41a61") != std::string::npos || low.find("6fd41a61") != std::string::npos;
}

bool parse_and_encode_patch_rhs(uint32_t addr, std::string_view rhs_raw,
                                const std::vector<std::pair<std::string, double>>& vars,
                                PatchWrite& out) {
    out = PatchWrite{};
    out.addr = addr;
    std::string rhs = trim(rhs_raw);
    if (rhs.empty()) return false;

    // Strip optional prefix like "bla " or "code " if someone wrote "0x... = bla 0x..."
    std::string low = lower_ascii(rhs);

    auto eval_num = [&](std::string_view expr, double& val_out) -> bool {
        return mods::eval_expr(expr, vars, &val_out, true);
    };

    if (low.starts_with(".float ") || low.starts_with(".float\t")) {
        double v = 0.0;
        if (!eval_num(rhs.substr(7), v)) return false;
        float fv = static_cast<float>(v);
        uint32_t bits = 0;
        std::memcpy(&bits, &fv, sizeof(bits));
        append_be32(out.bytes, bits);
        return true;
    }
    if (low.starts_with(".double ") || low.starts_with(".double\t")) {
        double v = 0.0;
        if (!eval_num(rhs.substr(8), v)) return false;
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        append_be64(out.bytes, bits);
        return true;
    }
    if (low.starts_with(".int ") || low.starts_with(".int\t") ||
        low.starts_with(".uint ") || low.starts_with(".uint\t") ||
        low.starts_with(".word ") || low.starts_with(".word\t")) {
        size_t sp = rhs.find_first_of(" \t");
        double v = 0.0;
        if (!eval_num(rhs.substr(sp + 1), v)) return false;
        int64_t iv = std::llround(v);
        append_be32(out.bytes, static_cast<uint32_t>(iv));
        return true;
    }
    if (low.starts_with(".short ") || low.starts_with(".short\t") ||
        low.starts_with(".ushort ") || low.starts_with(".ushort\t") ||
        low.starts_with(".half ") || low.starts_with(".half\t")) {
        size_t sp = rhs.find_first_of(" \t");
        double v = 0.0;
        if (!eval_num(rhs.substr(sp + 1), v)) return false;
        int64_t iv = std::llround(v);
        append_be16(out.bytes, static_cast<uint16_t>(iv));
        return true;
    }
    if (low.starts_with(".byte ") || low.starts_with(".byte\t") ||
        low.starts_with(".ubyte ") || low.starts_with(".ubyte\t")) {
        size_t sp = rhs.find_first_of(" \t");
        double v = 0.0;
        if (!eval_num(rhs.substr(sp + 1), v)) return false;
        int64_t iv = std::llround(v);
        out.bytes.push_back(static_cast<uint8_t>(iv & 0xFF));
        return true;
    }
    if (low.starts_with(".string ") || low.starts_with(".string\t") ||
        low.starts_with(".asciiz ") || low.starts_with(".asciiz\t") ||
        low.starts_with(".ascii ") || low.starts_with(".ascii\t")) {
        bool nul_term = !low.starts_with(".ascii ") && !low.starts_with(".ascii\t");
        size_t q1 = rhs.find('"');
        size_t q2 = rhs.rfind('"');
        if (q1 == std::string::npos || q2 == std::string::npos || q2 <= q1) return false;
        std::string_view body(rhs.data() + q1 + 1, q2 - q1 - 1);
        for (size_t i = 0; i < body.size(); ++i) {
            char c = body[i];
            if (c == '\\' && i + 1 < body.size()) {
                char n = body[++i];
                if (n == 'n') out.bytes.push_back('\n');
                else if (n == 'r') out.bytes.push_back('\r');
                else if (n == 't') out.bytes.push_back('\t');
                else if (n == '0') out.bytes.push_back('\0');
                else out.bytes.push_back(static_cast<uint8_t>(n));
            } else {
                out.bytes.push_back(static_cast<uint8_t>(c));
            }
        }
        if (nul_term) out.bytes.push_back(0);
        return true;
    }

    // PPC instructions: nop, blr, li rD, imm, lis rD, imm, b target, bl target
    if (low == "nop") {
        append_be32(out.bytes, 0x60000000u);
        out.is_code_instr = true;
        out.is_nop = true;
        return true;
    }
    if (low == "blr") {
        append_be32(out.bytes, 0x4E800020u);
        out.is_code_instr = true;
        out.is_blr = true;
        return true;
    }
    if (low.starts_with("li ") || low.starts_with("li\t") ||
        low.starts_with("lis ") || low.starts_with("lis\t")) {
        bool is_lis = low.starts_with("lis");
        std::string_view args = std::string_view(rhs).substr(is_lis ? 3 : 2);
        size_t comma = args.find(',');
        if (comma == std::string_view::npos) return false;
        int rd = 0;
        if (!parse_reg_num(args.substr(0, comma), rd)) return false;
        double imm_d = 0.0;
        if (!eval_num(args.substr(comma + 1), imm_d)) return false;
        int32_t imm32 = static_cast<int32_t>(std::llround(imm_d));
        uint16_t simm = static_cast<uint16_t>(imm32 & 0xFFFF);
        uint32_t opcode = is_lis ? 15u : 14u;
        uint32_t insn = (opcode << 26) | (static_cast<uint32_t>(rd) << 21) | simm;
        append_be32(out.bytes, insn);
        out.is_code_instr = true;
        if (!is_lis && rd == 3) {
            out.is_li_r3 = true;
            out.li_r3_imm = static_cast<int16_t>(simm);
        }
        return true;
    }
    if (low.starts_with("b ") || low.starts_with("b\t") ||
        low.starts_with("bl ") || low.starts_with("bl\t")) {
        bool is_bl = low.starts_with("bl");
        std::string_view arg = trim(std::string_view(rhs).substr(is_bl ? 2 : 1));
        double target_d = 0.0;
        if (!eval_num(arg, target_d)) return false;
        uint32_t target = static_cast<uint32_t>(std::llround(target_d));
        int64_t diff = static_cast<int64_t>(target) - static_cast<int64_t>(addr);
        uint32_t li_field = static_cast<uint32_t>(diff) & 0x03FFFFFCu;
        uint32_t insn = (18u << 26) | li_field | (is_bl ? 1u : 0u);
        append_be32(out.bytes, insn);
        out.is_code_instr = true;
        if (!is_bl) {
            out.is_b = true;
            out.b_target = target;
        }
        return true;
    }

    // Raw 32-bit hex or arithmetic expression (e.g. 0xdeadbeef)
    double val = 0.0;
    if (eval_num(rhs, val)) {
        uint32_t u32 = static_cast<uint32_t>(static_cast<int64_t>(std::llround(val)));
        append_be32(out.bytes, u32);
        if (u32 == 0x4E800020u) {
            out.is_code_instr = true;
            out.is_blr = true;
        } else if (u32 == 0x60000000u) {
            out.is_code_instr = true;
            out.is_nop = true;
        }
        return true;
    }

    return false;
}

void parse_patch_file(const fs::path& path,
                      const std::vector<std::pair<std::string, double>>& preset_vars,
                      std::vector<PatchWrite>& out_writes) {
    std::string content;
    if (!read_file_utf8(path, content)) return;

    std::vector<std::pair<std::string, double>> vars = preset_vars;
    std::istringstream f(content);
    std::string line;
    uint32_t cur_origin = 0;
    bool has_origin = false;

    // Two-pass over lines in each section so forward symbol references (_sym = 0x...) resolve
    struct SectionBlock {
        bool matches = true;
        std::vector<std::string> lines;
    };
    std::vector<SectionBlock> blocks;
    blocks.push_back(SectionBlock{true, {}});

    while (std::getline(f, line)) {
        std::string s = trim(strip_comment(line));
        if (s.empty()) continue;
        if (s.front() == '[' && s.back() == ']') {
            blocks.push_back(SectionBlock{true, {}});
            continue;
        }
        size_t eq = s.find('=');
        if (eq != std::string::npos) {
            std::string lhs = lower_ascii(trim(std::string_view(s).substr(0, eq)));
            std::string rhs = trim(std::string_view(s).substr(eq + 1));
            if (lhs == "modulematches") {
                blocks.back().matches = module_matches_botw(rhs);
                continue;
            }
        }
        blocks.back().lines.push_back(std::move(s));
    }

    for (const auto& blk : blocks) {
        if (!blk.matches) continue;
        // Pass 1: collect symbol definitions (_symName = expr or $var = expr)
        for (const std::string& s : blk.lines) {
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string lhs = trim(std::string_view(s).substr(0, eq));
            std::string rhs = trim(std::string_view(s).substr(eq + 1));
            if (lhs.empty()) continue;
            if (lhs[0] == '_' || lhs[0] == '$') {
                double v = 0.0;
                if (mods::eval_expr(rhs, vars, &v, true)) {
                    set_var(vars, lhs, v);
                }
            }
        }

        // Pass 2: parse origin and patch writes
        cur_origin = 0;
        has_origin = false;
        for (const std::string& s : blk.lines) {
            // Check "origin = 0x..."
            size_t eq = s.find('=');
            if (eq != std::string::npos) {
                std::string lhs = trim(std::string_view(s).substr(0, eq));
                std::string rhs = trim(std::string_view(s).substr(eq + 1));
                std::string low_lhs = lower_ascii(lhs);
                if (low_lhs == "origin") {
                    double v = 0.0;
                    if (mods::eval_expr(rhs, vars, &v, true)) {
                        cur_origin = static_cast<uint32_t>(std::llround(v));
                        has_origin = true;
                    }
                    continue;
                }
                if (!lhs.empty() && (lhs[0] == '_' || lhs[0] == '$')) {
                    double v = 0.0;
                    if (mods::eval_expr(rhs, vars, &v, true)) {
                        set_var(vars, lhs, v);
                    }
                    continue;
                }
                // Address assignment: 0xXXXXXXXX = <directive or instruction>
                double addr_d = 0.0;
                if (mods::eval_expr(lhs, vars, &addr_d, false)) {
                    uint32_t addr = static_cast<uint32_t>(std::llround(addr_d));
                    PatchWrite pw;
                    if (parse_and_encode_patch_rhs(addr, rhs, vars, pw)) {
                        cur_origin = addr + static_cast<uint32_t>(pw.bytes.size());
                        has_origin = true;
                        out_writes.push_back(std::move(pw));
                    }
                }
            } else if (has_origin) {
                // Sequential instruction/directive under an origin
                if (s.back() == ':') {
                    // Label definition at current origin
                    std::string lbl = trim(std::string_view(s).substr(0, s.size() - 1));
                    if (!lbl.empty()) set_var(vars, lbl, static_cast<double>(cur_origin));
                    continue;
                }
                PatchWrite pw;
                if (parse_and_encode_patch_rhs(cur_origin, s, vars, pw)) {
                    cur_origin += static_cast<uint32_t>(pw.bytes.size());
                    out_writes.push_back(std::move(pw));
                }
            }
        }
    }
}

void write_patched_bytes(uint32_t addr, const std::vector<uint8_t>& bytes,
                         std::unordered_set<uint32_t>& saved_addrs) {
    if (!g_patch_backend.write_u8) return;
    for (size_t i = 0; i < bytes.size(); ++i) {
        uint32_t a = addr + static_cast<uint32_t>(i);
        if (g_patch_backend.read_u8 && saved_addrs.insert(a).second) {
            g_saved_patch_bytes.push_back(SavedByte{a, g_patch_backend.read_u8(a)});
        }
        g_patch_backend.write_u8(a, bytes[i]);
    }
}

void restore_previous_patches_locked() {
    if (g_patch_backend.write_u8) {
        for (auto it = g_saved_patch_bytes.rbegin(); it != g_saved_patch_bytes.rend(); ++it) {
            g_patch_backend.write_u8(it->addr, it->orig);
        }
    }
    g_saved_patch_bytes.clear();
    if (g_patch_backend.restore_fn_overrides) {
        g_patch_backend.restore_fn_overrides();
    }
}

void apply_patches_locked() {
    restore_previous_patches_locked();
    if (!g_patch_backend.write_u8) return;

    std::unordered_set<uint32_t> saved_addrs;
    constexpr uint32_t kCodeStart = 0x02000000u;
    constexpr uint32_t kCodeEnd = 0x04000000u;

    for (auto& mod : g_mods) {
        mod.code_patches_applied = 0;
        mod.data_patches_applied = 0;
        mod.mid_fn_patches_skipped = 0;

        ResolvedModLayout layout = resolve_mod_layout(utf8_to_path(mod.root_path));
        if (layout.patch_files.empty()) continue;

        std::vector<PatchWrite> writes;
        for (const fs::path& pf : layout.patch_files) {
            parse_patch_file(pf, mod.evaluated_vars, writes);
        }

        for (size_t i = 0; i < writes.size(); ++i) {
            const PatchWrite& pw = writes[i];
            if (pw.bytes.empty()) continue;

            // Always write the big-endian bytes to guest memory (for rodata/data and code inspection)
            write_patched_bytes(pw.addr, pw.bytes, saved_addrs);

            if (pw.addr >= kCodeStart && pw.addr < kCodeEnd) {
                bool at_entry = g_patch_backend.is_fn_entry && g_patch_backend.is_fn_entry(pw.addr);
                if (at_entry) {
                    // Check for 2-instruction sequence: `li r3, imm` at entry followed by `blr` at entry + 4
                    if (pw.is_li_r3 && i + 1 < writes.size() &&
                        writes[i + 1].addr == pw.addr + 4 && writes[i + 1].is_blr) {
                        write_patched_bytes(writes[i + 1].addr, writes[i + 1].bytes, saved_addrs);
                        if (g_patch_backend.override_ret_imm) {
                            g_patch_backend.override_ret_imm(
                                pw.addr, static_cast<uint32_t>(writes[i].li_r3_imm));
                        }
                        mod.code_patches_applied++;
                        i++;  // Consumed the following blr
                        continue;
                    }
                    if (pw.is_blr || pw.is_nop) {
                        if (pw.is_blr && g_patch_backend.override_nop) {
                            g_patch_backend.override_nop(pw.addr);
                        }
                        mod.code_patches_applied++;
                        continue;
                    }
                    if (pw.is_b && g_patch_backend.redirect_fn) {
                        if (g_patch_backend.redirect_fn(pw.addr, pw.b_target)) {
                            mod.code_patches_applied++;
                            continue;
                        }
                    }
                }
                mod.mid_fn_patches_skipped++;
            } else {
                mod.data_patches_applied++;
            }
        }
    }

    // Sync counters into g_discovered as well
    std::lock_guard<std::mutex> dlk(g_disc_mu);
    for (auto& dm : g_discovered) {
        dm.code_patches_applied = 0;
        dm.data_patches_applied = 0;
        dm.mid_fn_patches_skipped = 0;
        for (const auto& m : g_mods) {
            if (m.folder == dm.folder) {
                dm.code_patches_applied = m.code_patches_applied;
                dm.data_patches_applied = m.data_patches_applied;
                dm.mid_fn_patches_skipped = m.mid_fn_patches_skipped;
                break;
            }
        }
    }
}

}  // namespace

namespace {


// ============================================================================
// 6. Built-in .zip Extractor (via zlib raw inflate) + Folder Copy for Installer
// ============================================================================
uint16_t rd_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t rd_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool extract_zip_utf8(const fs::path& zip_path, const fs::path& dest_dir, std::string& err) {
    std::string data_str;
    if (!read_file_utf8(zip_path, data_str)) {
        err = "Could not open archive: " + path_to_utf8(zip_path);
        return false;
    }
    const auto* data = reinterpret_cast<const uint8_t*>(data_str.data());
    size_t sz = data_str.size();
    if (sz < 22) {
        err = "File is too small to be a .zip archive.";
        return false;
    }

    size_t max_back = std::min<size_t>(sz, 65557);
    size_t eocd = sz;
    for (size_t i = sz - 22; i + 22 <= sz; --i) {
        if (rd_le32(data + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
        if (sz - i >= max_back || i == 0) break;
    }
    if (eocd == sz) {
        err = "Not a valid .zip archive (missing central directory).";
        return false;
    }

    uint16_t entries = rd_le16(data + eocd + 10);
    uint32_t cd_size = rd_le32(data + eocd + 12);
    uint32_t cd_off = rd_le32(data + eocd + 16);
    if (static_cast<uint64_t>(cd_off) + cd_size > sz) {
        err = "Corrupt .zip central directory.";
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    fs::path canon_dest = fs::weakly_canonical(dest_dir, ec);

    size_t pos = cd_off;
    for (uint16_t i = 0; i < entries && pos + 46 <= sz; ++i) {
        if (rd_le32(data + pos) != 0x02014b50u) {
            err = "Corrupt .zip central directory entry.";
            return false;
        }
        uint16_t flags = rd_le16(data + pos + 8);
        uint16_t method = rd_le16(data + pos + 10);
        uint32_t comp_sz = rd_le32(data + pos + 20);
        uint32_t uncomp_sz = rd_le32(data + pos + 24);
        uint16_t name_len = rd_le16(data + pos + 28);
        uint16_t extra_len = rd_le16(data + pos + 30);
        uint16_t comment_len = rd_le16(data + pos + 32);
        uint32_t lhoff = rd_le32(data + pos + 42);

        if (pos + 46 + name_len + extra_len + comment_len > sz) {
            err = "Truncated .zip central directory entry.";
            return false;
        }
        std::string raw_name(reinterpret_cast<const char*>(data + pos + 46), name_len);
        pos += 46 + name_len + extra_len + comment_len;

        if (flags & 1) {
            err = "Encrypted .zip archives are not supported.";
            return false;
        }
        for (char& ch : raw_name) {
            if (ch == '\\') ch = '/';
        }
        bool is_dir = !raw_name.empty() && raw_name.back() == '/';
        fs::path rel = utf8_to_path(raw_name).lexically_normal();
        if (rel.empty() || rel.is_absolute()) continue;
        bool bad_seg = false;
        for (const auto& seg : rel) {
            std::string s = path_to_utf8(seg);
            if (s == ".." || s.find(':') != std::string::npos) bad_seg = true;
        }
        if (bad_seg) continue;

        fs::path out_path = dest_dir / rel;
        fs::path canon_out = fs::weakly_canonical(out_path, ec);
        if (!ec && !canon_dest.empty()) {
            auto m = std::mismatch(canon_dest.begin(), canon_dest.end(), canon_out.begin(),
                                   canon_out.end());
            if (m.first != canon_dest.end()) continue;
        }
        if (is_dir) {
            fs::create_directories(out_path, ec);
            continue;
        }
        if (lhoff + 30 > sz || rd_le32(data + lhoff) != 0x04034b50u) {
            err = "Corrupt .zip local file header.";
            return false;
        }
        uint16_t lname = rd_le16(data + lhoff + 26);
        uint16_t lextra = rd_le16(data + lhoff + 28);
        size_t data_off = static_cast<size_t>(lhoff) + 30 + lname + lextra;
        if (data_off + comp_sz > sz) {
            err = "Truncated .zip file payload.";
            return false;
        }

        fs::create_directories(out_path.parent_path(), ec);
        FILE* wf = mods::fopen_utf8(path_to_utf8(out_path), "wb");
        if (!wf) {
            err = "Could not create extracted file: " + path_to_utf8(out_path);
            return false;
        }

        const uint8_t* src = data + data_off;
        bool write_ok = true;
        if (method == 0) {
            if (comp_sz != uncomp_sz) {
                fclose(wf);
                err = "Stored .zip entry size mismatch.";
                return false;
            }
            if (uncomp_sz > 0 && fwrite(src, 1, uncomp_sz, wf) != uncomp_sz) write_ok = false;
        } else if (method == 8) {
            z_stream zs{};
            zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
            zs.avail_in = comp_sz;
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
                fclose(wf);
                err = "zlib inflateInit2 failed.";
                return false;
            }
            uint8_t buf[65536];
            int ret = Z_OK;
            while (ret == Z_OK) {
                zs.next_out = buf;
                zs.avail_out = sizeof(buf);
                ret = inflate(&zs, Z_NO_FLUSH);
                size_t have = sizeof(buf) - zs.avail_out;
                if (have > 0 && fwrite(buf, 1, have, wf) != have) {
                    write_ok = false;
                    break;
                }
            }
            inflateEnd(&zs);
            if (ret != Z_STREAM_END) {
                fclose(wf);
                err = "Corrupt deflate stream in .zip.";
                return false;
            }
        } else {
            fclose(wf);
            err = "Unsupported .zip compression method.";
            return false;
        }
        fclose(wf);
        if (!write_ok) {
            err = "Failed writing extracted file: " + path_to_utf8(out_path);
            return false;
        }
    }
    return true;
}

// ============================================================================
// 5. VFS Overlay Tree, Case-Insensitive Resolution & INI Persistence
// ============================================================================
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
        const std::string& name = parts[1];
        if (name.size() == 3 + 16) {
            for (size_t i = 3; i < name.size(); i++)
                if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return v;
        } else if (name.size() != 3) {
            return v;
        }
        v.id = GuestVol::Id::Aoc;
        v.rel = join_from(2);
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
    if (parts.empty()) {
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
        for (fs::directory_iterator it(utf8_to_path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (it->is_symlink(sec)) continue;
            std::string name = path_to_utf8(it->path().filename());
            if (!name.empty()) cached.emplace(lower_ascii(name), name);
        }
        cached.emplace("", "");
    }
    auto it = cached.find(want_lower);
    if (it == cached.end()) return {};
    return it->second;
}

std::string case_path(const std::string& base, const std::string& rel) {
    if (base.empty()) return {};
    fs::path exact = utf8_to_path(base);
    auto parts = split_rel(rel);
    for (const auto& part : parts) exact /= utf8_to_path(part);
    std::error_code ec;
    auto st = fs::status(exact, ec);
    if (!ec && (fs::is_regular_file(st) || fs::is_directory(st))) return path_to_utf8(exact);
    if (rel.empty()) return {};
    std::string cur = base;
    for (const auto& part : parts) {
        fs::path next = utf8_to_path(cur) / utf8_to_path(part);
        st = fs::status(next, ec);
        if (!ec && (fs::is_regular_file(st) || fs::is_directory(st))) {
            cur = path_to_utf8(next);
            continue;
        }
        std::string actual = lookup_component(cur, lower_ascii(part));
        if (actual.empty()) return {};
        cur = path_to_utf8(utf8_to_path(cur) / utf8_to_path(actual));
    }
    return cur;
}

bool is_file(const std::string& host, uint64_t& size) {
    if (host.empty()) return false;
    std::error_code ec;
    fs::path p = utf8_to_path(host);
    auto st = fs::status(p, ec);
    if (ec || !fs::is_regular_file(st)) return false;
    auto n = fs::file_size(p, ec);
    if (ec) return false;
    size = static_cast<uint64_t>(n);
    return true;
}

bool is_dir(const std::string& host) {
    if (host.empty()) return false;
    std::error_code ec;
    return fs::is_directory(utf8_to_path(host), ec);
}

fs::path child_ci(const fs::path& dir, const std::string& want) {
    std::error_code ec;
    if (fs::is_directory(dir / want, ec)) return dir / want;
    std::string lw = lower_ascii(want);
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec)) continue;
        if (lower_ascii(path_to_utf8(it->path().filename())) == lw) return it->path();
    }
    return {};
}

std::string owner_name(int owner) {
    if (owner < 0 || owner >= static_cast<int>(g_mods.size())) return "user DLC folder";
    return g_mods[owner].folder;
}

void insert_file(Node& root, const std::string& rel, const std::string& host, int owner, bool base, const char* vol) {
    auto parts = split_rel(rel);
    if (parts.empty()) return;
    Node* n = &root;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        std::string key = lower_ascii(parts[i]);
        Node& child = n->kids[key];
        if (child.name.empty()) {
            child.name = parts[i];
            child.owner = owner;
        }
        if (child.file) {
            if (child.owner != kUserDlc && child.owner != owner)
                g_conflicts.push_back({std::string(vol) + "/" + rel, child.owner, owner});
            child.file = false;
            child.host.clear();
        }
        n = &child;
    }
    std::string key = lower_ascii(parts.back());
    Node& leaf = n->kids[key];
    if ((leaf.file || !leaf.kids.empty()) && leaf.owner != kUserDlc && leaf.owner != owner)
        g_conflicts.push_back({std::string(vol) + "/" + rel, leaf.owner, owner});
    leaf.name = parts.back();
    leaf.host = host;
    leaf.file = true;
    leaf.base = base;
    leaf.owner = owner;
    leaf.kids.clear();
}

size_t add_tree(Node& root, const fs::path& host_root, const std::string& base, int owner, const char* vol,
                size_t& replaces, const std::string& prefix = "") {
    std::error_code ec;
    if (host_root.empty() || !fs::is_directory(host_root, ec)) return 0;
    size_t nfiles = 0, skipped = 0;
    fs::recursive_directory_iterator it(host_root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_symlink(sec)) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(sec)) continue;
        std::string host = path_to_utf8(it->path());
        std::string generic;
        try {
            fs::path rel_p = it->path().lexically_relative(host_root);
            std::u8string u8 = rel_p.generic_u8string();
            generic = std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        } catch (...) {
            generic.clear();
        }
        if (host.empty() || generic.empty() || generic.rfind("..", 0) == 0) {
            skipped++;
            continue;
        }
        std::string full_rel = prefix.empty() ? generic : (prefix + "/" + generic);
        uint64_t sz = 0;
        bool in_base = is_file(case_path(base, full_rel), sz);
        if (in_base) replaces++;
        insert_file(root, full_rel, host, owner, in_base, vol);
        nfiles++;
    }
    if (skipped) LOG("mods: %llu files under %s skipped (name not readable)", static_cast<unsigned long long>(skipped), path_to_utf8(host_root).c_str());
    return nfiles;
}

uint32_t count_regular_files(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return 0;
    uint32_t n = 0;
    for (const fs::directory_entry& e :
         fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code st_ec;
        if (e.is_regular_file(st_ec) && !path_to_utf8(e.path()).empty()) n++;
    }
    return n;
}

uint32_t count_resolved_content_files(const ResolvedModLayout& layout) {
    uint32_t total = 0;
    if (!layout.content_dir.empty()) total += count_regular_files(layout.content_dir);
    for (const auto& [_, sub_path] : layout.direct_content_dirs) {
        total += count_regular_files(sub_path);
    }
    return total;
}

fs::path aoc_content_root(const fs::path& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return {};
    fs::path content = child_ci(folder, "content");
    if (!content.empty() && !child_ci(content, "0010").empty()) return content;
    if (!child_ci(folder, "0010").empty()) return folder;
    if (!content.empty()) return content;
    return folder;
}

void tally(const Node& node) {
    for (const auto& [key, child] : node.kids) {
        if (child.file) {
            g_files++;
            if (child.base) g_replacing++;
            else g_added++;
        } else {
            tally(child);
        }
    }
}

bool hex4(const std::string& s) {
    if (s.size() != 4) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

void collect_owned(const Node& root) {
    for (const auto& [key, child] : root.kids)
        if (!child.file && hex4(key)) g_owned.insert(static_cast<uint16_t>(std::strtoul(key.c_str(), nullptr, 16)));
}

void collect_owned_base(const std::string& base) {
    if (base.empty()) return;
    std::error_code ec;
    for (fs::directory_iterator it(utf8_to_path(base), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec) || it->is_symlink(sec)) continue;
        std::string name = lower_ascii(path_to_utf8(it->path().filename()));
        if (hex4(name)) g_owned.insert(static_cast<uint16_t>(std::strtoul(name.c_str(), nullptr, 16)));
    }
}

uint64_t parse_title(const std::string& game) {
    if (game.empty()) return 0;
    fs::path root = utf8_to_path(game);
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

bool base_nonempty(const std::string& base) {
    if (!is_dir(base)) return false;
    std::error_code ec;
    fs::directory_iterator it(utf8_to_path(base), ec);
    return !ec && it != fs::directory_iterator();
}

std::string base_for(GuestVol::Id id) {
    if (id == GuestVol::Id::Content) return g_content_base;
    if (id == GuestVol::Id::Aoc) return g_aoc_base;
    return {};
}

const Node& tree_for(GuestVol::Id id) { return id == GuestVol::Id::Aoc ? g_aoc : g_content; }

// Read and modify mods.ini preserving sections ([Enabled], [Order], [Presets], etc.)
struct IniSec {
    std::string name;
    std::vector<std::pair<std::string, std::string>> kv;
};

std::vector<IniSec> read_ini_sections(const fs::path& ini_path) {
    std::string text = read_text(ini_path, 256 * 1024);
    std::vector<IniSec> secs;
    auto find_sec = [&](std::string_view n) -> IniSec* {
        std::string want = lower_ascii(n);
        for (auto& s : secs)
            if (lower_ascii(s.name) == want) return &s;
        return nullptr;
    };
    each_ini_line(text, [&](const std::string& sec, const std::string& key, const std::string& val) {
        IniSec* s = find_sec(sec);
        if (!s) {
            std::string title = sec == "enabled"   ? "Enabled"
                              : sec == "order"     ? "Order"
                              : sec == "presets"   ? "Presets"
                                                   : sec;
            secs.push_back({title, {}});
            s = &secs.back();
        }
        s->kv.emplace_back(key, val);
    });
    return secs;
}

IniSec& ensure_ini_section(std::vector<IniSec>& secs, const std::string& title, bool at_front = false) {
    std::string want = lower_ascii(title);
    for (auto& s : secs)
        if (lower_ascii(s.name) == want) return s;
    if (at_front) {
        secs.insert(secs.begin(), {title, {}});
        return secs.front();
    }
    secs.push_back({title, {}});
    return secs.back();
}

bool write_ini_sections(const fs::path& ini_path, const std::vector<IniSec>& secs) {
    std::error_code ec;
    if (ini_path.has_parent_path()) fs::create_directories(ini_path.parent_path(), ec);
    fs::path tmp = ini_path;
    tmp += ".tmp";
    std::ostringstream out;
    bool first = true;
    for (const auto& s : secs) {
        if (!first) out << "\r\n";
        first = false;
        if (!s.name.empty()) out << "[" << s.name << "]\r\n";
        for (const auto& [k, v] : s.kv) out << k << "=" << v << "\r\n";
    }
    std::string body = out.str();
    FILE* f = mods::fopen_utf8(path_to_utf8(tmp), "wb");
    if (!f) return false;
    size_t n = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (n != body.size()) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, ini_path, ec);
    if (ec) {
        fs::remove(ini_path, ec);
        fs::rename(tmp, ini_path, ec);
        if (ec) return false;
    }
    return true;
}

void scan_discovered(const fs::path& user) {
    if (user.empty()) return;
    fs::path mods_dir = user / "mods";
    std::error_code ec;
    std::vector<std::string> present;
    for (fs::directory_iterator it(mods_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec)) continue;
        std::string name = path_to_utf8(it->path().filename());
        if (!name.empty() && name[0] != '.' && valid_folder(name)) present.push_back(name);
    }
    std::sort(present.begin(), present.end(), iless);
    std::string ini = read_text(user / "mods.ini", 256 * 1024);
    mods::Config cfg = mods::parse_config(ini);
    std::vector<std::string> ordered;
    if (cfg.has_order) {
        for (const auto& o : cfg.order)
            for (const auto& p : present)
                if (lower_ascii(p) == lower_ascii(o)) ordered.push_back(p);
        for (const auto& p : present) {
            bool seen = false;
            for (const auto& x : ordered)
                if (lower_ascii(x) == lower_ascii(p)) seen = true;
            if (!seen) ordered.push_back(p);
        }
    } else {
        ordered = present;
    }

    std::vector<mods::DiscoveredMod> list;
    list.reserve(ordered.size());
    for (const std::string& folder : ordered) {
        fs::path dir = mods_dir / utf8_to_path(folder);
        ResolvedModLayout layout = resolve_mod_layout(dir);
        RulesFile rf = !layout.rules_path.empty()
                           ? parse_rules_internal(read_text(layout.rules_path, 64 * 1024))
                           : parse_rules_internal(read_text(dir / "rules.txt", 64 * 1024));

        mods::DiscoveredMod d;
        d.folder = folder;
        d.name = rf.name.empty() ? folder : rf.name;
        d.description = rf.description;
        d.version = rf.version;
        d.root_path = path_to_utf8(dir);
        d.enabled = cfg.is_enabled(folder);
        evaluate_rules_for_mod(folder, rf, cfg, d.presets, d.evaluated_vars, d.texture_rules);
        d.patch_files = static_cast<uint32_t>(layout.patch_files.size());
        d.shader_files = static_cast<uint32_t>(layout.shader_files.size());

        bool matched_loaded = false;
        for (const auto& m : g_mods) {
            if (lower_ascii(m.folder) == lower_ascii(folder)) {
                d.loaded_now = true;
                d.content_files = static_cast<uint32_t>(m.content_files);
                d.aoc_files = static_cast<uint32_t>(m.aoc_files);
                d.overridden_files = m.overridden_files;
                d.overrides_mods = m.overrides_mods;
                d.overridden_by_mods = m.overridden_by_mods;
                d.custom_shaders = m.custom_shaders;
                d.code_patches_applied = m.code_patches_applied;
                d.data_patches_applied = m.data_patches_applied;
                d.mid_fn_patches_skipped = m.mid_fn_patches_skipped;
                matched_loaded = true;
                break;
            }
        }
        if (!matched_loaded) {
            d.content_files = count_resolved_content_files(layout);
            d.aoc_files = count_regular_files(aoc_content_root(layout.aoc_dir));
        }
        list.push_back(std::move(d));
    }

    std::lock_guard<std::mutex> lock(g_disc_mu);
    g_discovered = std::move(list);
    bool diff = false;
    for (const auto& d : g_discovered) {
        if (d.enabled != d.loaded_now) diff = true;
    }
    g_pending_restart = diff;
}

void load_mods(const fs::path& user) {
    fs::path mods_dir = user / "mods";
    std::error_code ec;
    std::vector<std::string> present;
    for (fs::directory_iterator it(mods_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec)) continue;
        std::string name = path_to_utf8(it->path().filename());
        if (!name.empty() && name[0] != '.' && valid_folder(name)) present.push_back(name);
    }
    std::string ini = read_text(user / "mods.ini", 256 * 1024);
    mods::Config cfg = mods::parse_config(ini);
    std::vector<std::string> order = mods::load_order(cfg, present);
    LOG("mods: %llu mod folders in %s, %llu enabled%s", static_cast<unsigned long long>(present.size()), path_to_utf8(mods_dir).c_str(), static_cast<unsigned long long>(order.size()),
        ini.empty() ? " (no mods.ini)" : "");
    for (const auto& e : cfg.enabled) {
        if (!e.enabled) continue;
        bool found = false;
        for (const auto& p : present)
            if (lower_ascii(p) == lower_ascii(e.folder)) found = true;
        if (!found) LOG("mods: %s is enabled in mods.ini but has no folder in mods/", e.folder.c_str());
    }

    for (const std::string& folder : order) {
        fs::path dir = mods_dir / utf8_to_path(folder);
        ResolvedModLayout layout = resolve_mod_layout(dir);
        RulesFile rf = !layout.rules_path.empty()
                           ? parse_rules_internal(read_text(layout.rules_path, 64 * 1024))
                           : parse_rules_internal(read_text(dir / "rules.txt", 64 * 1024));

        mods::ModInfo info;
        info.folder = folder;
        info.name = rf.name.empty() ? folder : rf.name;
        info.description = rf.description;
        info.version = rf.version;
        info.root_path = path_to_utf8(dir);
        evaluate_rules_for_mod(folder, rf, cfg, info.presets, info.evaluated_vars, info.texture_rules);
        if (!layout.shader_files.empty()) {
            load_custom_shaders_for_mod(layout, info.evaluated_vars, info.custom_shaders);
        }
        info.patch_files = static_cast<uint32_t>(layout.patch_files.size());
        info.shader_files = static_cast<uint32_t>(layout.shader_files.size());

        int owner = static_cast<int>(g_mods.size());
        g_mods.push_back(std::move(info));
        mods::ModInfo& m = g_mods.back();

        if (!rf.title_ids.empty()) {
            bool usa = false;
            for (const auto& t : rf.title_ids)
                if (t.find("101c9400") != std::string::npos) usa = true;
            if (!usa) LOG("mods: %s: rules.txt titleIds do not list 00050000101C9400 (BotW USA); loading it anyway", folder.c_str());
        }

        if (!layout.content_dir.empty()) {
            m.content_files += add_tree(g_content, layout.content_dir, g_content_base, owner, "content", m.replaces);
        }
        for (const auto& [sub_name, sub_path] : layout.direct_content_dirs) {
            m.content_files += add_tree(g_content, sub_path, g_content_base, owner, "content", m.replaces, sub_name);
        }
        if (!layout.aoc_dir.empty()) {
            m.aoc_files += add_tree(g_aoc, aoc_content_root(layout.aoc_dir), g_aoc_base, owner, "aoc", m.replaces);
        }

        std::string label = m.name == folder ? folder : m.name + " [" + folder + "]";
        if (!m.version.empty()) label += " version " + m.version;
        size_t total = m.content_files + m.aoc_files;
        LOG("mod %d: %s: %llu files (%llu content, %llu aoc), %llu replace game files, %llu new, %llu tex rules, %u patches, %u shaders",
            owner + 1, label.c_str(), static_cast<unsigned long long>(total), static_cast<unsigned long long>(m.content_files), static_cast<unsigned long long>(m.aoc_files), static_cast<unsigned long long>(m.replaces), static_cast<unsigned long long>(total - m.replaces),
            static_cast<unsigned long long>(m.texture_rules.size()), m.patch_files, m.shader_files);
    }

    std::vector<std::set<int>> overrides_graph(g_mods.size());
    std::vector<std::set<int>> overridden_by_graph(g_mods.size());
    for (const Conflict& c : g_conflicts) {
        if (c.loser >= 0 && c.loser < static_cast<int>(g_mods.size())) {
            g_mods[c.loser].lost++;
            g_mods[c.loser].overridden_files++;
        }
        if (c.winner >= 0 && c.winner < static_cast<int>(g_mods.size()) &&
            c.loser >= 0 && c.loser < static_cast<int>(g_mods.size())) {
            overrides_graph[c.winner].insert(c.loser);
            overridden_by_graph[c.loser].insert(c.winner);
        }
    }
    for (size_t i = 0; i < g_mods.size(); ++i) {
        for (int prev : overrides_graph[i]) g_mods[i].overrides_mods.push_back(g_mods[prev].name);
        for (int later : overridden_by_graph[i]) g_mods[i].overridden_by_mods.push_back(g_mods[later].name);
    }

    for (size_t i = 0; i < g_conflicts.size() && i < kConflictLines; i++) {
        const Conflict& c = g_conflicts[i];
        LOG("mods: conflict %s: %s wins over %s", c.path.c_str(), owner_name(c.winner).c_str(), owner_name(c.loser).c_str());
    }
    if (g_conflicts.size() > kConflictLines) LOG("mods: %llu more conflicts not listed", static_cast<unsigned long long>(g_conflicts.size() - kConflictLines));
    if (!g_conflicts.empty()) LOG("mods: %llu conflicting files in total, the later mod in the load order wins", static_cast<unsigned long long>(g_conflicts.size()));
}

void reset_state_locked() {
    restore_previous_patches_locked();
    g_content = Node{};
    g_aoc = Node{};
    g_content_base.clear();
    g_aoc_base.clear();
    g_title = 0x0005000C101C9400ull;
    g_title_from_meta = false;
    g_pack = false;
    {
        std::lock_guard<std::mutex> lock(g_disc_mu);
        g_discovered.clear();
        g_pending_restart = false;
    }
    g_mods.clear();
    g_conflicts.clear();
    g_owned.clear();
    {
        std::lock_guard<std::mutex> lock(g_open_mu);
        g_opened.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_case_mu);
        g_case_cache.clear();
    }
    g_files = g_replacing = g_added = 0;
}

void rebuild_state_locked() {
    reset_state_locked();
    g_inited = true;

    if (!g_game_path.empty()) {
        fs::path game = utf8_to_path(g_game_path);
        g_content_base = path_to_utf8(game / "content");
        fs::path aoc = aoc_content_root(game / "aoc");
        if (!aoc.empty()) g_aoc_base = path_to_utf8(aoc);
        uint64_t base = g_base_title_id ? g_base_title_id : parse_title(g_game_path);
        if (base != 0) {
            g_title = (base & 0xFFFFFFFFULL) | 0x0005000C00000000ULL;
            g_title_from_meta = true;
        }
    }

    fs::path user = g_user_path.empty() ? fs::path() : utf8_to_path(g_user_path);
    fs::path user_aoc = user.empty() ? fs::path() : aoc_content_root(user / "aoc");
    size_t user_aoc_files = 0, user_aoc_replaces = 0;
    if (!user_aoc.empty()) user_aoc_files = add_tree(g_aoc, user_aoc, g_aoc_base, kUserDlc, "aoc", user_aoc_replaces);
    if (user_aoc_files)
        LOG("mods: user DLC folder %s (%llu %s, below mods)", path_to_utf8(user_aoc).c_str(), static_cast<unsigned long long>(user_aoc_files),
            user_aoc_files == 1 ? "file" : "files");

    if (g_no_mods) LOG("mods: off for this run (--no-mods)");
    else if (!user.empty()) load_mods(user);
    if (!user.empty()) scan_discovered(user);

    uint64_t pack_sz = 0;
    if (is_file(case_path(g_aoc_base, "0010/Pack/AocMainField.pack"), pack_sz)) g_pack = true;
    bool fd = false, fo = false;
    const Node* pk = walk(g_aoc, "0010/Pack/AocMainField.pack", fd, fo);
    if (pk && fd) g_pack = true;
    tally(g_content);
    tally(g_aoc);
    collect_owned(g_aoc);
    collect_owned_base(g_aoc_base);
    if (g_pack) g_owned.insert(0x0010);
    if (g_no_dlc) {
        g_pack = false;
        g_owned.clear();
    }

    apply_patches_locked();
    if (!user.empty()) scan_discovered(user);
    g_graphic_pack_version.fetch_add(1, std::memory_order_release);

    if (!g_mods.empty() || g_files)
        LOG("mods: %llu mods loaded, %llu overlay files, %llu replace a game file, %llu new", static_cast<unsigned long long>(g_mods.size()), static_cast<unsigned long long>(g_files), static_cast<unsigned long long>(g_replacing),
            static_cast<unsigned long long>(g_added));
    char mount[40];
    snprintf(mount, sizeof mount, "/vol/aoc%016llx", static_cast<unsigned long long>(g_title));
    if (g_no_dlc) {
        LOG("DLC: disabled for this run (--no-dlc)");
    } else if (g_pack) {
        LOG("DLC: installed as %s (AocMainField.pack found%s)", mount, g_title_from_meta ? "" : ", title id assumed USA");
    } else if (g_aoc.kids.empty() && !base_nonempty(g_aoc_base)) {
        LOG("DLC: not installed");
    } else {
        LOG("DLC: not installed. Add-on files were found, but AocMainField.pack was not, so the game is told the DLC is missing");
    }
}

}  // namespace

// ============================================================================
// Public API Implementation
// ============================================================================
namespace mods {

Rules parse_rules(std::string_view text) {
    RulesFile rf = parse_rules_internal(text);
    Rules r;
    r.has_definition = rf.has_definition;
    r.name = std::move(rf.name);
    r.description = std::move(rf.description);
    r.version = std::move(rf.version);
    r.title_ids = std::move(rf.title_ids);
    r.default_vars = rf.default_vars;
    r.presets = rf.presets;

    Config empty_cfg;
    std::vector<PresetCategory> cats;
    std::vector<std::pair<std::string, double>> vars;
    evaluate_rules_for_mod("", rf, empty_cfg, cats, vars, r.texture_rules);
    return r;
}

bool Config::is_enabled(std::string_view folder) const {
    std::string want = lower_ascii(folder);
    bool on = false;
    for (const auto& e : enabled)
        if (lower_ascii(e.folder) == want) on = e.enabled;
    return on;
}

std::string Config::get_preset(std::string_view folder, std::string_view category) const {
    std::string want_key = category.empty() ? lower_ascii(folder)
                                            : (lower_ascii(folder) + ":" + lower_ascii(category));
    std::string val;
    for (const auto& pe : presets) {
        if (lower_ascii(pe.key) == want_key) val = pe.value;
    }
    return val;
}

Config parse_config(std::string_view text) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text.remove_prefix(3);
    }
    Config cfg;
    std::string section;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string line = trim(text.substr(at, nl - at));
        at = nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            section = lower_ascii(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (section == "enabled") {
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = lower_ascii(unquote(trim(line.substr(eq + 1))));
            bool on = (val == "1" || val == "true" || val == "yes" || val == "on");
            if (valid_folder(key)) cfg.enabled.push_back({key, on});
        } else if (section == "order") {
            auto add_order_item = [&](std::string f) {
                f = trim(f);
                if (!valid_folder(f)) return;
                bool dup = false;
                for (const auto& o : cfg.order)
                    if (lower_ascii(o) == lower_ascii(f)) dup = true;
                if (!dup) cfg.order.push_back(f);
            };
            if (eq != std::string::npos) {
                std::string key = lower_ascii(trim(line.substr(0, eq)));
                std::string val = unquote(trim(line.substr(eq + 1)));
                if (key == "order") {
                    cfg.has_order = true;
                    cfg.order.clear();
                    std::string cur;
                    for (char c : val + ",") {
                        if (c == ',') {
                            add_order_item(cur);
                            cur.clear();
                        } else {
                            cur.push_back(c);
                        }
                    }
                }
            } else {
                // Also support one-folder-per-line under [Order]
                cfg.has_order = true;
                add_order_item(unquote(line));
            }
        } else if (section == "presets") {
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = unquote(trim(line.substr(eq + 1)));
            if (!key.empty() && !val.empty()) {
                cfg.presets.push_back({key, val});
            }
        }
    }
    return cfg;
}

std::vector<std::string> load_order(const Config& cfg, const std::vector<std::string>& present) {
    std::vector<std::string> on;
    for (const auto& p : present)
        if (cfg.is_enabled(p)) on.push_back(p);
    std::sort(on.begin(), on.end(), iless);
    if (!cfg.has_order) return on;
    std::vector<std::string> listed, unlisted;
    for (const auto& o : cfg.order)
        for (const auto& p : on)
            if (lower_ascii(p) == lower_ascii(o)) listed.push_back(p);
    for (const auto& p : on)
        if (std::find(listed.begin(), listed.end(), p) == listed.end()) unlisted.push_back(p);
    unlisted.insert(unlisted.end(), listed.begin(), listed.end());
    return unlisted;
}

void init(const std::string& game_root, const std::string& user_root, bool enabled, bool enable_dlc,
          uint64_t base_title_id) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    g_game_path = game_root;
    g_user_path = user_root;
    g_no_mods = !enabled;
    g_no_dlc = !enable_dlc;
    g_base_title_id = base_title_id;
    rebuild_state_locked();
}

void reload() {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    rebuild_state_locked();
}

void log_shutdown() {
    if (!g_inited) return;
    static bool once = false;
    if (once) return;
    once = true;
    if (!g_files) return;
    LOG("mods: %llu distinct overlay files were opened this run", static_cast<unsigned long long>(opened_files()));
}

void set_patch_backend(PatchBackend backend) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    g_patch_backend = std::move(backend);
}

void apply_patches() {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    apply_patches_locked();
}

Query query(const std::string& guest) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    Query q;
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return q;
    if (vol.id == GuestVol::Id::Aoc && g_no_dlc) {
        q.kind = Query::Kind::Missing;
        return q;
    }
    if (vol.id == GuestVol::Id::Content && g_content.kids.empty()) return q;
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
        q.host = vol.rel.empty() ? base_for(vol.id) : case_path(base_for(vol.id), vol.rel);
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
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    out.clear();
    GuestVol vol = parse_guest(guest);
    if (vol.id != GuestVol::Id::Content && vol.id != GuestVol::Id::Aoc) return ListStatus::NotVolume;
    if (vol.id == GuestVol::Id::Aoc && g_no_dlc) return ListStatus::Missing;
    if (vol.id == GuestVol::Id::Content && g_content.kids.empty()) return ListStatus::NotVolume;
    bool file = false, occluded = false;
    const Node* node = walk(tree_for(vol.id), vol.rel, file, occluded);
    if (occluded) return ListStatus::Missing;
    if (node && file) return ListStatus::NotDir;
    std::string base_dir = vol.rel.empty() ? base_for(vol.id) : case_path(base_for(vol.id), vol.rel);
    bool base_is_dir = is_dir(base_dir);
    if (!base_is_dir) {
        uint64_t sz = 0;
        if (is_file(base_dir, sz) && !(node && !file)) return ListStatus::NotDir;
    }
    bool overlay_dir = node && !file;
    if (!base_is_dir && !overlay_dir) return ListStatus::Missing;

    std::map<std::string, DirItem> merged;
    if (base_is_dir) {
        std::error_code ec;
        for (fs::directory_iterator it(utf8_to_path(base_dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code sec;
            if (it->is_symlink(sec)) continue;
            std::string name = path_to_utf8(it->path().filename());
            if (name.empty() || name[0] == '.') continue;
            DirItem item;
            item.name = name;
            std::string host = path_to_utf8(it->path());
            uint64_t sz = 0;
            if (is_file(host, sz)) {
                item.size = sz;
            } else if (is_dir(host)) {
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
                item.size = sz;
            } else {
                item.dir = true;
            }
            merged[key] = item;
        }
    }
    out.reserve(merged.size());
    for (auto& [_, item] : merged) out.push_back(std::move(item));
    return ListStatus::Ok;
}

bool read_only_volume(const std::string& guest) {
    GuestVol vol = parse_guest(guest);
    return vol.id == GuestVol::Id::Content || vol.id == GuestVol::Id::Aoc || vol.id == GuestVol::Id::Code ||
           vol.id == GuestVol::Id::Meta;
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

bool disabled_by_flag() { return g_no_mods; }
const std::vector<ModInfo>& loaded() {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    return g_mods;
}
const std::vector<ModInfo>& enabled_mods() {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    return g_mods;
}
const std::vector<DiscoveredMod>& discovered_mods() {
    std::lock_guard<std::mutex> lock(g_disc_mu);
    return g_discovered;
}
std::vector<DiscoveredMod> status() {
    std::lock_guard<std::mutex> lock(g_disc_mu);
    return g_discovered;
}

size_t conflicts() { return g_conflicts.size(); }
size_t index_files() { return g_files; }
size_t replacing_files() { return g_replacing; }
size_t added_files() { return g_added; }
size_t opened_files() {
    std::lock_guard<std::mutex> lock(g_open_mu);
    return g_opened.size();
}

std::string summary() {
    if (g_no_mods) return "Off for this run (--no-mods)";
    if (g_mods.empty()) return "None loaded";
    std::string s = std::to_string(g_mods.size()) + ": ";
    for (size_t i = g_mods.size(); i-- > 0;) {  // highest priority first
        if (i + 1 != g_mods.size()) s += ", ";
        s += g_mods[i].name;
    }
    if (s.size() > 40) s = s.substr(0, 37) + "...";
    return s;
}

bool pending_restart() {
    std::lock_guard<std::mutex> lock(g_disc_mu);
    return g_pending_restart;
}

bool set_enabled(std::string_view folder, bool on) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    if (g_user_path.empty() || !valid_folder(std::string(folder))) return false;
    fs::path user = utf8_to_path(g_user_path);
    fs::path ini_path = user / "mods.ini";

    auto secs = read_ini_sections(ini_path);
    IniSec& en = ensure_ini_section(secs, "Enabled", /*at_front=*/true);
    bool updated = false;
    std::string want = lower_ascii(folder);
    for (auto& [k, v] : en.kv) {
        if (lower_ascii(k) == want) {
            v = on ? "1" : "0";
            updated = true;
        }
    }
    if (!updated) en.kv.emplace_back(std::string(folder), on ? "1" : "0");
    if (!write_ini_sections(ini_path, secs)) return false;

    std::lock_guard<std::mutex> lock(g_disc_mu);
    bool diff = false;
    for (auto& d : g_discovered) {
        if (lower_ascii(d.folder) == want) {
            d.enabled = on;
        }
        if (d.enabled != d.loaded_now) diff = true;
    }
    g_pending_restart = diff;
    return true;
}

bool set_preset(std::string_view folder, std::string_view category, std::string_view preset_name) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    if (g_user_path.empty() || !valid_folder(std::string(folder))) return false;

    std::string want_folder = lower_ascii(folder);
    std::string resolved_cat(category);
    bool found_mod = false;
    bool found_cat = false;
    bool single_cat = false;
    std::string mod_root_utf8;

    {
        std::lock_guard<std::mutex> lock(g_disc_mu);
        for (const auto& d : g_discovered) {
            if (lower_ascii(d.folder) == want_folder) {
                found_mod = true;
                mod_root_utf8 = d.root_path;
                single_cat = (d.presets.size() == 1);
                for (const auto& pc : d.presets) {
                    if (iequals(pc.category, resolved_cat)) {
                        resolved_cat = pc.category;
                        found_cat = true;
                        break;
                    }
                }
                if (!found_cat && resolved_cat.empty() && d.presets.size() == 1) {
                    resolved_cat = d.presets[0].category;
                    found_cat = true;
                }
                break;
            }
        }
    }
    if (!found_mod || !found_cat) return false;

    fs::path user = utf8_to_path(g_user_path);
    fs::path ini_path = user / "mods.ini";
    auto secs = read_ini_sections(ini_path);
    IniSec& ps = ensure_ini_section(secs, "Presets", /*at_front=*/false);

    auto upsert_kv = [&](const std::string& key, const std::string& val) {
        std::string lk_key = lower_ascii(key);
        for (auto& [k, v] : ps.kv) {
            if (lower_ascii(k) == lk_key) {
                v = val;
                return;
            }
        }
        ps.kv.emplace_back(key, val);
    };

    std::string key_cat = resolved_cat.empty() ? std::string(folder)
                                               : (std::string(folder) + ":" + resolved_cat);
    upsert_kv(key_cat, std::string(preset_name));
    if (resolved_cat.empty() || single_cat) {
        upsert_kv(std::string(folder), std::string(preset_name));
    }
    if (!write_ini_sections(ini_path, secs)) return false;

    // Re-evaluate rules, texture_rules, and shaders for this mod right away
    Config cfg = parse_config(read_text(ini_path, 256 * 1024));
    ResolvedModLayout layout = resolve_mod_layout(utf8_to_path(mod_root_utf8));
    RulesFile rf = !layout.rules_path.empty()
                       ? parse_rules_internal(read_text(layout.rules_path, 64 * 1024))
                       : parse_rules_internal(read_text(utf8_to_path(mod_root_utf8) / "rules.txt", 64 * 1024));

    std::vector<PresetCategory> new_cats;
    std::vector<std::pair<std::string, double>> new_vars;
    std::vector<TextureRedefineRule> new_tex;
    evaluate_rules_for_mod(std::string(folder), rf, cfg, new_cats, new_vars, new_tex);

    for (auto& m : g_mods) {
        if (lower_ascii(m.folder) == want_folder) {
            m.presets = new_cats;
            m.evaluated_vars = new_vars;
            m.texture_rules = new_tex;
            if (!layout.shader_files.empty()) {
                load_custom_shaders_for_mod(layout, m.evaluated_vars, m.custom_shaders);
            }
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_disc_mu);
        for (auto& d : g_discovered) {
            if (lower_ascii(d.folder) == want_folder) {
                d.presets = new_cats;
                d.evaluated_vars = new_vars;
                d.texture_rules = new_tex;
                break;
            }
        }
    }
    g_graphic_pack_version.fetch_add(1, std::memory_order_release);
    return true;
}

bool move_order(std::string_view folder, int delta) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    if (g_user_path.empty() || delta == 0 || !valid_folder(std::string(folder))) return false;

    std::vector<std::string> current_order;
    size_t idx = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_disc_mu);
        for (size_t i = 0; i < g_discovered.size(); ++i) {
            current_order.push_back(g_discovered[i].folder);
            if (lower_ascii(g_discovered[i].folder) == lower_ascii(folder)) {
                idx = i;
                found = true;
            }
        }
    }
    if (!found) return false;
    int target = std::clamp(static_cast<int>(idx) + delta, 0, static_cast<int>(current_order.size()) - 1);
    if (target == static_cast<int>(idx)) return false;

    std::string item = current_order[idx];
    current_order.erase(current_order.begin() + static_cast<ptrdiff_t>(idx));
    current_order.insert(current_order.begin() + target, item);

    fs::path user = utf8_to_path(g_user_path);
    fs::path ini_path = user / "mods.ini";
    auto secs = read_ini_sections(ini_path);
    IniSec& ord = ensure_ini_section(secs, "Order", /*at_front=*/false);
    ord.kv.clear();
    std::string joined;
    for (size_t i = 0; i < current_order.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += current_order[i];
    }
    ord.kv.emplace_back("order", joined);
    if (!write_ini_sections(ini_path, secs)) return false;

    {
        std::lock_guard<std::mutex> lock(g_disc_mu);
        DiscoveredMod dm = std::move(g_discovered[idx]);
        g_discovered.erase(g_discovered.begin() + static_cast<ptrdiff_t>(idx));
        g_discovered.insert(g_discovered.begin() + target, std::move(dm));
        g_pending_restart = true;
    }
    return true;
}

bool install_from_path(const std::string& user_root, const std::string& src_path,
                       std::string* installed_folder_out, std::string* err_out) {
    auto set_err = [&](std::string msg) {
        if (err_out) *err_out = std::move(msg);
        return false;
    };
    if (user_root.empty() || src_path.empty()) {
        return set_err("Missing user folder or mod source path.");
    }

    fs::path src = utf8_to_path(src_path);
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        return set_err("Mod source path does not exist: " + src_path);
    }

    fs::path user_dir = utf8_to_path(user_root);
    fs::path mods_dir = user_dir / "mods";
    fs::create_directories(mods_dir, ec);

    std::string stem = path_to_utf8(src.filename());
    if (fs::is_regular_file(src, ec)) {
        std::string ext = lower_ascii(path_to_utf8(src.extension()));
        if (ext != ".zip" && ext != ".bnp" && ext != ".7z") {
            return set_err("Only .zip / .bnp / .7z archives or mod folders can be installed directly.");
        }
        stem = path_to_utf8(src.stem());
    }
    if (stem.empty() || !valid_folder(stem)) stem = "InstalledMod";

    fs::path staging = mods_dir / utf8_to_path(".installing_" + stem);
    fs::remove_all(staging, ec);

    if (fs::is_regular_file(src, ec)) {
        std::string zip_err;
        if (!extract_zip_utf8(src, staging, zip_err)) {
            bool tar_ok = false;
#ifdef _WIN32
            fs::create_directories(staging, ec);
            std::wstring cmd = L"C:\\Windows\\System32\\tar.exe -xf \"" + src.wstring() + L"\" -C \"" + staging.wstring() + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
            cmd_buf.push_back(L'\0');
            if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 120000);
                DWORD exit_code = 1;
                GetExitCodeProcess(pi.hProcess, &exit_code);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                tar_ok = (exit_code == 0);
            }
#endif
            if (!tar_ok) {
                fs::remove_all(staging, ec);
                return set_err(zip_err);
            }
        }
        fs::path info_p = staging / "info.json";
        fs::path rules_p = staging / "rules.txt";
        if (fs::is_regular_file(info_p, ec) && !fs::is_regular_file(rules_p, ec)) {
            std::string jtxt;
            if (read_file_utf8(info_p, jtxt)) {
                auto get_jstr = [&](const char* k) -> std::string {
                    std::string pat = std::string("\"") + k + "\"";
                    size_t p = jtxt.find(pat);
                    if (p == std::string::npos) return {};
                    p = jtxt.find(':', p + pat.size());
                    if (p == std::string::npos) return {};
                    p = jtxt.find('"', p + 1);
                    if (p == std::string::npos) return {};
                    ++p;
                    std::string s;
                    while (p < jtxt.size() && jtxt[p] != '"') {
                        if (jtxt[p] == '\\' && p + 1 < jtxt.size()) { ++p; if (jtxt[p] == 'n') break; }
                        s.push_back(jtxt[p++]);
                    }
                    return std::string(trim(s));
                };
                std::string mname = get_jstr("name");
                std::string mdesc = get_jstr("desc");
                if (!mname.empty()) {
                    std::ofstream rout(rules_p, std::ios::binary | std::ios::trunc);
                    rout << "[Definition]\r\n"
                         << "titleIds = 00050000101C9300,00050000101C9400,00050000101C9500\r\n"
                         << "name = " << mname << "\r\n"
                         << "path = \"The Legend of Zelda: Breath of the Wild/Mods/" << mname << "\"\r\n"
                         << "description = " << (mdesc.empty() ? mname : mdesc) << "\r\n"
                         << "version = 7\r\n";
                }
            }
        }
    } else if (fs::is_directory(src, ec)) {
        fs::create_directories(staging, ec);
        fs::copy(src, staging, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            return set_err("Failed copying mod folder: " + ec.message());
        }
    } else {
        return set_err("Unsupported mod source type.");
    }

    ResolvedModLayout layout = resolve_mod_layout(staging);
    if (!layout.valid) {
        fs::remove_all(staging, ec);
        return set_err("No recognizable BotW mod files (content/, aoc/, Actor/, Pack/, rules.txt, or patches.txt) found in " +
                       stem + ".");
    }

    fs::path final_dest = mods_dir / utf8_to_path(stem);
    fs::remove_all(final_dest, ec);
    fs::rename(layout.effective_root, final_dest, ec);
    if (ec) {
        fs::create_directories(final_dest, ec);
        fs::copy(layout.effective_root, final_dest,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    fs::remove_all(staging, ec);

    if (installed_folder_out) *installed_folder_out = stem;

    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    fs::path ini_path = user_dir / "mods.ini";
    auto secs = read_ini_sections(ini_path);
    IniSec& en = ensure_ini_section(secs, "Enabled", /*at_front=*/true);
    bool found = false;
    for (auto& [k, v] : en.kv) {
        if (lower_ascii(k) == lower_ascii(stem)) {
            v = "1";
            found = true;
        }
    }
    if (!found) en.kv.emplace_back(stem, "1");
    for (auto& sec : secs) {
        if (lower_ascii(sec.name) != "order") continue;
        for (auto& [k, v] : sec.kv) {
            if (lower_ascii(k) != "order") continue;
            bool in_order = false;
            std::string cur;
            for (size_t i = 0; i <= v.size(); ++i) {
                if (i == v.size() || v[i] == ',') {
                    std::string item(trim(cur));
                    if (lower_ascii(item) == lower_ascii(stem)) in_order = true;
                    cur.clear();
                } else {
                    cur.push_back(v[i]);
                }
            }
            if (!in_order) {
                if (!trim(v).empty()) v += ", ";
                v += stem;
            }
        }
    }
    write_ini_sections(ini_path, secs);

    if (g_user_path == user_root) {
        scan_discovered(user_dir);
    }
    return true;
}

bool install_from_path(const std::string& src_path, std::string* installed_folder_out,
                       std::string* err_out) {
    std::string ur;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
        ur = g_user_path;
    }
    return install_from_path(ur, src_path, installed_folder_out, err_out);
}

uint64_t graphic_pack_version() {
    return g_graphic_pack_version.load(std::memory_order_acquire);
}

std::optional<std::string> find_custom_shader(uint64_t base_hash, uint64_t aux_hash,
                                              ShaderStage stage) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mtx);
    for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it) {
        for (const auto& cs : it->custom_shaders) {
            if (cs.stage != stage || cs.base_hash != base_hash) continue;
            if (cs.has_aux_hash && cs.aux_hash != aux_hash) continue;
            return cs.source;
        }
    }
    return std::nullopt;
}

}  // namespace mods
