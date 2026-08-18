#pragma once

#include <Windows.h>

#include <cstdint>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::retail_log {

extern SRWLOCK g_lock;
extern hooking::detour::Handle g_handle;

/** Last outer native-log call to enter and return through the observer. */
struct ProgressSnapshot final {
    std::uint64_t enteredSerial{};
    std::uint64_t returnedSerial{};
    std::uint64_t enteredCallerRva{};
    std::uint64_t returnedCallerRva{};
    std::int32_t enteredSite{-1};
    std::int32_t returnedSite{-1};
    std::uint32_t enteredThread{};
    std::uint32_t returnedThread{};
    std::uint32_t activeObserverCalls{};
    std::uint32_t activeNativeCalls{};
};

/** @return The enqueue observer body itself, with internal linkage. */
[[nodiscard]] void* enqueue_entry_point() noexcept;

/** @return Lock-free progress used by the watchdog assert to locate a native-log stall. */
[[nodiscard]] ProgressSnapshot progress_snapshot() noexcept;

} // namespace sunrise::client::hooks::retail_log
