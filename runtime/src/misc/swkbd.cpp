// Wii U software keyboard HLE. The guest ABI and structure offsets follow Cemu's
// swkbd implementation, which is available under the Mozilla Public License 2.0.
#include "hle.h"
#include "host/swkbd_state.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
swkbd::Keyboard g_keyboard;
std::mutex g_guest_lock;
uint32_t g_string = 0, g_dirty_info = 0;
uint32_t g_receiver = 0, g_receiver_buffer = 0, g_receiver_capacity = 0;

void ensure_buffers() {
    if (!g_string) g_string = mem::sys_alloc(8192, 16);
    if (!g_dirty_info) g_dirty_info = mem::sys_alloc(8, 4);
}

std::u16string read_string(uint32_t addr, size_t max) {
    std::u16string result;
    if (!addr) return result;
    for (size_t i = 0; i < max; ++i) {
        uint64_t pos = uint64_t(addr) + i * 2;
        if (pos > UINT32_MAX || !mem::commit(static_cast<uint32_t>(pos), 2, false)) break;
        char16_t ch = mem::r16(static_cast<uint32_t>(pos));
        if (!ch) break;
        result.push_back(ch);
    }
    return result;
}

void set_receiver(uint32_t arg) {
    std::lock_guard<std::mutex> lock(g_guest_lock);
    g_receiver = g_receiver_buffer = g_receiver_capacity = 0;
    if (!arg || !mem::commit(arg, 0x18, false)) return;
    g_receiver = mem::r32(arg);
    g_receiver_buffer = mem::r32(arg + 4);
    g_receiver_capacity = std::min<uint32_t>(mem::r32(arg + 8), 4096);
}

uint32_t publish(PPCContext* context, bool notify) {
    bool changed = false;
    std::u16string text = g_keyboard.text(notify ? &changed : nullptr);
    uint32_t result, receiver, buffer, capacity, dirty_info;
    {
        std::lock_guard<std::mutex> lock(g_guest_lock);
        ensure_buffers();
        result = g_string;
        receiver = g_receiver;
        buffer = g_receiver_buffer;
        capacity = g_receiver_capacity;
        dirty_info = g_dirty_info;
    }
    if (result && mem::commit(result, (text.size() + 1) * 2, true)) {
        for (size_t i = 0; i < text.size(); ++i) mem::w16(result + static_cast<uint32_t>(i * 2), text[i]);
        mem::w16(result + static_cast<uint32_t>(text.size() * 2), 0);
    }
    if (changed && buffer && capacity && mem::commit(buffer, capacity * 2, true)) {
        size_t count = std::min(text.size(), static_cast<size_t>(capacity - 1));
        if (count && text[count - 1] >= 0xd800 && text[count - 1] <= 0xdbff) --count;
        for (size_t i = 0; i < count; ++i) mem::w16(buffer + static_cast<uint32_t>(i * 2), text[i]);
        mem::w16(buffer + static_cast<uint32_t>(count * 2), 0);
    }
    if (changed && context && receiver && dirty_info && mem::commit(receiver, 4, false)) {
        uint32_t vtable = mem::r32(receiver);
        if (vtable && mem::commit(vtable + 12, 4, false)) {
            uint32_t callback = mem::r32(vtable + 12);
            if (callback && mem::commit(dirty_info, 8, true)) {
                mem::w32(dirty_info, 0);
                mem::w32(dirty_info + 4, static_cast<uint32_t>(text.size()));
                guest_call(context, callback, {receiver, dirty_info});
            }
        }
    }
    return result;
}
}  // namespace

namespace swkbd {
Keyboard& keyboard() { return g_keyboard; }
}

HLE_EXPORT(swkbd, SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient) {
    std::lock_guard<std::mutex> lock(g_guest_lock);
    ensure_buffers();
    g_receiver = g_receiver_buffer = g_receiver_capacity = 0;
    g_keyboard.close();
    RET(0);
}
HLE_EXPORT(swkbd, SwkbdDestroy__3RplFv) { g_keyboard.close(); RET(0); }
HLE_EXPORT(swkbd, SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg) {
    set_receiver(ARG(0)); RET(0);
}
HLE_EXPORT(swkbd, SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg) {
    uint32_t arg = ARG(0), initial = 0, limit = 0;
    if (arg && mem::commit(arg, 0xd4, false)) {
        initial = mem::r32(arg + 0xc8);
        limit = mem::r32(arg + 0xd0);
    }
    if (static_cast<int32_t>(limit) <= 0) limit = 4095;
    limit = std::min(limit, 4095u);
    g_keyboard.show(read_string(initial, limit), limit);
    RET(1);
}
HLE_EXPORT(swkbd, SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg) {
    uint32_t arg = ARG(0);
    if (arg && mem::commit(arg, 0xc0, false)) set_receiver(arg + 0xa8);
    uint32_t buffer, capacity;
    { std::lock_guard<std::mutex> lock(g_guest_lock); buffer = g_receiver_buffer; capacity = g_receiver_capacity; }
    g_keyboard.show(read_string(buffer, capacity ? capacity - 1 : 0), capacity > 1 ? capacity - 1 : 4095);
    RET(1);
}
HLE_EXPORT(swkbd, SwkbdDisappearInputForm__3RplFv) { g_keyboard.close(); RET(1); }
HLE_EXPORT(swkbd, SwkbdDisappearKeyboard__3RplFv) { g_keyboard.close(); RET(1); }
HLE_EXPORT(swkbd, SwkbdGetStateKeyboard__3RplFv) { RET(g_keyboard.view().active ? 2 : 0); }
HLE_EXPORT(swkbd, SwkbdGetStateInputForm__3RplFv) { RET(g_keyboard.view().active ? 2 : 0); }
HLE_EXPORT(swkbd, SwkbdGetInputFormString__3RplFv) { RET(publish(nullptr, false)); }
HLE_EXPORT(swkbd, SwkbdSetInputFormString__3RplFPCw) { g_keyboard.replace(read_string(ARG(0), 4095)); RET(0); }
HLE_EXPORT(swkbd, SwkbdIsDecideOkButton__3RplFPb) { RET(g_keyboard.view().accepted ? 1 : 0); }
HLE_EXPORT(swkbd, SwkbdIsDecideCancelButton__3RplFPb) { RET(g_keyboard.view().cancelled ? 1 : 0); }
HLE_EXPORT(swkbd, SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo) { publish(c, true); RET(0); }
HLE_EXPORT(swkbd, SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo) {
    uint32_t out = ARG(0);
    if (out && mem::commit(out, 0x19, true)) {
        for (uint32_t i = 0; i < 0x18; i += 4) mem::w32(out + i, 0xffffffffu);
        mem::w8(out + 0x18, 0);
    }
    RET(0);
}
HLE_EXPORT(swkbd, SwkbdInitLearnDic__3RplFPv) {
    uint32_t out = ARG(0);
    constexpr uint32_t size = 0xa460, first = 0x48, second = first + 1001 * 2;
    constexpr uint32_t table_end = second + 1001 * 2, entries_end = table_end + 1000 * 0x20;
    if (!out || !mem::commit(out, size, true)) { RET(0); return; }
    std::memset(mem::host(out), 0, size);
    mem::w32(out, 0x4e4a4443); mem::w32(out + 4, 0x30000); mem::w32(out + 8, 0x80020000);
    mem::w32(out + 0xc, size - 0x48); mem::w32(out + 0x10, 0x2c);
    mem::w32(out + 0x14, 0x6e); mem::w32(out + 0x18, 0x6e); mem::w32(out + 0x1c, table_end);
    mem::w32(out + 0x28, 1000); mem::w32(out + 0x2c, 0x20);
    mem::w32(out + 0x3c, first); mem::w32(out + 0x40, second);
    mem::w32(out + 0x44, entries_end); mem::w32(out + size - 4, 0x4e4a4443);
    RET(1);
}

#define SWKBD_NOOP(name) HLE_EXPORT(swkbd, name) { RET(0); }
SWKBD_NOOP(SwkbdSetVersion__3RplFi)
SWKBD_NOOP(SwkbdDrawTV__3RplFv)
SWKBD_NOOP(SwkbdDrawDRC__3RplFv)
SWKBD_NOOP(SwkbdCalcSubThreadFont__3RplFv)
SWKBD_NOOP(SwkbdCalcSubThreadPredict__3RplFv)
SWKBD_NOOP(SwkbdIsNeedCalcSubThreadFont__3RplFv)
SWKBD_NOOP(SwkbdIsNeedCalcSubThreadPredict__3RplFv)
SWKBD_NOOP(SwkbdIsCoveredWithSubWindow__3RplFv)
SWKBD_NOOP(SwkbdInactivateSelectCursor__3RplFv)
SWKBD_NOOP(SwkbdSetControllerRemo__3RplFQ3_2nn5swkbd14ControllerType)
SWKBD_NOOP(SwkbdSetCursorPos__3RplFi)
SWKBD_NOOP(SwkbdMuteAllSound__3RplFb)
SWKBD_NOOP(SwkbdConfirmUnfixAll__3RplFv)
SWKBD_NOOP(SwkbdSetEnableOkButton__3RplFb)
SWKBD_NOOP(SwkbdSetSelectFrom__3RplFi)
SWKBD_NOOP(SwkbdSetUserSoundObj__3RplFPQ3_2nn5swkbd9ISoundObj)
SWKBD_NOOP(SwkbdGetKeyboardCondition__3RplFPQ3_2nn5swkbd17KeyboardCondition)
SWKBD_NOOP(SwkbdSetUserControllerEventObj__3RplFPQ3_2nn5swkbd19IControllerEventObj)
#undef SWKBD_NOOP

HLE_EXPORT(swkbd, SwkbdIsKeyboardTarget__3RplFPQ3_2nn5swkbd14IEventReceiver) {
    RET(g_keyboard.view().active ? 1 : 0);
}
HLE_EXPORT(swkbd, SwkbdIsSelectCursorActive__3RplFv) { RET(g_keyboard.view().active ? 1 : 0); }
