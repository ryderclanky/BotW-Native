// coreinit FS (synchronous API) and nn_save. Guest paths: /vol/content -> <game>/content,
// /vol/code -> <game>/code, /vol/save -> <user>/save, /vol/aoc<titleid> -> DLC content.
// Enabled mods overlay content and aoc (runtime/src/coreinit/mods.cpp). The game dump is not written.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include "coreinit/mods.h"
#include "diag.h"
#include "hle.h"
#include "os.h"

#include "botw_target.h"
#if TARGET_OS_IOS  // iOS port: case-insensitive lookup in the game folder (ios/src/fs_case_ios.cpp)
std::string botw_ios_fix_case(const std::string& host);
#endif

namespace {
enum : int32_t {
    FS_OK = 0, FS_END = -2, FS_MAX = -3, FS_EXISTS = -5, FS_NOT_FOUND = -6, FS_NOT_FILE = -7, FS_NOT_DIR = -8,
    FS_PERMISSION = -10, FS_FATAL = -0x400,
};
constexpr uint32_t FLAG_DIR = 0x80000000u, FLAG_FILE = 0x01000000u;

struct Handle {
    FILE* f = nullptr;
    DIR* d = nullptr;
    std::vector<mods::DirItem> items;
    size_t item_at = 0;
    bool listed = false;
    std::string host;
};
std::mutex g_fs_lock;
std::vector<Handle> g_handles(1);  // index 0 unused
std::string g_cwd = "/vol/content";

// Win64 long is 32 bits, so struct stat / fseeko truncate past 2 GB and stat() can fail with
// EOVERFLOW. Cafe paths stay slash-separated; the host path uses backslashes.
std::string native_path(std::string s) {
#if defined(_WIN32)
    for (char& ch : s)
        if (ch == '/') ch = '\\';
#endif
    return s;
}

struct HostInfo {
    bool ok = false;
    bool dir = false;
    uint64_t size = 0;
    int err = 0;
};
HostInfo host_info(const std::string& hp) {
    HostInfo info;
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(hp.c_str(), &st) != 0) {
        info.err = errno;
        return info;
    }
    info.ok = true;
    info.dir = (st.st_mode & _S_IFMT) == _S_IFDIR;
    info.size = (uint64_t)st.st_size;
#else
    struct stat st;
    if (stat(hp.c_str(), &st) != 0) {
        info.err = errno;
        return info;
    }
    info.ok = true;
    info.dir = S_ISDIR(st.st_mode);
    info.size = (uint64_t)st.st_size;
#endif
    return info;
}

#if TARGET_OS_IOS
std::string host_path_exact(std::string p);
std::string host_path(std::string p) { return botw_ios_fix_case(host_path_exact(std::move(p))); }
#define host_path host_path_exact
#endif
std::string abs_guest(std::string p) {
    for (char& ch : p)
        if (ch == '\\') ch = '/';
    if (p.empty() || p[0] != '/') p = g_cwd + "/" + p;
    // Resolve '.' and '..' so a guest path cannot escape the volume it names.
    std::vector<std::string> parts;
    std::string cur;
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
    for (char ch : p) {
        if (ch == '/') flush();
        else cur.push_back(ch);
    }
    flush();
    std::string n;
    for (const auto& part : parts) {
        n += '/';
        n += part;
    }
    return n.empty() ? "/" : n;
}

std::string host_path(std::string p) {
    std::string n = abs_guest(std::move(p));
    auto starts = [&](const char* pre) { return n.compare(0, strlen(pre), pre) == 0; };
    if (starts("/vol/content")) return native_path(g_game_root + "/content" + n.substr(12));
    if (starts("/vol/code")) return native_path(g_game_root + "/code" + n.substr(9));
    if (starts("/vol/meta")) return native_path(g_game_root + "/meta" + n.substr(9));
    if (starts("/vol/save")) return native_path(g_user_root + "/save" + n.substr(9));
    if (starts("/vol/aoc")) {
        // /vol/aoc/<rel> or /vol/aoc<16 hex title id>/<rel>. Reads go through mods::query, which
        // also understands an aoc/content layout. This fallback is the unsuffixed game/aoc folder.
        size_t i = 8;
        if (n.size() >= 24) {
            bool hex = true;
            for (size_t k = 8; k < 24 && hex; k++)
                if (!std::isxdigit(static_cast<unsigned char>(n[k]))) hex = false;
            if (hex && (n.size() == 24 || n[24] == '/')) i = 24;
        }
        return native_path(g_game_root + "/aoc" + n.substr(i));
    }
    return native_path(g_user_root + "/unknown_volume" + n);
}
#if TARGET_OS_IOS
#undef host_path
#endif

void host_mkdir(const char* path) {
#if defined(_WIN32)
    // MinGW mkdir takes only the path. EEXIST is fine: parents may already exist.
    if (_mkdir(path) != 0 && errno != EEXIST) return;
#else
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return;
#endif
}

void mkdirs(const std::string& p) {
    for (size_t i = 1; i < p.size(); i++) {
#if defined(_WIN32)
        if (p[i] == '/' || p[i] == '\\') host_mkdir(p.substr(0, i).c_str());
#else
        if (p[i] == '/') host_mkdir(p.substr(0, i).c_str());
#endif
    }
    host_mkdir(p.c_str());
}

uint32_t add_handle(Handle h) {
    for (size_t i = 1; i < g_handles.size(); i++)
        if (!g_handles[i].f && !g_handles[i].d && !g_handles[i].listed) {
            g_handles[i] = h;
            return (uint32_t)i;
        }
    g_handles.push_back(h);
    return (uint32_t)g_handles.size() - 1;
}
Handle* get(uint32_t h) { return h && h < g_handles.size() ? &g_handles[h] : nullptr; }

// Kernel ReadFile/WriteFile does not fault reserved pages in. Commit first; if a page in the span
// is already committed but not writable (BOTW_WATCH), copy through a host buffer so the watch
// still sees the user-mode store.
size_t read_guest(FILE* f, uint32_t dst, size_t want) {
    if (!want) return 0;
    if (mem::commit(dst, want, true)) return fread(mem::host(dst), 1, want, f);
    size_t got = 0;
    std::vector<uint8_t> tmp(std::min(want, (size_t)1 << 20));
    while (got < want) {
        size_t chunk = std::min(tmp.size(), want - got);
        size_t n = fread(tmp.data(), 1, chunk, f);
        if (n) memcpy(mem::host(dst + (uint32_t)got), tmp.data(), n);
        got += n;
        if (n < chunk) break;
    }
    return got;
}

size_t write_guest(FILE* f, uint32_t src, size_t want) {
    if (!want) return 0;
    if (mem::commit(src, want, false)) return fwrite(mem::host(src), 1, want, f);
    size_t put = 0;
    std::vector<uint8_t> tmp(std::min(want, (size_t)1 << 20));
    while (put < want) {
        size_t chunk = std::min(tmp.size(), want - put);
        memcpy(tmp.data(), mem::host(src + (uint32_t)put), chunk);
        size_t n = fwrite(tmp.data(), 1, chunk, f);
        put += n;
        if (n < chunk) break;
    }
    return put;
}

void fill_stat(uint32_t out, bool dir, uint64_t size) {
    mem::commit(out, 0x64, true);
    memset(mem::host(out), 0, 0x64);
    mem::w32(out + 0x00, dir ? FLAG_DIR : FLAG_FILE);
    mem::w32(out + 0x04, 0x666);
    uint32_t sz = dir ? 0 : (uint32_t)size;
    mem::w32(out + 0x10, sz);
    mem::w32(out + 0x14, sz);
}

int32_t fail(PPCContext* c, int32_t err, uint32_t mask, const char* what, const std::string& path) {
    // errHandling mask: bit set = the game handles this error itself. Others are fatal on hardware.
    bool handled = mask == 0xFFFFFFFFu || (err == FS_NOT_FOUND && (mask & 0x40)) || (err == FS_EXISTS && (mask & 0x20)) ||
                   (err == FS_NOT_FILE && (mask & 0x80)) || (err == FS_NOT_DIR && (mask & 0x100));
    TRACE("FS %s %s -> %d", what, path.c_str(), err);
    diag_fs(path.c_str(), "FS %s %s -> %d errno %d%s", what, path.c_str(), err, errno, handled ? " (handled)" : "");
    if (!handled) LOG("FS %s %s failed (%d) with errHandling %x", what, path.c_str(), err, mask);
    return err;
}

int32_t open_file(PPCContext* c, const std::string& raw, const char* mode, uint32_t handle_out, uint32_t mask) {
    std::string gpath = abs_guest(raw);
    std::string m = mode ? mode : "";
    bool reading = m.find('r') != std::string::npos && m.find('+') == std::string::npos && m.find('w') == std::string::npos &&
                   m.find('a') == std::string::npos;
    if (mods::read_only_volume(gpath) && !reading) {
        LOG("FS write blocked on %s (the game dump and mods are read-only)", gpath.c_str());
        return fail(c, FS_PERMISSION, mask, "open", gpath);
    }
    std::string hp;
    HostInfo info;
    bool overlay = false;
    mods::Query q = mods::query(gpath);
    if (q.kind != mods::Query::Kind::Other) {
        if (q.kind == mods::Query::Kind::Dir) return fail(c, FS_NOT_FILE, mask, "open", gpath);
        if (q.kind != mods::Query::Kind::File) {
            diag_fs(gpath.c_str(), "FS open miss %s", gpath.c_str());
            return fail(c, FS_NOT_FOUND, mask, "open", gpath);
        }
        hp = q.host;
        info.ok = true;
        info.size = q.size;
        overlay = q.overlay;
#if TARGET_OS_IOS
        hp = botw_ios_fix_case(hp);
#endif
    } else {
        hp = host_path(gpath);
        info = host_info(hp);
        // A 32-bit stat fails with EOVERFLOW on a large file. fopen can still open it.
        if (!info.ok && reading && info.err == EOVERFLOW) {
            FILE* probe = fopen(hp.c_str(), "rb");
            if (probe) {
                fclose(probe);
                info.ok = true;
                info.dir = false;
                diag_fs(gpath.c_str(), "FS stat EOVERFLOW on %s, fopen ok", hp.c_str());
            }
        }
        if (info.ok && info.dir) return fail(c, FS_NOT_FILE, mask, "open", gpath);
        if (!info.ok && reading) {
            diag_fs(gpath.c_str(), "FS open miss %s host %s errno %d", gpath.c_str(), hp.c_str(), info.err);
            return fail(c, FS_NOT_FOUND, mask, "open", gpath);
        }
        if (m.find('w') != std::string::npos || m.find('a') != std::string::npos) {
            size_t s = hp.find_last_of("/\\");
            if (s != std::string::npos) mkdirs(hp.substr(0, s));
        }
    }
    std::string hm;
    for (char ch : m)
        if (ch != 'b') hm += ch;
    hm += 'b';
    FILE* f = fopen(hp.c_str(), hm.c_str());
    if (!f) return fail(c, errno == ENOENT ? FS_NOT_FOUND : FS_PERMISSION, mask, "open", gpath);
    if (overlay) mods::note_opened(gpath);
    Handle opened;
    opened.f = f;
    opened.host = hp;
    std::lock_guard<std::mutex> l(g_fs_lock);
    uint32_t h = add_handle(std::move(opened));
    mem::w32(handle_out, h);
    diag_fs(gpath.c_str(), "FS open %s (%s) host %s size %llu%s -> %u", gpath.c_str(), mode ? mode : "", hp.c_str(),
            (unsigned long long)info.size, overlay ? " overlay" : "", h);
    TRACE("FS open %s (%s) -> %u", gpath.c_str(), m.c_str(), h);
    return FS_OK;
}

int32_t open_dir(PPCContext* c, const std::string& raw, uint32_t out, uint32_t mask) {
    std::string gpath = abs_guest(raw);
    std::vector<mods::DirItem> items;
    mods::ListStatus listed = mods::list_dir(gpath, items);
    if (listed != mods::ListStatus::NotVolume) {
        if (listed != mods::ListStatus::Ok) {
            diag_fs(gpath.c_str(), "FS opendir miss %s", gpath.c_str());
            return fail(c, FS_NOT_FOUND, mask, "opendir", gpath);
        }
        diag_fs(gpath.c_str(), "FS opendir merged %s (%zu entries)", gpath.c_str(), items.size());
        Handle opened;
        opened.items = std::move(items);
        opened.listed = true;
        opened.host = gpath;
        std::lock_guard<std::mutex> l(g_fs_lock);
        mem::w32(out, add_handle(std::move(opened)));
        return FS_OK;
    }
    std::string hp = host_path(gpath);
    DIR* d = opendir(hp.c_str());
    if (!d) {
        diag_fs(gpath.c_str(), "FS opendir miss %s host %s errno %d", gpath.c_str(), hp.c_str(), errno);
        return fail(c, FS_NOT_FOUND, mask, "opendir", gpath);
    }
    diag_fs(gpath.c_str(), "FS opendir %s host %s", gpath.c_str(), hp.c_str());
    Handle opened;
    opened.d = d;
    opened.host = hp;
    std::lock_guard<std::mutex> l(g_fs_lock);
    mem::w32(out, add_handle(std::move(opened)));
    return FS_OK;
}

int32_t get_stat(PPCContext* c, const std::string& raw, uint32_t out, uint32_t mask) {
    std::string gpath = abs_guest(raw);
    mods::Query q = mods::query(gpath);
    if (q.kind != mods::Query::Kind::Other) {
        if (q.kind == mods::Query::Kind::Missing) {
            diag_fs(gpath.c_str(), "FS stat miss %s", gpath.c_str());
            return fail(c, FS_NOT_FOUND, mask, "stat", gpath);
        }
        bool dir = q.kind == mods::Query::Kind::Dir;
        fill_stat(out, dir, q.size);
        diag_fs(gpath.c_str(), "FS stat %s host %s %s size %llu%s", gpath.c_str(), q.host.c_str(), dir ? "dir" : "file",
                (unsigned long long)q.size, q.overlay ? " overlay" : "");
        return FS_OK;
    }
    std::string hp = host_path(gpath);
    HostInfo info = host_info(hp);
    if (!info.ok) {
        diag_fs(gpath.c_str(), "FS stat miss %s host %s errno %d", gpath.c_str(), hp.c_str(), info.err);
        return fail(c, FS_NOT_FOUND, mask, "stat", gpath);
    }
    fill_stat(out, info.dir, info.size);
    diag_fs(gpath.c_str(), "FS stat %s host %s %s size %llu", gpath.c_str(), hp.c_str(), info.dir ? "dir" : "file",
            (unsigned long long)info.size);
    return FS_OK;
}

std::string save_path(uint32_t account, const char* path) {
    // nn_save's account argument is a slot (1 = nn::act slot 1, 0xFF = common), not a decimal id.
    // Slot 1 is persistent id 0x80000001. Wii U and Cemu name the directory with that id in hex,
    // which is the folder SAVEInit creates. std::to_string would print 2147483649 and miss the files.
    account &= 0xFFu;
    char folder[16];
    if (account == 0xFF) snprintf(folder, sizeof folder, "common");
    else snprintf(folder, sizeof folder, "%08x", 0x80000000u + account);
    std::string base = std::string("/vol/save/") + folder;
    std::string p = path ? path : "";
    if (!p.empty() && p[0] != '/') p = "/" + p;
    return base + p;
}
}  // namespace

HLE_EXPORT(coreinit, FSInit) {}
HLE_EXPORT(coreinit, FSShutdown) {}
HLE_EXPORT(coreinit, FSAddClient) { RET(0); }
HLE_EXPORT(coreinit, FSDelClient) { RET(0); }
HLE_EXPORT(coreinit, FSInitCmdBlock) {
    mem::commit(ARG(0), 0xA80, true);
    memset(mem::host(ARG(0)), 0, 0xA80);
}
HLE_EXPORT(coreinit, FSSetCmdPriority) { RET(0); }
HLE_EXPORT(coreinit, FSGetVolumeState) { RET(1); }
HLE_EXPORT(coreinit, FSGetLastError) { RET(0); }
HLE_EXPORT(coreinit, FSGetLastErrorCodeForViewer) { RET(0); }
HLE_EXPORT(coreinit, FSSetStateChangeNotification) {}

// FSOpenFile(client, block, path, mode, FSFileHandle* out, errHandling)
HLE_EXPORT(coreinit, FSOpenFile) { RET(open_file(c, mem::str(ARG(2)), mem::str(ARG(3)), ARG(4), ARG(5))); }
HLE_EXPORT(coreinit, FSCloseFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && h->f) {
        fclose(h->f);
        *h = Handle{};
    }
    RET(0);
}
// FSReadFile(client, block, dst, size, count, handle, flag, err) -> elements read
HLE_EXPORT(coreinit, FSReadFile) {
    uint32_t dst = ARG(2), size = ARG(3), count = ARG(4);
    Handle* h;
    std::string host;
    {
        std::lock_guard<std::mutex> l(g_fs_lock);
        h = get(ARG(5));
        if (h) host = h->host;
    }
    if (!h || !h->f) {
        diag_fs(host.c_str(), "FS read bad handle %u", ARG(5));
        RET(FS_FATAL);
        return;
    }
    size_t want = (size_t)size * count;
    size_t n = read_guest(h->f, dst, want);
    bool text = host.find("Region") != std::string::npos || host.find(".txt") != std::string::npos;
    if (diag_on() && (text || n != want || want <= 4096)) {
        char hex[96];
        hex[0] = 0;
        size_t nb = n < 32 ? n : 32;
        int p = 0;
        for (size_t i = 0; i < nb && p < (int)sizeof hex - 4; i++)
            p += snprintf(hex + p, sizeof hex - p, "%02x", mem::r8(dst + (uint32_t)i));
        diag_fs(host.c_str(), "FS read %s want %zu got %zu [%s]", host.c_str(), want, n, hex);
    }
    RET(size ? (uint32_t)(n / size) : 0);
}
HLE_EXPORT(coreinit, FSWriteFile) {
    uint32_t src = ARG(2), size = ARG(3), count = ARG(4);
    Handle* h;
    {
        std::lock_guard<std::mutex> l(g_fs_lock);
        h = get(ARG(5));
    }
    if (!h || !h->f) {
        RET(FS_FATAL);
        return;
    }
    size_t n = write_guest(h->f, src, (size_t)size * count);
    RET(size ? (uint32_t)(n / size) : 0);
}
HLE_EXPORT(coreinit, FSSetPosFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && h->f) {
#if defined(_WIN32)
        // Cafe's position is a uint32. A 32-bit off_t would treat values >= 2 GB as negative.
        _fseeki64(h->f, (__int64)ARG(3), SEEK_SET);
#else
        fseeko(h->f, (off_t)ARG(3), SEEK_SET);
#endif
    }
    RET(0);
}
HLE_EXPORT(coreinit, FSFlushFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && h->f) fflush(h->f);
    RET(0);
}
HLE_EXPORT(coreinit, FSGetStatFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (!h || !h->f) {
        RET(FS_FATAL);
        return;
    }
    fflush(h->f);
    HostInfo info = host_info(h->host);
    if (!info.ok) {
        RET(FS_FATAL);
        return;
    }
    fill_stat(ARG(3), info.dir, info.size);
    RET(0);
}
// FSGetStat(client, block, path, FSStat* out, err)
HLE_EXPORT(coreinit, FSGetStat) { RET(get_stat(c, mem::str(ARG(2)), ARG(3), ARG(4))); }
HLE_EXPORT(coreinit, FSOpenDir) { RET(open_dir(c, mem::str(ARG(2)), ARG(3), ARG(4))); }
// FSReadDir(client, block, dirHandle, FSDirEntry* out, err): 0 or -2 at the end
HLE_EXPORT(coreinit, FSReadDir) {
    uint32_t out = ARG(3);
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (!h || (!h->d && !h->listed)) {
        RET(FS_FATAL);
        return;
    }
    if (h->listed) {
        if (h->item_at >= h->items.size()) {
            RET(FS_END);
            return;
        }
        const mods::DirItem& e = h->items[h->item_at++];
        fill_stat(out, e.dir, e.size);
        mem::commit(out + 0x64, 0x80, true);
        memset(mem::host(out + 0x64), 0, 0x80);
        strncpy((char*)mem::host(out + 0x64), e.name.c_str(), 0x7F);
        RET(0);
        return;
    }
    for (;;) {
        struct dirent* e = readdir(h->d);
        if (!e) {
            RET(FS_END);
            return;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || e->d_name[0] == '.') continue;
#if defined(_WIN32)
        std::string full = h->host + "\\" + e->d_name;
#else
        std::string full = h->host + "/" + e->d_name;
#endif
        HostInfo info = host_info(full);
        if (!info.ok) continue;
        fill_stat(out, info.dir, info.size);
        mem::commit(out + 0x64, 0x80, true);
        memset(mem::host(out + 0x64), 0, 0x80);
        strncpy((char*)mem::host(out + 0x64), e->d_name, 0x7F);
        RET(0);
        return;
    }
}
HLE_EXPORT(coreinit, FSCloseDir) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && (h->d || h->listed)) {
        if (h->d) closedir(h->d);
        *h = Handle{};
    }
    RET(0);
}
HLE_EXPORT(coreinit, FSMakeDir) {
    std::string gpath = abs_guest(mem::str(ARG(2)));
    if (mods::read_only_volume(gpath)) {
        LOG("FS mkdir blocked on %s (the game dump and mods are read-only)", gpath.c_str());
        RET(fail(c, FS_PERMISSION, ARG(3), "mkdir", gpath));
        return;
    }
    std::string hp = host_path(gpath);
    if (host_info(hp).ok) {
        RET(fail(c, FS_EXISTS, ARG(3), "mkdir", gpath));
        return;
    }
    mkdirs(hp);
    RET(0);
}
HLE_EXPORT(coreinit, FSGetCwd) {
    uint32_t buf = ARG(2), len = ARG(3);
    if (len) {
        mem::commit(buf, len, true);
        strncpy((char*)mem::host(buf), g_cwd.c_str(), len - 1);
        mem::w8(buf + len - 1, 0);
    }
    RET(0);
}

// ---- nn_save: per-account save folders under /vol/save -------------------------------------------
HLE_EXPORT(nn_save, SAVEInit) {
    mkdirs(native_path(g_user_root + "/save/common"));
    mkdirs(native_path(g_user_root + "/save/80000001"));
    RET(0);
}
HLE_EXPORT(nn_save, SAVEShutdown) {}
HLE_EXPORT(nn_save, SAVEInitSaveDir) {
    mkdirs(host_path(save_path(ARG(0), "")));
    RET(0);
}
HLE_EXPORT(nn_save, SAVEFlushQuota) { RET(0); }
// SAVEGetStat(client, block, account, path, FSStat*, err)
HLE_EXPORT(nn_save, SAVEGetStat) { RET(get_stat(c, save_path(ARG(2), mem::str(ARG(3))), ARG(4), ARG(5))); }
// SAVEOpenFile(client, block, account, path, mode, handle*, err)
HLE_EXPORT(nn_save, SAVEOpenFile) {
    RET(open_file(c, save_path(ARG(2), mem::str(ARG(3))), mem::str(ARG(4)), ARG(5), ARG(6)));
}
HLE_EXPORT(nn_save, SAVEOpenDir) { RET(open_dir(c, save_path(ARG(2), mem::str(ARG(3))), ARG(4), ARG(5))); }
HLE_EXPORT(nn_save, SAVEMakeDir) {
    std::string hp = host_path(save_path(ARG(2), mem::str(ARG(3))));
    if (host_info(hp).ok) {
        RET(fail(c, FS_EXISTS, ARG(4), "mkdir", hp));
        return;
    }
    mkdirs(hp);
    RET(0);
}

// Remove a file or an empty directory.
static int32_t remove_path(PPCContext* c, const std::string& raw, uint32_t mask) {
    std::string gpath = abs_guest(raw);
    if (mods::read_only_volume(gpath)) {
        LOG("FS remove blocked on %s (the game dump and mods are read-only)", gpath.c_str());
        return fail(c, FS_PERMISSION, mask, "remove", gpath);
    }
    std::string hp = host_path(gpath);
    HostInfo info = host_info(hp);
    if (!info.ok) return fail(c, FS_NOT_FOUND, mask, "remove", gpath);
    if ((info.dir ? rmdir(hp.c_str()) : unlink(hp.c_str())) != 0) return fail(c, FS_PERMISSION, mask, "remove", gpath);
    TRACE("FS remove %s", gpath.c_str());
    return FS_OK;
}
// FSRemove(client, block, path, err)
HLE_EXPORT(coreinit, FSRemove) { RET(remove_path(c, mem::str(ARG(2)), ARG(3))); }
// SAVERemove(client, block, account, path, err)
HLE_EXPORT(nn_save, SAVERemove) { RET(remove_path(c, save_path(ARG(2), mem::str(ARG(3))), ARG(4))); }
