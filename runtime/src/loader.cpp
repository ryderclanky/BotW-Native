#include "loader.h"

#include <stdio.h>
#include <zlib.h>

#include <map>
#include <vector>

#include "guest_table.h"
#include "hle.h"
#include "utf8_path.h"

namespace loader {
namespace {
Module g_main;

struct Shdr {
    uint32_t name, type, flags, addr, offset, size, link, info, align, entsize;
};
constexpr uint32_t SHT_SYMTAB = 2, SHT_RELA = 4, SHT_NOBITS = 8, SHT_RPL_IMPORTS = 0x80000002,
                   SHT_RPL_FILEINFO = 0x80000004;
constexpr uint32_t SHF_ALLOC = 2, SHF_EXECINSTR = 4, SHF_TLS = 0x400, SHF_RPL_ZLIB = 0x08000000;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

struct DataImportDef {
    uint32_t size;
    void (*init)(uint32_t);
    uint32_t addr = 0;
};
std::map<std::string, DataImportDef>& data_defs() {
    static std::map<std::string, DataImportDef> d;
    return d;
}
}  // namespace

const Module& main_module() { return g_main; }

void define_data_import(const char* lib, const char* name, uint32_t size, void (*init)(uint32_t)) {
    data_defs()[std::string(lib) + "." + name] = {size, init};
}
uint32_t data_import_addr(const char* lib, const char* name) {
    auto it = data_defs().find(std::string(lib) + "." + name);
    return it == data_defs().end() ? 0 : it->second.addr;
}

bool load_main(const std::string& path) {
    FILE* f = botw::fopen_utf8(path, "rb");
    if (!f) {
        LOG("loader: cannot open %s", path.c_str());
        return false;
    }
    std::vector<uint8_t> raw;
    fseek(f, 0, SEEK_END);
    raw.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(raw.data(), 1, raw.size(), f);
    fclose(f);
    const uint8_t* h = raw.data();
    if (be32(h) != 0x7F454C46) return false;
    g_main.path = path;
    g_main.entry = be32(h + 0x18);
    uint32_t shoff = be32(h + 0x20);
    uint16_t shentsize = be16(h + 0x2E), shnum = be16(h + 0x30);
    std::vector<Shdr> sh(shnum);
    std::vector<std::vector<uint8_t>> data(shnum);
    for (int i = 0; i < shnum; i++) {
        const uint8_t* p = h + shoff + i * shentsize;
        Shdr& s = sh[i];
        uint32_t* fld = &s.name;
        for (int k = 0; k < 10; k++) fld[k] = be32(p + 4 * k);
        if (s.type == SHT_NOBITS || !s.size) continue;
        const uint8_t* src = h + s.offset;
        if (s.flags & SHF_RPL_ZLIB) {
            uLongf usize = be32(src);
            data[i].resize(usize);
            if (uncompress(data[i].data(), &usize, src + 4, s.size - 4) != Z_OK) {
                LOG("loader: zlib error in section %d", i);
                return false;
            }
            s.size = (uint32_t)usize;
        } else {
            data[i].assign(src, src + s.size);
        }
    }
    // file info: SDA bases, stack size, TLS module index
    for (int i = 0; i < shnum; i++) {
        if (sh[i].type != SHT_RPL_FILEINFO) continue;
        const uint8_t* fi = data[i].data();
        g_main.sda_base = be32(fi + 0x24);
        g_main.sda2_base = be32(fi + 0x28);
        g_main.stack_size = be32(fi + 0x2C);
        g_main.tls_module = be16(fi + 0x58);
    }
    // place allocated sections at their link addresses
    for (int i = 0; i < shnum; i++) {
        Shdr& s = sh[i];
        if (!(s.flags & SHF_ALLOC) || !s.size || s.type == SHT_RPL_IMPORTS) continue;
        mem::commit(s.addr, s.size, true);
        if (s.type == SHT_NOBITS) memset(mem::host(s.addr), 0, s.size);
        else memcpy(mem::host(s.addr), data[i].data(), s.size);
        if (s.flags & SHF_EXECINSTR) {
            if (!g_main.text_start || s.addr < g_main.text_start) g_main.text_start = s.addr;
            g_main.text_end = std::max(g_main.text_end, s.addr + s.size);
        } else if (s.addr >= mem::kMem2 && s.addr < mem::kMem2 + mem::kMem2Size) {
            g_main.data_end = std::max(g_main.data_end, s.addr + s.size);
        }
        if (s.flags & SHF_TLS) {
            g_main.tls_size += s.size;
            if (s.type != SHT_NOBITS) {
                g_main.tls_image = s.addr;
                g_main.tls_image_size = s.size;
            }
        }
    }
    // import stubs: function imports dispatch to HLE, data imports get runtime storage
    std::map<uint32_t, GuestFn> stub_fn;
    for (uint32_t i = 0; i < g_guest_import_count; i++) {
        const GuestImportEntry& e = g_guest_imports[i];
        if (e.name[0] == '.') continue;  // section symbol
        if (!e.is_data) {
            dispatch::add_import_stub(e.addr, e.fn, e.lib, e.name);
            mem::w32(e.addr, 0x4E800020);
        }
    }
    // Data imports live at the fixed slots the recompiler assigned (the table's address).
    for (uint32_t i = 0; i < g_guest_import_count; i++) {
        const GuestImportEntry& e = g_guest_imports[i];
        if (!e.is_data) continue;
        auto it = data_defs().find(std::string(e.lib) + "." + e.name);
        if (it == data_defs().end()) {
            LOG("loader: data import %s.%s has no definition (left zeroed)", e.lib, e.name);
            continue;
        }
        if (it->second.size > 0x1000) LOG("loader: data import %s too large for its slot", e.name);
        it->second.addr = e.addr;
        mem::commit(e.addr, 0x1000, true);
        memset(mem::host(e.addr), 0, 0x1000);
        if (it->second.init) it->second.init(e.addr);
    }
    // relocations
    int symtab = -1;
    for (int i = 0; i < shnum; i++)
        if (sh[i].type == SHT_SYMTAB) symtab = i;
    const uint8_t* syms = data[symtab].data();
    const uint8_t* strtab = data[sh[symtab].link].data();
    std::vector<uint32_t> symval(sh[symtab].size / 16);
    int missing_data = 0;
    for (size_t j = 0; j < symval.size(); j++) {
        const uint8_t* sp = syms + j * 16;
        uint32_t value = be32(sp + 4);
        uint16_t shndx = be16(sp + 14);
        symval[j] = value;
        if (shndx && shndx < shnum && sh[shndx].type == SHT_RPL_IMPORTS) {
            const char* nm = (const char*)strtab + be32(sp);
            bool is_data = !(sh[shndx].flags & SHF_EXECINSTR);
            if (is_data && nm[0] != '.') {
                uint32_t addr = 0;
                for (uint32_t k = 0; k < g_guest_import_count; k++)
                    if (g_guest_imports[k].is_data && !strcmp(g_guest_imports[k].name, nm)) addr = g_guest_imports[k].addr;
                if (!addr) missing_data++;
                symval[j] = addr;
            }
        }
    }
    uint32_t nrel = 0;
    for (int i = 0; i < shnum; i++) {
        if (sh[i].type != SHT_RELA) continue;
        const uint8_t* rp = data[i].data();
        for (uint32_t off = 0; off + 12 <= sh[i].size; off += 12) {
            uint32_t at = be32(rp + off), info = be32(rp + off + 4);
            int32_t addend = (int32_t)be32(rp + off + 8);
            uint32_t type = info & 0xFF, sym = info >> 8;
            if (sym >= symval.size()) continue;
            uint32_t v = symval[sym] + addend;
            uint8_t* p = mem::host(at);
            switch (type) {
                case 0: break;
                case 1: mem::w32(at, v); break;
                case 4: mem::w16(at, (uint16_t)v); break;
                case 5: mem::w16(at, (uint16_t)(v >> 16)); break;
                case 6: mem::w16(at, (uint16_t)((v + 0x8000) >> 16)); break;
                case 10: {  // REL24: only text, which is never executed from guest memory; keep in range
                    uint32_t d = v - at;
                    uint32_t insn = mem::r32(at);
                    mem::w32(at, (insn & 0xFC000003) | (d & 0x03FFFFFC));
                    break;
                }
                case 11: {
                    uint32_t d = v - at;
                    mem::w32(at, (mem::r32(at) & 0xFFFF0003) | (d & 0xFFFC));
                    break;
                }
                case 68: mem::w32(at, g_main.tls_module); break;  // DTPMOD32
                case 78: mem::w32(at, v); break;                  // DTPREL32
                default:
                    LOG("loader: unhandled relocation type %u at %08x", type, at);
            }
            (void)p;
            nrel++;
        }
    }
    LOG("loader: %s entry %08x text %08x-%08x data end %08x sda %08x/%08x stack %x tls mod %u size %x, %u relocs%s",
        path.c_str(), g_main.entry, g_main.text_start, g_main.text_end, g_main.data_end, g_main.sda_base,
        g_main.sda2_base, g_main.stack_size, g_main.tls_module, g_main.tls_size, nrel,
        missing_data ? " (missing data imports!)" : "");
    return true;
}
}  // namespace loader
