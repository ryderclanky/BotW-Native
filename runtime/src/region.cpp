// Region detection from the dump's meta.xml and the U-King.rpx version check.
#include "region.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <vector>

#include "log.h"
#include "utf8_path.h"

#if defined(_WIN32)
void region_message_box(const char*);
#endif

uint64_t g_foreground_title_id = 0x00050000101C9400ull;  // CafeSystem::GetForegroundTitleId

namespace region {
namespace {
Info g_info;

// SHA-256 of the v1.5.0 U-King.rpx. The USA and Europe updates ship the same file.
const char* const kKnownRpx[] = {
    "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30",
};

std::string read_file(const std::string& p) {
    std::ifstream f(botw::path_from_utf8(p), std::ios::binary);
    if (!f) return {};
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// First run of `digits` hex characters after the tag name, or empty.
std::string tag_hex(const std::string& xml, const char* tag, size_t digits) {
    std::string open = std::string("<") + tag;
    size_t at = xml.find(open);
    if (at == std::string::npos) return {};
    at = xml.find('>', at);
    if (at == std::string::npos) return {};
    std::string hex;
    for (size_t i = at + 1; i < xml.size() && xml[i] != '<'; i++) {
        if (isxdigit((unsigned char)xml[i])) hex.push_back(xml[i]);
        else if (!hex.empty()) break;
    }
    return hex.size() == digits ? hex : std::string();
}

struct Lang {
    const char* code;
    uint32_t id;
};
const Lang kLangs[] = {{"ja", 0}, {"en", 1}, {"fr", 2}, {"de", 3}, {"it", 4}, {"es", 5},
                       {"zh", 6}, {"ko", 7}, {"nl", 8}, {"pt", 9}, {"ru", 10}, {"tw", 11}};

int lang_id(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    for (const Lang& l : kLangs)
        if (s == l.code) return (int)l.id;
    return -1;
}
const char* lang_code(uint32_t id) {
    for (const Lang& l : kLangs)
        if (l.id == id) return l.code;
    return "?";
}

// Languages each release has text for, first is the default.
std::vector<uint32_t> region_langs(uint32_t code) {
    if (code == JPN) return {0};
    if (code == EUR) return {1, 3, 2, 5, 4, 8, 10};
    return {1, 2, 5};
}

uint32_t country_for(uint32_t code, uint32_t lang) {
    if (code == JPN) return 1;  // Japan
    if (code == EUR) {
        switch (lang) {
        case 3: return 78;   // Germany
        case 2: return 77;   // France
        case 5: return 105;  // Spain
        case 4: return 83;   // Italy
        case 8: return 94;   // Netherlands
        case 10: return 100; // Russia
        default: return 110; // United Kingdom
        }
    }
    if (lang == 2) return 18;  // Canada
    if (lang == 5) return 54;  // Mexico
    return 49;                 // United States
}

// ---- SHA-256 ----
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t bits = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        bits += (uint64_t)n * 8;
        while (n) {
            size_t take = std::min(n, 64 - used);
            memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string hex() {
        uint64_t total = bits;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(total >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

void show_error(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
#if defined(_WIN32)
    ::region_message_box(msg.c_str());
#endif
}
}  // namespace

const char* name(uint32_t code) { return code == JPN ? "Japan" : code == EUR ? "Europe" : "USA"; }

void init(const std::string& game_root, const std::string& language_override) {
    Info info;
    std::string xml = read_file(game_root + "/meta/meta.xml");
    bool has_meta = !xml.empty();
    if (!has_meta) xml = read_file(game_root + "/code/app.xml");
    std::string tid = tag_hex(xml, "title_id", 16);
    if (!tid.empty()) info.title_id = strtoull(tid.c_str(), nullptr, 16);
    uint32_t low = (uint32_t)info.title_id;
    std::string reg = tag_hex(xml, "region", 8);
    uint32_t r = reg.empty() ? 0 : (uint32_t)strtoul(reg.c_str(), nullptr, 16);
    if (r & EUR) info.code = EUR;
    else if (r & USA) info.code = USA;
    else if (r & JPN) info.code = JPN;
    else if (low == 0x101C9500) info.code = EUR;
    else if (low == 0x101C9300) info.code = JPN;
    else info.code = USA;
    if (!has_meta && !game_root.empty()) {
        unsigned found = 0;
        std::error_code ec;
        std::filesystem::directory_iterator it(botw::path_from_utf8(game_root) / "content" / "Pack", ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::string n = botw::path_to_utf8(it->path().filename());
            if (n.size() != 16 || n.compare(0, 7, "Bootup_") != 0 || n.compare(11, 5, ".pack") != 0) continue;
            std::string prefix = n.substr(7, 2);
            if (prefix == "US") found |= USA;
            else if (prefix == "EU") found |= EUR;
            else if (prefix == "JP") found |= JPN;
        }
        if (found == USA || found == EUR || found == JPN) info.code = found;
    }
    if (tid.empty() || !has_meta) {
        info.title_id = info.code == EUR ? 0x00050000101C9500ull : info.code == JPN ? 0x00050000101C9300ull : 0x00050000101C9400ull;
        if (tid.empty()) LOG("region: no title_id in metadata, using %s", name(info.code));
    } else {
        // app/update metadata may identify the update title; the foreground title is the base game.
        info.title_id = 0x0005000000000000ull | (info.title_id & 0xffffffffull);
    }

    std::vector<uint32_t> langs = region_langs(info.code);
    std::vector<uint32_t> available;
    const char* prefix = info.code == EUR ? "EU" : info.code == JPN ? "JP" : "US";
    for (uint32_t id : langs) {
        std::error_code ec;
        auto file = botw::path_from_utf8(game_root) / "content" / "Pack" /
                    (std::string("Bootup_") + prefix + lang_code(id) + ".pack");
        if (std::filesystem::is_regular_file(file, ec)) available.push_back(id);
    }
    if (available.empty()) available = langs;
    info.language = available[0];
    std::string want = language_override;
    if (want.empty())
        if (const char* e = getenv("BOTW_LANGUAGE")) want = e;
    if (!want.empty()) {
        int id = lang_id(want);
        bool ok = false;
        for (uint32_t l : available) ok |= id >= 0 && (uint32_t)id == l;
        if (ok) info.language = (uint32_t)id;
        else LOG("region: language '%s' is not in the %s release, using %s", want.c_str(), name(info.code), lang_code(info.language));
    }
    info.country = country_for(info.code, info.language);
    g_info = info;
    g_foreground_title_id = info.title_id;
    LOG("region: %s, title %016llx, language %s (%u), country %u", name(info.code),
        (unsigned long long)info.title_id, lang_code(info.language), info.language, info.country);
}

const Info& get() { return g_info; }

bool check_rpx(const std::string& path, bool warn_only) {
    std::ifstream f(botw::path_from_utf8(path), std::ios::binary);
    if (!f) return true;  // missing file: the loader reports it
    Sha256 sha;
    std::vector<uint8_t> chunk(1 << 20);
    while (f) {
        f.read((char*)chunk.data(), (std::streamsize)chunk.size());
        sha.update(chunk.data(), (size_t)f.gcount());
    }
    if (f.bad()) {
        LOG("U-King.rpx: could not read file for verification");
        return false;
    }
    std::string got = sha.hex();
    for (const char* k : kKnownRpx)
        if (got == k) {
            LOG("U-King.rpx: v1.5.0 (sha256 %s)", got.c_str());
            return true;
        }
    std::string msg =
        "This U-King.rpx is not the supported version.\n\n"
        "BotW-Native needs a supported v1.5.0 executable. Install update v208 over your dump "
        "so code/U-King.rpx comes from the update, not the base game.\n\n"
        "File: " + path + "\nSHA-256: " + got + "\n\nStart with --allow-unknown-rpx to try anyway.";
    LOG("U-King.rpx: unknown build (sha256 %s)%s", got.c_str(), warn_only ? ", continuing (--allow-unknown-rpx)" : "");
    if (warn_only) return true;
    show_error(msg);
    return false;
}
}  // namespace region

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
void region_message_box(const char* msg) { MessageBoxA(nullptr, msg, "BotW-Native", MB_OK | MB_ICONERROR); }
#endif
