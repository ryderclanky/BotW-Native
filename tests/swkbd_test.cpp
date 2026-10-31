#include "hle.h"
#include "host/swkbd_state.h"
#include <cstdio>
#include <cstdlib>

namespace {
uint8_t memory[1024 * 1024]{};
uint32_t next_address = 0x10000;
int callbacks = 0;
}
extern "C" { uint8_t* g_mem = memory; }
namespace mem {
uint64_t g_fast_commit_end = 0;
bool commit_slow(uint32_t addr, size_t size, bool) { return uint64_t(addr) + size <= sizeof(memory); }
uint32_t sys_alloc(uint32_t size, uint32_t align) {
    next_address = (next_address + align - 1) & ~(align - 1);
    uint32_t address = next_address;
    next_address += size;
    return next_address <= sizeof(memory) ? address : 0;
}
}
HleRegistration::HleRegistration(const char*, const char*, GuestFn) {}
uint32_t guest_call(PPCContext*, uint32_t fn, std::initializer_list<uint32_t> args) {
    if (fn != 0x1234 || args.size() != 2) std::abort();
    ++callbacks;
    return 0;
}

extern "C" void hle_swkbd_SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient(PPCContext*);
extern "C" void hle_swkbd_SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg(PPCContext*);
extern "C" void hle_swkbd_SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg(PPCContext*);
extern "C" void hle_swkbd_SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo(PPCContext*);
extern "C" void hle_swkbd_SwkbdGetInputFormString__3RplFv(PPCContext*);
extern "C" void hle_swkbd_SwkbdIsDecideOkButton__3RplFPb(PPCContext*);
extern "C" void hle_swkbd_SwkbdDisappearInputForm__3RplFv(PPCContext*);

int main() {
    PPCContext context{};
    hle_swkbd_SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient(&context);
    constexpr uint32_t receiver_arg = 0x1000, receiver = 0x1100, vtable = 0x1200, buffer = 0x1300;
    mem::w32(receiver_arg, receiver); mem::w32(receiver_arg + 4, buffer); mem::w32(receiver_arg + 8, 8);
    mem::w32(receiver, vtable); mem::w32(vtable + 12, 0x1234);
    context.r[3] = receiver_arg;
    hle_swkbd_SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg(&context);
    constexpr uint32_t appear = 0x2000, initial = 0x2100;
    mem::w16(initial, 'A'); mem::w16(initial + 2, 0);
    mem::w32(appear + 0xc8, initial); mem::w32(appear + 0xd0, 4);
    context.r[3] = appear;
    hle_swkbd_SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg(&context);
    if (context.r[3] != 1 || !swkbd::keyboard().view().active) return 1;
    swkbd::keyboard().input("\xc3\xa9\xf0\x9f\x98\x80");
    swkbd::keyboard().input("X");
    if (swkbd::keyboard().text().size() != 4) return 2;
    if (swkbd::keyboard().view().text != "A\xc3\xa9\xf0\x9f\x98\x80") return 7;
    swkbd::keyboard().input("\xc0\xaf");  // reject overlong / malformed UTF-8
    if (swkbd::keyboard().text().size() != 4) return 8;
    swkbd::keyboard().backspace();
    swkbd::keyboard().input("Z");
    hle_swkbd_SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo(&context);
    if (callbacks != 1 || mem::r16(buffer) != 'A' || mem::r16(buffer + 2) != 0xe9 ||
        mem::r16(buffer + 4) != 'Z' || mem::r16(buffer + 6) != 0) return 3;
    hle_swkbd_SwkbdGetInputFormString__3RplFv(&context);
    if (!context.r[3] || mem::r16(context.r[3] + 4) != 'Z') return 4;
    swkbd::keyboard().accept();
    hle_swkbd_SwkbdIsDecideOkButton__3RplFPb(&context);
    if (context.r[3] != 1) return 5;
    hle_swkbd_SwkbdDisappearInputForm__3RplFv(&context);
    if (swkbd::keyboard().view().active) return 6;
    std::puts("software keyboard tests passed");
}
