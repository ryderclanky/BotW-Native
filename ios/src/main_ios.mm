// iOS entry point. SDL3 owns main(): SDL_RunApp -> UIApplicationMain with SDL's scene delegate,
// which calls ios_main on the main thread once the scene is up. ios_main finds the game folder in
// the app's Documents (copied there with Finder file sharing), prepares the sandbox paths and hands
// over to the shared runtime entry (runtime/src/main.cpp, compiled with -Dmain=botw_main).
#import <UIKit/UIKit.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/sysctl.h>

#include <string>
#include <thread>
#include <vector>

#include "log.h"

int botw_main(int argc, char** argv);

namespace {
NSString* const kReadme =
    @"BotW-Native needs the Breath of the Wild (Wii U, v1.5.0) game files.\n\n"
     "Copy the folder that contains code/, content/ and meta/ into this app's Documents\n"
     "(Finder > iPhone > Files > BotW, drag the folder in) and name it \"game\":\n\n"
     "    Documents/game/code/U-King.rpx\n"
     "    Documents/game/content/...\n"
     "    Documents/game/meta/...\n\n"
     "Saves, shader caches and runtime.log live in the app's Library/Application Support/BotWRecomp.\n";

bool has_rpx(NSString* dir) {
    return [NSFileManager.defaultManager fileExistsAtPath:[dir stringByAppendingPathComponent:@"code/U-King.rpx"]];
}

NSString* find_game(NSString* docs) {
    NSString* g = [docs stringByAppendingPathComponent:@"game"];
    if (has_rpx(g)) return g;
    if (has_rpx(docs)) return docs;  // code/ content/ meta/ copied straight into Documents
    for (NSString* name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:docs error:nil]) {
        NSString* d = [docs stringByAppendingPathComponent:name];
        if (has_rpx(d)) return d;
    }
    return nil;
}

uint64_t footprint_bytes() {
    task_vm_info_data_t vm;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &n) != KERN_SUCCESS) return 0;
    return vm.phys_footprint;
}

// Memory/jetsam headroom in runtime.log every 10 s (the numbers the port has to live within).
void start_stats_thread(std::string device) {
    std::thread([device] {
        bool first = true;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(first ? 2 : 10));
            if (first) LOG("%s", device.c_str());  // the log file is open by now
            first = false;
            LOG("ios: footprint %llu MB, available before jetsam %llu MB",
                (unsigned long long)(footprint_bytes() >> 20), (unsigned long long)(os_proc_available_memory() >> 20));
        }
    }).detach();
}

int ios_main(int argc, char** argv) {
    // landscape only; system edge gestures need a second swipe while playing
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");

    NSFileManager* fm = NSFileManager.defaultManager;
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString* support =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    // runtime/src/main.cpp creates $HOME/Library/Application Support/BotWRecomp with a plain mkdir
    [fm createDirectoryAtPath:[support stringByAppendingPathComponent:@"BotWRecomp"]
        withIntermediateDirectories:YES attributes:nil error:nil];
    [kReadme writeToFile:[docs stringByAppendingPathComponent:@"README.txt"] atomically:YES
                encoding:NSUTF8StringEncoding error:nil];

    std::vector<std::string> args(argv, argv + argc);
    bool has_game_arg = false;
    for (auto& a : args) has_game_arg |= a == "--game";
    if (!has_game_arg && !getenv("BOTW_GAME")) {
        NSString* game = find_game(docs);
        while (!game) {
            const SDL_MessageBoxButtonData buttons[] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Retry"},
                {0, 0, "Quit"},
            };
            const SDL_MessageBoxData box = {
                SDL_MESSAGEBOX_INFORMATION, nullptr, "Game files not found",
                "Copy the Wii U game folder (code/ content/ meta/) into this app's Documents as \"game\" "
                "using Finder file sharing, then tap Retry. See README.txt in the app's files.",
                2, buttons, nullptr};
            int pressed = 0;
            if (!SDL_ShowMessageBox(&box, &pressed) || pressed == 0) return 1;
            game = find_game(docs);
        }
        // 12 GB of game data must not go into iCloud / computer backups
        NSURL* url = [NSURL fileURLWithPath:game];
        if (![game isEqualToString:docs]) [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        args.push_back("--game");
        args.push_back(game.UTF8String);
    }

    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t memsize = 0;
    size_t len = sizeof memsize;
    sysctl(mib, 2, &memsize, &len, nullptr, 0);
    char model[64] = {};
    len = sizeof model;
    sysctlbyname("hw.machine", model, &len, nullptr, 0);
    char device[256];
    snprintf(device, sizeof device, "ios: %s, iOS %s, RAM %llu MB, available before jetsam at launch %llu MB", model,
             UIDevice.currentDevice.systemVersion.UTF8String, (unsigned long long)(memsize >> 20),
             (unsigned long long)(os_proc_available_memory() >> 20));
    start_stats_thread(device);

    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    return botw_main((int)args.size(), cargv.data());
}
}  // namespace

int main(int argc, char** argv) { return SDL_RunApp(argc, argv, ios_main, nullptr); }
