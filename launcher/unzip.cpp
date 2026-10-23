#include "unzip.h"

#include <zlib.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <vector>

namespace {

constexpr uint64_t kMaxZipBytes = 512ull * 1024 * 1024;
constexpr uint64_t kMaxUnpackBytes = 1024ull * 1024 * 1024;
constexpr uint64_t kMaxFileBytes = 256ull * 1024 * 1024;
constexpr int kMaxFiles = 8000;

uint16_t r16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t r32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

bool safe_parts(const std::string& name, std::vector<std::string>& parts) {
    parts.clear();
    if (name.empty() || name[0] == '/' || name[0] == '\\') return false;
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') return false;
    std::string cur;
    auto flush = [&](bool dir_slash) {
        if (cur == "..") return false;
        if (!cur.empty() && cur != ".") parts.push_back(cur);
        cur.clear();
        (void)dir_slash;
        return true;
    };
    for (char c : name) {
        if (c == '\0') return false;
        if (c == '/' || c == '\\') {
            if (!flush(true)) return false;
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && !flush(false)) return false;
    return true;
}

bool inside_dest(const std::filesystem::path& dest, const std::filesystem::path& file) {
    std::error_code ec;
    auto base = std::filesystem::weakly_canonical(dest, ec);
    if (ec) return false;
    auto parent = file.parent_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) return false;
    auto canon = std::filesystem::weakly_canonical(parent, ec);
    if (ec) return false;
    auto b = base.u8string();
    auto p = canon.u8string();
    if (p == b) return true;
    if (p.size() <= b.size()) return false;
    if (p.compare(0, b.size(), b) != 0) return false;
    char sep = static_cast<char>(p[b.size()]);
    return sep == '/' || sep == '\\';
}

bool inflate_raw(const uint8_t* in, uint32_t in_n, std::vector<uint8_t>& out, std::string& err) {
    z_stream strm{};
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        err = "could not start deflate";
        return false;
    }
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in));
    strm.avail_in = in_n;
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());
    int rc = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (rc != Z_STREAM_END || strm.total_out != out.size()) {
        err = "deflate data in the zip is truncated or corrupt";
        return false;
    }
    return true;
}

}  // namespace

bool unzip_to(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string& err) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(zip, ec);
    if (ec) {
        err = "could not read zip";
        return false;
    }
    if (sz < 22 || sz > kMaxZipBytes) {
        err = sz > kMaxZipBytes ? "zip is larger than 512 MB" : "file is not a zip";
        return false;
    }
    std::ifstream in(zip, std::ios::binary);
    if (!in) {
        err = "could not open zip";
        return false;
    }
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    if (static_cast<uint64_t>(in.gcount()) != sz) {
        err = "could not read zip";
        return false;
    }
    size_t eocd = static_cast<size_t>(-1);
    size_t back = sz < (65535u + 22) ? static_cast<size_t>(sz) : (65535u + 22);
    for (size_t i = 0; i + 22 <= back; i++) {
        size_t off = static_cast<size_t>(sz) - 22 - i;
        if (r32(data.data() + off) != 0x06054b50) continue;
        uint16_t comment = r16(data.data() + off + 20);
        if (off + 22 + comment != sz) continue;
        eocd = off;
        break;
    }
    if (eocd == static_cast<size_t>(-1)) {
        err = "file is not a zip (no end of central directory)";
        return false;
    }
    if (eocd >= 20 && r32(data.data() + eocd - 20) == 0x07064b50) {
        err = "zip64 archives are not supported";
        return false;
    }
    if (r16(data.data() + eocd + 4) != 0 || r16(data.data() + eocd + 6) != 0) {
        err = "multi-disk zips are not supported";
        return false;
    }
    uint16_t entries = r16(data.data() + eocd + 10);
    uint32_t cd_size = r32(data.data() + eocd + 12);
    uint32_t cd_off = r32(data.data() + eocd + 16);
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {
        err = "zip64 archives are not supported";
        return false;
    }
    if (static_cast<uint64_t>(cd_off) + cd_size > sz) {
        err = "zip central directory is truncated";
        return false;
    }
    if (entries > kMaxFiles) {
        err = "zip has too many files";
        return false;
    }
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        err = "could not create the folder for the zip";
        return false;
    }
    size_t p = cd_off;
    uint64_t total_uncomp = 0;
    for (uint16_t n = 0; n < entries; n++) {
        if (p + 46 > static_cast<size_t>(sz) || r32(data.data() + p) != 0x02014b50) {
            err = "zip central directory is corrupt";
            return false;
        }
        uint16_t flags = r16(data.data() + p + 8);
        uint16_t method = r16(data.data() + p + 10);
        uint32_t comp = r32(data.data() + p + 20);
        uint32_t uncomp = r32(data.data() + p + 24);
        uint16_t namelen = r16(data.data() + p + 28);
        uint16_t extralen = r16(data.data() + p + 30);
        uint16_t commentlen = r16(data.data() + p + 32);
        uint32_t local = r32(data.data() + p + 42);
        if (p + 46 + namelen > sz) {
            err = "zip file name is truncated";
            return false;
        }
        std::string name(reinterpret_cast<char*>(data.data() + p + 46), namelen);
        p += 46 + namelen + extralen + commentlen;
        if (comp == 0xFFFFFFFFu || uncomp == 0xFFFFFFFFu || local == 0xFFFFFFFFu) {
            err = "zip64 entries are not supported";
            return false;
        }
        if (flags & 1) {
            err = "encrypted zips are not supported";
            return false;
        }
        std::vector<std::string> parts;
        if (!safe_parts(name, parts)) {
            err = "zip contains a path that escapes the destination (" + name + ")";
            return false;
        }
        bool dir_entry = !name.empty() && (name.back() == '/' || name.back() == '\\');
        std::filesystem::path out = dest;
        for (const auto& part : parts) out /= std::filesystem::path(std::u8string(part.begin(), part.end()));
        if (dir_entry || (parts.empty() && name.find('/') != std::string::npos)) {
            std::filesystem::create_directories(parts.empty() ? dest : out, ec);
            continue;
        }
        if (method != 0 && method != 8) {
            err = "zip entry uses an unsupported compression method (" + name + ")";
            return false;
        }
        if (uncomp > kMaxFileBytes) {
            err = "a file inside the zip is larger than 256 MB";
            return false;
        }
        total_uncomp += uncomp;
        if (total_uncomp > kMaxUnpackBytes || (total_uncomp > 64ull * 1024 * 1024 && total_uncomp > sz * 80)) {
            err = "zip expands too far";
            return false;
        }
        if (!inside_dest(dest, out)) {
            err = "zip contains a path that escapes the destination (" + name + ")";
            return false;
        }
        if (static_cast<uint64_t>(local) + 30 > sz || r32(data.data() + local) != 0x04034b50) {
            err = "zip local header is missing for " + name;
            return false;
        }
        uint16_t lname = r16(data.data() + local + 26);
        uint16_t lextra = r16(data.data() + local + 28);
        uint64_t data_off = static_cast<uint64_t>(local) + 30u + lname + lextra;
        if (data_off + comp > sz) {
            err = "zip entry is truncated (" + name + ")";
            return false;
        }
        std::filesystem::create_directories(out.parent_path(), ec);
        std::vector<uint8_t> raw(uncomp);
        const uint8_t* src = data.data() + data_off;
        if (method == 0) {
            if (comp != uncomp) {
                err = "stored zip entry has a mismatched size (" + name + ")";
                return false;
            }
            raw.assign(src, src + uncomp);
        } else if (!inflate_raw(src, comp, raw, err)) {
            err += " (" + name + ")";
            return false;
        }
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file) {
            err = "could not write " + name;
            return false;
        }
        if (!raw.empty()) file.write(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        if (!file) {
            err = "could not write " + name;
            return false;
        }
    }
    return true;
}
