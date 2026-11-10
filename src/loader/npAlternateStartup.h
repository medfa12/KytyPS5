#pragma once

#include <array>
#include <cstdlib>
#include <cstdint>
#include <string_view>

namespace Loader::NpAlternateStartup {
inline constexpr uint64_t ImageSize = 411092;
inline constexpr uint8_t ImageHash[32] = {
    0x81,0xde,0x80,0x9c,0x26,0xe4,0xcd,0x1a,0x73,0xd6,0x26,0x8c,0x7e,0x89,0xd1,0xe4,
    0x1c,0xf8,0x11,0x39,0x7f,0x1a,0xc7,0xbe,0x17,0xf7,0x0e,0x8e,0x14,0xa2,0x6a,0x35};
inline constexpr uint8_t OriginalHash[32] = {
    0xa0,0xdf,0x8b,0x8c,0x6b,0x7e,0x7d,0xaf,0xce,0x5e,0x1b,0xfd,0x6f,0x31,0x12,0x6b,
    0x61,0xf5,0x13,0x46,0x2c,0xcb,0xbb,0x59,0x19,0x86,0x7f,0x1b,0x18,0x79,0x37,0xc3};
enum class Selection { Legacy, Alternate54, Invalid };

// Empty/0 are disabled. Alternate alone or the runner's complete legacy54
// implication set is accepted; partial implications and dispatcher56 are not.
inline Selection Select(std::string_view alternate, const std::array<std::string_view, 4>& family,
                        std::string_view dispatcher, std::string_view event_queue = {}) {
    // Regular startup72 requires its entire allocator/worker closure. Never
    // widen alternate54 or select native ownership from a partial flag set.
    if (!event_queue.empty() && event_queue != "0") {
        if (event_queue != "1" || (!alternate.empty() && alternate != "0") || dispatcher != "1") { return Selection::Invalid; }
        for (auto flag: family) { if (flag != "1") { return Selection::Invalid; } }
    }
    if (alternate.empty() || alternate == "0") { return Selection::Legacy; }
    if (alternate != "1" || (!dispatcher.empty() && dispatcher != "0")) { return Selection::Invalid; }
    unsigned enabled = 0;
    for (auto flag: family) {
        if (flag == "1") { ++enabled; }
        else if (!flag.empty() && flag != "0") { return Selection::Invalid; }
    }
    return enabled == 0 || enabled == 4 ? Selection::Alternate54 : Selection::Invalid;
}
inline std::string_view Env(const char* key) {
    const char* value = std::getenv(key);
    return value ? value : "";
}
inline Selection Selected() {
    return Select(Env("KYTY_NATIVE_NP_ALTERNATE_STARTUP"),
        {Env("KYTY_NATIVE_NP_OWNERSHIP"), Env("KYTY_NATIVE_NP_WORKQUEUE"),
         Env("KYTY_NATIVE_NP_CALLOUT"), Env("KYTY_NATIVE_NP_HTTP_POOL")}, Env("KYTY_NATIVE_NP_DISPATCHER"), Env("KYTY_NATIVE_NP_EVENT_QUEUE"));
}
inline bool LegacyRequested() {
    for (const char* key: {"KYTY_NATIVE_NP_OWNERSHIP", "KYTY_NATIVE_NP_WORKQUEUE", "KYTY_NATIVE_NP_CALLOUT",
                          "KYTY_NATIVE_NP_HTTP_POOL", "KYTY_NATIVE_NP_DISPATCHER"}) {
        if (Env(key) == "1") { return true; }
    }
    return false;
}
} // namespace Loader::NpAlternateStartup
