#include "retail_log_enqueue_observer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::retail_log {
namespace {

using Enqueue = void(__fastcall*)(std::int32_t, const char*) noexcept;

/** The game copies exactly this many bytes out of the caller's text buffer. */
constexpr std::size_t kNativeTextSize = 320;
/** Site id the game uses for an unregistered line. */
constexpr std::int32_t kUnregisteredSite = -1;
/** Line storage holds the cleaned text plus its fixed key prefix. */
constexpr std::size_t kEventCapacity = kNativeTextSize + 64;
/** Native zone churn can toggle this cosmetic channel name hundreds of times per second. */
constexpr std::string_view kChannelNameChangePrefix =
    "networking:channel: Channel name change from";

thread_local bool g_inObserver{};
/** Outer native-log call currently entered, or the last one when no call is active. */
std::atomic<std::int32_t> g_enteredSite{-1};
/** Last outer native-log call whose original enqueue returned. */
std::atomic<std::int32_t> g_returnedSite{-1};
/** Monotonic sequence assigned to each outer native-log call. */
std::atomic<std::uint64_t> g_nextSerial{0};
std::atomic<std::uint64_t> g_enteredSerial{0};
std::atomic<std::uint64_t> g_returnedSerial{0};
std::atomic<std::uint32_t> g_activeObservers{0};
std::atomic<std::uint32_t> g_activeNative{0};

/**
 * Copies the native text into fixed storage as one printable line.
 * @param text Borrowed native buffer.
 * @param output Receives the cleaned characters.
 * @return Number of characters written.
 */
[[nodiscard]] std::size_t sanitize(const char* text, std::array<char, kNativeTextSize>& output) {
    std::size_t length = 0;
    __try {
        for (; length < kNativeTextSize - 1 && text[length] != '\0'; ++length) {
            const char value = text[length];
            // One line, one event: the native text carries its own line breaks.
            output[length] = value >= ' ' && value != '\x7F' ? value : ' ';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    while (length != 0 && output[length - 1] == ' ') {
        --length;
    }
    return length;
}

/**
 * Writes one captured line.
 * @param siteId Registered site id.
 * @param text Borrowed native buffer.
 */
void capture_line(std::int32_t siteId, const char* text) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::info)) {
        return;
    }
    std::array<char, kNativeTextSize> sanitized{};
    const std::size_t textLength = sanitize(text, sanitized);
    if (std::string_view(sanitized.data(), textLength).starts_with(kChannelNameChangePrefix)) {
        return;
    }
    std::array<char, kEventCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=retail site=%d text=%.*s",
                                      siteId,
                                      static_cast<int>(textLength),
                                      sanitized.data());
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
}

/**
 * Mirrors the single funnel every retail log line passes through.
 * @param siteId Registered site id.
 * @param text Native buffer holding the already-formatted line.
 */
__declspec(noinline) void __fastcall enqueue_body(std::int32_t siteId, const char* text) noexcept {
    // Native enqueue can itself emit a nested line, so only the outer call is captured.
    const bool outer = !g_inObserver;
    g_inObserver = true;
    std::uint64_t serial = 0;
    if (outer) {
        g_activeObservers.fetch_add(1, std::memory_order_acq_rel);
        serial = g_nextSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        g_enteredSite.store(siteId, std::memory_order_relaxed);
        g_enteredSerial.store(serial, std::memory_order_release);
        g_activeNative.fetch_add(1, std::memory_order_acq_rel);
    }
    const auto call = reinterpret_cast<Enqueue>(g_handle.original);
    if (call != nullptr) {
        call(siteId, text);
    }
    if (outer) {
        g_returnedSite.store(siteId, std::memory_order_relaxed);
        g_returnedSerial.store(serial, std::memory_order_release);
        g_activeNative.fetch_sub(1, std::memory_order_acq_rel);
        if (siteId != kUnregisteredSite && text != nullptr) {
            capture_line(siteId, text);
        }
        g_inObserver = false;
        g_activeObservers.fetch_sub(1, std::memory_order_acq_rel);
    }
}

} // namespace

/** @return The enqueue observer body itself, with internal linkage. */
void* enqueue_entry_point() noexcept {
    return reinterpret_cast<void*>(&enqueue_body);
}

/** @return Lock-free progress used by the watchdog assert to locate a native-log stall. */
ProgressSnapshot progress_snapshot() noexcept {
    ProgressSnapshot snapshot{};
    snapshot.enteredSerial = g_enteredSerial.load(std::memory_order_acquire);
    snapshot.enteredSite = g_enteredSite.load(std::memory_order_relaxed);
    snapshot.returnedSerial = g_returnedSerial.load(std::memory_order_acquire);
    snapshot.returnedSite = g_returnedSite.load(std::memory_order_relaxed);
    snapshot.activeObserverCalls = g_activeObservers.load(std::memory_order_acquire);
    snapshot.activeNativeCalls = g_activeNative.load(std::memory_order_acquire);
    return snapshot;
}

} // namespace sunrise::client::hooks::retail_log
