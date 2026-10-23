// Portable checks for save detection, zip extract, and controls.ini. Not part of the game.
#include "input/controls_file.h"
#include "save_import.h"
#include "unzip.h"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fails = 0;

void expect(bool cond, const std::string& msg) {
    if (cond) return;
    std::cerr << "FAIL " << msg << "\n";
    g_fails++;
}

void write_bytes(const fs::path& path, const std::vector<uint8_t>& b) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

std::vector<uint8_t> header(bool little, uint32_t version) {
    std::vector<uint8_t> b(16, 0);
    auto put = [&](int off, uint32_t v, bool le) {
        if (le) {
            b[off] = static_cast<uint8_t>(v);
            b[off + 1] = static_cast<uint8_t>(v >> 8);
            b[off + 2] = static_cast<uint8_t>(v >> 16);
            b[off + 3] = static_cast<uint8_t>(v >> 24);
        } else {
            b[off] = static_cast<uint8_t>(v >> 24);
            b[off + 1] = static_cast<uint8_t>(v >> 16);
            b[off + 2] = static_cast<uint8_t>(v >> 8);
            b[off + 3] = static_cast<uint8_t>(v);
        }
    };
    put(0, version, little);
    put(4, 0xFFFFFFFFu, false);
    put(8, 1, little);
    return b;
}

void w16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
void w32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 24));
}

struct ZipItem {
    std::string name;
    std::vector<uint8_t> data;
    bool deflate = false;
};

std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& in) {
    z_stream strm{};
    deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    strm.avail_in = static_cast<uInt>(in.size());
    std::vector<uint8_t> out(in.size() + 64);
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);
    if (rc != Z_STREAM_END) return {};
    out.resize(strm.total_out);
    return out;
}

std::vector<uint8_t> make_zip(const std::vector<ZipItem>& items) {
    std::vector<uint8_t> out;
    struct Cd {
        std::string name;
        uint16_t method;
        uint32_t comp, uncomp, local;
    };
    std::vector<Cd> cds;
    for (const auto& item : items) {
        std::vector<uint8_t> stored = item.data;
        uint16_t method = 0;
        if (item.deflate) {
            stored = deflate_raw(item.data);
            method = 8;
        }
        Cd cd{item.name, method, static_cast<uint32_t>(stored.size()), static_cast<uint32_t>(item.data.size()),
              static_cast<uint32_t>(out.size())};
        w32(out, 0x04034b50);
        w16(out, 20);
        w16(out, 0);
        w16(out, method);
        w16(out, 0);
        w16(out, 0);
        w32(out, 0);
        w32(out, cd.comp);
        w32(out, cd.uncomp);
        w16(out, static_cast<uint16_t>(item.name.size()));
        w16(out, 0);
        out.insert(out.end(), item.name.begin(), item.name.end());
        out.insert(out.end(), stored.begin(), stored.end());
        cds.push_back(cd);
    }
    uint32_t cd_off = static_cast<uint32_t>(out.size());
    for (const auto& cd : cds) {
        w32(out, 0x02014b50);
        w16(out, 20);
        w16(out, 20);
        w16(out, 0);
        w16(out, cd.method);
        w16(out, 0);
        w16(out, 0);
        w32(out, 0);
        w32(out, cd.comp);
        w32(out, cd.uncomp);
        w16(out, static_cast<uint16_t>(cd.name.size()));
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w16(out, 0);
        w32(out, 0);
        w32(out, cd.local);
        out.insert(out.end(), cd.name.begin(), cd.name.end());
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size() - cd_off);
    w32(out, 0x06054b50);
    w16(out, 0);
    w16(out, 0);
    w16(out, static_cast<uint16_t>(cds.size()));
    w16(out, static_cast<uint16_t>(cds.size()));
    w32(out, cd_size);
    w32(out, cd_off);
    w16(out, 0);
    return out;
}

fs::path scratch() {
    auto p = fs::temp_directory_path() / "botw-launcher-test";
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void test_versions(const fs::path& root) {
    auto dir = root / "ver";
    write_bytes(dir / "option.sav", header(false, 0x471B));
    write_bytes(dir / "0" / "game_data.sav", header(false, 0x471B));
    saveimport::Located loc;
    std::string err;
    expect(saveimport::locate(dir / "0" / "game_data.sav", loc, err), "walk up from game_data.sav: " + err);
    expect(!loc.dangerous && !loc.caution, "1.5.0 wii u should be clean");
    expect(loc.slots == 1 && loc.has_option, "slot and option counted");
    expect(loc.files.size() >= 2, "both savs inspected");
    for (const auto& f : loc.files) {
        expect(f.endian == saveimport::Endian::WiiU, "wii u endian");
        expect(f.age == saveimport::Age::Exact150, "exact 1.5.0");
        expect(f.version_name == "1.5.0", "version name");
    }

    auto sw = root / "switch";
    write_bytes(sw / "option.sav", header(true, 0x471B));
    write_bytes(sw / "0" / "game_data.sav", header(true, 0x471B));
    expect(saveimport::locate(sw, loc, err), "switch locate");
    expect(loc.dangerous, "switch is dangerous");
    expect(loc.warnings.find("Switch") != std::string::npos, "switch warning text");

    auto neu = root / "new";
    write_bytes(neu / "0" / "game_data.sav", header(false, 0x471E));
    expect(saveimport::locate(neu, loc, err), "1.6 locate");
    expect(loc.dangerous, "1.6 is dangerous");
    expect(loc.warnings.find("1.6.0") != std::string::npos, "1.6 name");

    auto old = root / "old";
    write_bytes(old / "option.sav", header(false, 0x471A));
    write_bytes(old / "0" / "game_data.sav", header(false, 0x471A));
    expect(saveimport::locate(old, loc, err), "1.4 locate");
    expect(!loc.dangerous && loc.caution, "older is a caution, not a hard stop");
    expect(loc.warnings.find("1.4.0") != std::string::npos, "1.4 name");
}

void test_layouts(const fs::path& root) {
    auto cemu = root / "mlc01" / "usr" / "save" / "00050000" / "101C9400" / "user";
    write_bytes(cemu / "80000001" / "option.sav", header(false, 0x471B));
    write_bytes(cemu / "80000001" / "0" / "game_data.sav", header(false, 0x471B));
    write_bytes(cemu / "80000001" / "5" / "caption.sav", header(false, 0x471B));
    write_bytes(cemu / "common" / "extra.sav", header(false, 0x471B));
    saveimport::Located loc;
    std::string err;
    expect(saveimport::locate(root / "mlc01", loc, err), "cemu tree: " + err);
    expect(loc.account_name == "80000001", "account name");
    expect(loc.title_id == "101c9400", "usa title id");
    expect(loc.slots == 2, "two slots");
    expect(!loc.common_dir.empty(), "common sibling");
    expect(!loc.dangerous, "usa 1.5 is clean");

    auto eur = root / "eur" / "user" / "80000001";
    // parent of user must be the title id
    auto eur_user = root / "dump" / "00050000" / "101c9500" / "user" / "80000002";
    write_bytes(eur_user / "option.sav", header(false, 0x471B));
    expect(saveimport::locate(eur_user, loc, err), "eur account");
    expect(loc.title_id == "101c9500", "eur title");
    expect(!loc.dangerous, "eur is botw");
    expect(loc.note.find("80000002") != std::string::npos, "rename note");

    auto foreign = root / "other" / "00050000" / "deadbeef" / "user" / "80000001";
    write_bytes(foreign / "option.sav", header(false, 0x471B));
    expect(saveimport::locate(foreign, loc, err), "foreign title");
    expect(loc.dangerous, "unknown title is dangerous");
    expect(loc.warnings.find("DEADBEEF") != std::string::npos, "title id in the warning");
    (void)eur;
}

void test_zip_and_swap(const fs::path& root) {
    auto src = root / "zip-src";
    auto body = header(false, 0x471B);
    body.insert(body.end(), {'s', 'a', 'v', 'e'});
    std::vector<ZipItem> items = {{"80000001/option.sav", body, true}, {"80000001/0/game_data.sav", body, false}};
    auto zip = make_zip(items);
    auto zip_path = root / "save.zip";
    write_bytes(zip_path, zip);
    auto unpacked = root / "unpacked";
    std::string err;
    expect(unzip_to(zip_path, unpacked, err), "unzip deflate+store: " + err);
    expect(fs::is_regular_file(unpacked / "80000001" / "0" / "game_data.sav"), "slot extracted");
    saveimport::Located loc;
    expect(saveimport::locate(unpacked, loc, err), "locate unzipped: " + err);
    expect(loc.slots == 1 && loc.has_option, "zip layout");

    auto evil = root / "evil.zip";
    write_bytes(evil, make_zip({{"../escaped.txt", {'n', 'o'}, false}}));
    auto evil_dest = root / "evil-out";
    fs::create_directories(evil_dest);
    expect(!unzip_to(evil, evil_dest, err), "zip slip rejected");
    expect(!fs::exists(root / "escaped.txt"), "zip slip did not write");
    expect(err.find("escapes") != std::string::npos, "zip slip message");

    auto user = root / "user";
    write_bytes(saveimport::live_account_dir(user) / "option.sav", header(false, 0x471A));
    write_bytes(saveimport::live_account_dir(user) / "marker.txt", {'o', 'l', 'd'});
    auto imported = saveimport::import_account(user, loc, "save.zip");
    expect(imported.ok, "import: " + imported.message);
    expect(fs::is_regular_file(saveimport::live_account_dir(user) / "0" / "game_data.sav"), "live slot installed");
    expect(!imported.backup.empty(), "previous save backed up");
    expect(fs::is_regular_file(imported.backup / "account" / "marker.txt"), "backup kept the old marker");
    auto sets = saveimport::list_sets(user);
    expect(sets.size() == 2, "backup and import sets");

    auto back = saveimport::switch_to(user, imported.backup);
    expect(back.ok, "switch back: " + back.message);
    expect(fs::is_regular_file(saveimport::live_account_dir(user) / "marker.txt"), "old marker restored");
    expect(!fs::exists(saveimport::live_account_dir(user) / "0" / "game_data.sav"), "imported slot not left behind");

    auto removed = saveimport::delete_set(user, imported.imported);
    expect(removed.ok, "delete set");
    expect(!fs::exists(imported.imported), "set directory gone");
    expect(fs::exists(saveimport::live_account_dir(user) / "marker.txt"), "delete did not touch the live save");

    write_bytes(user / "save" / "2147483649" / "option.sav", header(false, 0x471B));
    fs::remove_all(saveimport::live_account_dir(user));
    expect(saveimport::decimal_legacy_present(user), "decimal legacy folder detected");
    expect(saveimport::decimal_legacy_folder(user), "decimal folder is still a save");
    write_bytes(saveimport::live_account_dir(user) / "option.sav", header(false, 0x471B));
    write_bytes(saveimport::live_account_dir(user) / "marker.txt", {'n', 'e', 'w'});
    expect(!saveimport::decimal_legacy_present(user), "live save hides the empty-slot hint");
    auto moved = saveimport::migrate_decimal(user);
    expect(moved.ok, "migrate: " + moved.message);
    expect(fs::is_regular_file(saveimport::live_account_dir(user) / "option.sav"), "decimal save copied to 80000001");
    expect(!fs::exists(saveimport::live_account_dir(user) / "marker.txt"), "live marker replaced");
    expect(fs::is_regular_file(user / "save" / "2147483649" / "option.sav"), "decimal folder was not moved");
    expect(!moved.backup.empty(), "existing 80000001 was backed up");
    expect(fs::is_regular_file(moved.backup / "account" / "marker.txt"), "backup kept the newer marker");
}

void test_controls(const fs::path& root) {
    auto rows = controlsfile::defaults();
    expect(rows.size() >= 24, "default action count");
    expect(rows[0].id == "a" && rows[0].key == "E" && rows[0].pad == "auto", "A defaults");
    bool saw_shift = false, saw_wasd = false;
    for (const auto& r : rows) {
        if (r.id == "b") {
            saw_shift = r.key == "Left Shift" && r.key2 == "Right Shift" && r.pad == "auto";
        }
        if (r.id == "lstick_up") saw_wasd = r.key == "W" && r.pad == "lefty+";
    }
    expect(saw_shift, "B is Left Shift / Right Shift / auto");
    expect(saw_wasd, "left stick up is W / lefty+");

    auto path = root / "controls.ini";
    std::string err;
    expect(controlsfile::save(path, rows, err), err);
    std::vector<controlsfile::Binding> loaded;
    expect(controlsfile::load(path, loaded, err) == controlsfile::LoadStatus::Ok, "reload");
    expect(loaded.size() == rows.size(), "round trip count");
    auto shown = controlsfile::editor_rows(true, loaded);
    expect(shown.size() == rows.size() && shown[1].key2 == "Right Shift", "editor keeps key2");

    std::ofstream partial(root / "partial.ini");
    partial << "a=key:Z,pad:east\n";
    partial.close();
    loaded.clear();
    expect(controlsfile::load(root / "partial.ini", loaded, err) == controlsfile::LoadStatus::Ok, "partial");
    shown = controlsfile::editor_rows(true, loaded);
    expect(shown[0].key == "Z", "overlay A");
    expect(shown[1].key.empty() && shown[1].pad.empty(), "omitted action is unbound");

    std::string canon, perr;
    expect(controlsfile::canonical_pad("LeftTrigger", canon, perr) && canon == "lefttrigger", "trigger alias");
    expect(controlsfile::canonical_pad("dpad_up", canon, perr) && canon == "dpup", "dpad alias");
    expect(!controlsfile::canonical_pad("not-a-button", canon, perr), "reject unknown pad");
    expect(controlsfile::load(root / "missing.ini", loaded, err) == controlsfile::LoadStatus::Missing, "missing file");
    auto defs = controlsfile::editor_rows(false, {});
    expect(defs[0].key == "E", "missing file shows defaults");
}

}  // namespace

int main() {
    auto root = scratch();
    test_versions(root);
    test_layouts(root);
    test_zip_and_swap(root);
    test_controls(root);
    fs::remove_all(root);
    if (g_fails) {
        std::cerr << g_fails << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
