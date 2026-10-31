// Choosing which GPU the game renders on. Pure logic with no Vulkan types, shared by the Vulkan
// renderer and the launcher's GPU list, and covered by tests/gpu_pick_test.cpp.
//
// Order: BOTW_GPU (index or part of the name) for one run, then the GPU saved in settings.ini
// (matched by its Vulkan device UUID, then by name), then the best GPU: discrete before
// integrated, then the most device-local memory. A missing or unusable choice falls through.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace gpupick {

// Same values as VkPhysicalDeviceType.
enum Type { kOther = 0, kIntegrated = 1, kDiscrete = 2, kVirtual = 3, kCpu = 4 };

struct Gpu {
    std::string name;
    uint8_t uuid[16]{};
    int type = kOther;
    uint64_t vram = 0;  // bytes of device-local memory
    bool usable = true;
};

struct Choice {
    int index = -1;  // -1: nothing usable
    std::string reason;
};

inline const char* type_name(int t) {
    switch (t) {
    case kDiscrete: return "discrete";
    case kIntegrated: return "integrated";
    case kVirtual: return "virtual";
    case kCpu: return "software";
    default: return "other";
    }
}

inline std::string uuid_hex(const uint8_t* u) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; i++) { s += d[u[i] >> 4]; s += d[u[i] & 15]; }
    return s;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline int type_rank(int t) {
    switch (t) {
    case kDiscrete: return 4;
    case kIntegrated: return 3;
    case kVirtual: return 2;
    case kOther: return 1;
    default: return 0;  // software renderers last
    }
}

// Best usable GPU, or -1.
inline int best(const std::vector<Gpu>& g) {
    int b = -1;
    for (int i = 0; i < (int)g.size(); i++) {
        if (!g[i].usable) continue;
        if (b < 0 || type_rank(g[i].type) > type_rank(g[b].type) ||
            (type_rank(g[i].type) == type_rank(g[b].type) && g[i].vram > g[b].vram))
            b = i;
    }
    return b;
}

inline Choice pick(const std::vector<Gpu>& g, const std::string& pref_uuid, const std::string& pref_name,
                   const std::string& override_) {
    Choice c;
    std::string note;
    if (!override_.empty()) {
        bool digits = true;
        for (char ch : override_) digits = digits && std::isdigit((unsigned char)ch);
        if (digits) {
            int i = std::atoi(override_.c_str());
            if (i >= 0 && i < (int)g.size() && g[i].usable) return {i, "BOTW_GPU=" + override_};
        } else {
            std::string want = lower(override_);
            for (int i = 0; i < (int)g.size(); i++)
                if (g[i].usable && lower(g[i].name).find(want) != std::string::npos) return {i, "BOTW_GPU=" + override_};
        }
        note = "BOTW_GPU=" + override_ + " matched no usable GPU; ";
    }
    if (!pref_uuid.empty() || !pref_name.empty()) {
        std::string want = lower(pref_uuid);
        if (!want.empty())
            for (int i = 0; i < (int)g.size(); i++)
                if (g[i].usable && uuid_hex(g[i].uuid) == want) return {i, note + "saved in settings"};
        if (!pref_name.empty())
            for (int i = 0; i < (int)g.size(); i++)
                if (g[i].usable && g[i].name == pref_name) return {i, note + "saved in settings, matched by name"};
        note += "saved GPU " + (pref_name.empty() ? pref_uuid : pref_name) + " not found; ";
    }
    c.index = best(g);
    if (c.index < 0) return c;
    bool alone = true;
    for (int i = 0; i < (int)g.size(); i++) alone = alone && (i == c.index || !g[i].usable);
    c.reason = note + (alone ? std::string("automatic, only usable GPU")
                             : std::string("automatic, best ") + type_name(g[c.index].type) + " GPU");
    return c;
}

}  // namespace gpupick
