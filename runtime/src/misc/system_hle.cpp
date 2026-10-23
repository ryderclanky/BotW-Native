// proc_ui, sysapp, nn_act, nn_boss, nn_nfp, nn_aoc: the app is always in the foreground, one local
// account (slot 1), no network services, no amiibo. DLC is reported only when AocMainField.pack
// is visible through the mod overlay or a user/game aoc folder (coreinit/mods.cpp).
#include <algorithm>
#include <cstring>

#include "../coreinit/mods.h"
#include "../hle.h"
#include "../os.h"

// ---- proc_ui -------------------------------------------------------------------------------------
HLE_EXPORT(proc_ui, ProcUIInit) {}
HLE_EXPORT(proc_ui, ProcUIShutdown) {}
HLE_EXPORT(proc_ui, ProcUIRegisterCallback) {}
HLE_EXPORT(proc_ui, ProcUIDrawDoneRelease) {}
// ProcUIStatus ProcUIProcessMessages(bool block): 0 = foreground
HLE_EXPORT(proc_ui, ProcUIProcessMessages) { RET(0); }

// ---- sysapp ----------------------------------------------------------------------------------------
// SYSGetArguments(SysArgs* out): no launch arguments
HLE_EXPORT(sysapp, SYSGetArguments) {
    if (ARG(0)) {
        mem::commit(ARG(0), 0x10, true);
        memset(mem::host(ARG(0)), 0, 0x10);
    }
    RET(0);
}
HLE_EXPORT(sysapp, SYSLaunchMenu) {
    LOG("SYSLaunchMenu: game asked to return to the Wii U menu");
    os::request_exit(0);
}
HLE_EXPORT(sysapp, SYSLaunchSettings) { LOG("SYSLaunchSettings (ignored)"); }

// ---- nn::act (accounts) -------------------------------------------------------------------------------
HLE_EXPORT(nn_act, Initialize__Q2_2nn3actFv) { RET(0); }
HLE_EXPORT(nn_act, Finalize__Q2_2nn3actFv) { RET(0); }
HLE_EXPORT(nn_act, GetSlotNo__Q2_2nn3actFv) { RET(1); }

// ---- nn::boss (SpotPass / play reports): accept and do nothing -------------------------------------
namespace {
constexpr uint32_t kResultOk = 0;
constexpr uint32_t kBossNotRegistered = 0xA0220D00;  // error result for "task not registered"
}  // namespace
HLE_EXPORT(nn_boss, Initialize__Q2_2nn4bossFv) { RET(kResultOk); }
HLE_EXPORT(nn_boss, Finalize__Q2_2nn4bossFv) {}
// Task::Task(): zero the object, return this
HLE_EXPORT(nn_boss, __ct__Q3_2nn4boss4TaskFv) {
    mem::commit(ARG(0), 0x20, true);
    memset(mem::host(ARG(0)), 0, 0x20);
    RET(ARG(0));
}
HLE_EXPORT(nn_boss, __dt__Q3_2nn4boss4TaskFv) { RET(ARG(0)); }
HLE_EXPORT(nn_boss, Initialize__Q3_2nn4boss4TaskFPCc) { RET(kResultOk); }
HLE_EXPORT(nn_boss, IsRegistered__Q3_2nn4boss4TaskCFv) { RET(0); }
HLE_EXPORT(nn_boss, Register__Q3_2nn4boss4TaskFRQ3_2nn4boss11TaskSetting) { RET(kResultOk); }
HLE_EXPORT(nn_boss, Unregister__Q3_2nn4boss4TaskFv) { RET(kResultOk); }
HLE_EXPORT(nn_boss, Run__Q3_2nn4boss4TaskFb) { RET(kResultOk); }
// uint32 Task::GetTurnState(uint32* executionCount): 0 = TASK_STOPPED
HLE_EXPORT(nn_boss, GetTurnState__Q3_2nn4boss4TaskCFPUi) {
    if (ARG(1)) mem::w32(ARG(1), 0);
    RET(0);
}
HLE_EXPORT(nn_boss, GetResult__Q3_2nn4boss4TaskCFPUi) {
    if (ARG(1)) mem::w32(ARG(1), 0);
    RET(0);
}
// TaskResult::TaskResult(nn::Result): store the result
HLE_EXPORT(nn_boss, __ct__Q3_2nn4boss10TaskResultFQ2_2nn6Result) {
    mem::w32(ARG(0), ARG(1));
    RET(ARG(0));
}
HLE_EXPORT(nn_boss, ErrorCode__Q3_2nn4boss10TaskResultCFv) { RET(0); }
HLE_EXPORT(nn_boss, __ct__Q3_2nn4boss17PlayReportSettingFv) {
    mem::commit(ARG(0), 0x20, true);
    memset(mem::host(ARG(0)), 0, 0x20);
    RET(ARG(0));
}
HLE_EXPORT(nn_boss, __dt__Q3_2nn4boss17PlayReportSettingFv) { RET(ARG(0)); }
HLE_EXPORT(nn_boss, Initialize__Q3_2nn4boss17PlayReportSettingFPvUi) {}
HLE_EXPORT(nn_boss, Set__Q3_2nn4boss17PlayReportSettingFPCcUi) { RET(1); }

// ---- nn::nfp (amiibo): initialized, nothing ever detected ---------------------------------------
HLE_EXPORT(nn_nfp, Initialize__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, Finalize__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, GetNfpState__Q2_2nn3nfpFv) { RET(1); }  // NFP_STATE_INIT
HLE_EXPORT(nn_nfp, StartDetection__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, StopDetection__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, SetActivateEvent__Q2_2nn3nfpFP7OSEvent) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, SetDeactivateEvent__Q2_2nn3nfpFP7OSEvent) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, Mount__Q2_2nn3nfpFv) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, MountRom__Q2_2nn3nfpFv) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, Unmount__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetNfpRomInfo__Q2_2nn3nfpFPQ3_2nn3nfp7RomInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, OpenApplicationArea__Q2_2nn3nfpFUi) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, ReadApplicationArea__Q2_2nn3nfpFPvUi) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetErrorCode__Q2_2nn3nfpFRCQ2_2nn6Result) { RET(0); }
HLE_EXPORT(nn_nfp, InitializeAmiiboSettingsArgsIn__Q2_2nn3nfpFPQ3_2nn3nfp20AmiiboSettingsArgsIn) {
    mem::commit(ARG(0), 0x60, true);
    memset(mem::host(ARG(0)), 0, 0x60);
    RET(kResultOk);
}
HLE_EXPORT(nn_nfp, SwitchToAmiiboSettings__Q2_2nn3nfpFRCQ3_2nn3nfp20AmiiboSettingsArgsInPCcUi) { RET(kResultOk); }

// ---- nn::aoc (DLC) -------------------------------------------------------------------------------
// AOCTitle matches Cemu: titleId u64 at 0, groupId u32 at 8, titleVersion u16 at 12, path[88] at
// 0x0E, then 2 bytes of padding. sizeof is 0x68. The mount path is /vol/aoc<16 lowercase hex>,
// and the content directory (0010/...) is mounted there, the same way Cemu mounts AOC "content".
HLE_EXPORT(nn_aoc, AOC_Initialize) { RET(0); }
HLE_EXPORT(nn_aoc, AOC_DebugRealDeviceAccess) { RET(0); }
HLE_EXPORT(nn_aoc, AOC_CalculateWorkBufferSize) { RET(0x80 + std::min(ARG(0), 256u) * 0x61); }
// AOC_ListTitle(u32* countOut, AOCTitle* list, maxCount, work, workSize)
HLE_EXPORT(nn_aoc, AOC_ListTitle) {
    uint32_t count_out = ARG(0);
    uint32_t list = ARG(1);
    uint32_t max_count = ARG(2);
    if (count_out && mem::commit(count_out, 4, true)) mem::w32(count_out, 0);
    static int logged = 0;
    bool ready = mods::dlc_ready();
    if (logged++ == 0) {
        if (ready) LOG("AOC_ListTitle: DLC %s", mods::aoc_mount_path().c_str());
        else LOG("AOC_ListTitle: DLC not installed");
    }
    if (!ready || !max_count || !list || !mem::commit(list, 0x68, true)) {
        RET(0);
        return;
    }
    memset(mem::host(list), 0, 0x68);
    mem::w64(list, mods::aoc_title_id());
    mem::w32(list + 8, 0);
    mem::w16(list + 12, 0);
    std::string path = mods::aoc_mount_path();
    strncpy((char*)mem::host(list + 0x0E), path.c_str(), 87);
    if (count_out) mem::w32(count_out, 1);
    RET(0);
}
// AOC_OpenTitle(char* pathOut, AOCTitle* title, work, workSize): path is at AOCTitle+0x0E
HLE_EXPORT(nn_aoc, AOC_OpenTitle) {
    if (!ARG(0) || !mem::commit(ARG(0), 0x80, true)) {
        RET(0);
        return;
    }
    memset(mem::host(ARG(0)), 0, 0x80);
    const char* src = "";
    if (ARG(1) && mem::commit(ARG(1) + 0x0E, 88, false)) src = mem::str(ARG(1) + 0x0E);
    if (!src) src = "";
    strncpy((char*)mem::host(ARG(0)), src, 0x7F);
    RET(0);
}
HLE_EXPORT(nn_aoc, AOC_CloseTitle) { RET(0); }
// AOC_GetPurchaseInfo(u32* out, u64 titleId (r5:r6), u16* ids, count, work, workSize)
HLE_EXPORT(nn_aoc, AOC_GetPurchaseInfo) {
    uint32_t out = ARG(0);
    uint32_t ids = ARG(4);
    uint32_t count = ARG(5);
    if (count > 256) count = 256;
    static int logged = 0;
    if (logged++ == 0) LOG("AOC_GetPurchaseInfo: %u entries, DLC %s", count, mods::dlc_ready() ? "on" : "off");
    if (!out || !count || !mem::commit(out, count * 4, true)) {
        RET(0);
        return;
    }
    bool ids_ok = ids && mem::commit(ids, count * 2, false);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t id = ids_ok ? mem::r16(ids + i * 2) : 0;
        mem::w32(out + i * 4, mods::owns_content(id) ? 1u : 0u);
    }
    RET(0);
}

HLE_EXPORT(sysapp, SYSSwitchToEShop) { LOG("SYSSwitchToEShop (ignored)"); }
