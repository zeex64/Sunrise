#include "activity_route_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::activity_route_probe {
namespace {

/** The native manager stores its orbit/mission identity index at this offset. */
constexpr std::size_t kIdentityOffset = 0x854;
/** This byte in the 0xa8-byte start record selects local zero versus authored nonzero. */
constexpr std::size_t kRecordRouteOffset = 0x12;
/** Unexpected identities share the high observation bit without losing normal slots 0 through 7. */
constexpr std::uint32_t kUnexpectedIdentityBit = 0x80000000U;

enum class Route {
    local,
    authored,
};

using LocalInitializer = std::uint8_t(__fastcall*)(void*,
                                                   std::uint64_t,
                                                   std::int32_t,
                                                   std::int64_t,
                                                   std::int64_t,
                                                   std::int64_t,
                                                   std::int32_t,
                                                   std::uint32_t);
using AuthoredInitializer = std::uint8_t(__fastcall*)(void*,
                                                      std::uint8_t,
                                                      std::uint32_t,
                                                      std::int64_t,
                                                      std::uint8_t,
                                                      std::uint64_t,
                                                      std::int64_t,
                                                      std::uint32_t,
                                                      std::uint32_t);
using RecordLookup = std::int64_t(__fastcall*)(std::int32_t, std::int64_t*);

std::atomic<std::uint32_t> g_recordLocal{};
std::atomic<std::uint32_t> g_recordAuthored{};
std::atomic<std::uint32_t> g_localCalled{};
std::atomic<std::uint32_t> g_localSucceeded{};
std::atomic<std::uint32_t> g_localFailed{};
std::atomic<std::uint32_t> g_authoredCalled{};
std::atomic<std::uint32_t> g_authoredSucceeded{};
std::atomic<std::uint32_t> g_authoredFailed{};

/** @return One stable observation bit for a native manager identity. */
[[nodiscard]] constexpr std::uint32_t identity_bit(std::int32_t identity) noexcept {
    return identity >= 0 && identity < 8 ? 1U << static_cast<std::uint32_t>(identity)
                                         : kUnexpectedIdentityBit;
}

/** @return True once for each route, result stage, and identity. */
[[nodiscard]] bool first(std::atomic<std::uint32_t>& observations, std::int32_t identity) noexcept {
    const std::uint32_t bit = identity_bit(identity);
    return (observations.fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
}

/** Reads the proven route byte without trusting an unavailable or stale native record. */
[[nodiscard]] bool read_record_route(std::int64_t record, std::uint8_t& route) noexcept {
    if (record == 0) {
        return false;
    }
    __try {
        route = *reinterpret_cast<const std::uint8_t*>(record + kRecordRouteOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Reads the identity without turning a diagnostic probe into a new native fault. */
[[nodiscard]] bool read_identity(void* manager, std::int32_t& identity) noexcept {
    if (manager == nullptr) {
        return false;
    }
    __try {
        identity = *reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(manager)
                                                          + kIdentityOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Emits the raw selector that the managed-session pump uses for its constructor branch. */
void report_record(std::int32_t identity, std::uint8_t selector) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=activity-route-record result=ok identity=%d selector=%u route=%s",
        identity,
        static_cast<unsigned>(selector),
        selector == 0 ? "local" : "authored");
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Emits one compact constructor boundary without logging any native payload pointers. */
void report(Route route, const char* result, std::int32_t identity) noexcept {
    std::array<char, 144> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=activity-route result=%s route=%s identity=%d",
                      result,
                      route == Route::authored ? "authored" : "local",
                      identity);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result[0] == 'f' ? core::log::Level::warn : core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Preserves the route-record lookup ABI and reports each identity/branch pair once. */
__declspec(noinline) std::int64_t __fastcall record_body(std::int32_t identity,
                                                         std::int64_t* manager) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityRouteRecord, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<RecordLookup>(lease.original);
    std::int64_t record = 0;
    __try {
        if (call != nullptr) {
            record = call(identity, manager);
        }
        std::uint8_t selector = 0;
        if (lease.accepting && read_record_route(record, selector)) {
            std::atomic<std::uint32_t>& observations =
                selector == 0 ? g_recordLocal : g_recordAuthored;
            if (first(observations, identity)) {
                report_record(identity, selector);
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return record;
}

/** Preserves the complete local-manager ABI and records entry plus native completion. */
__declspec(noinline) std::uint8_t __fastcall local_body(void* manager,
                                                        std::uint64_t flags,
                                                        std::int32_t variant,
                                                        std::int64_t selection,
                                                        std::int64_t session,
                                                        std::int64_t owner,
                                                        std::int32_t count,
                                                        std::uint32_t policy) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityRouteLocal, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<LocalInitializer>(lease.original);
    std::uint8_t result = 0;
    __try {
        std::int32_t identity = -1;
        const bool readable = read_identity(manager, identity);
        if (lease.accepting && readable && first(g_localCalled, identity)) {
            report(Route::local, "called", identity);
        }
        if (call != nullptr) {
            result = call(manager, flags, variant, selection, session, owner, count, policy);
        }
        std::atomic<std::uint32_t>& observations = result != 0 ? g_localSucceeded : g_localFailed;
        if (lease.accepting && readable && first(observations, identity)) {
            report(Route::local, result != 0 ? "ok" : "fail", identity);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the complete authored-manager ABI and records entry plus native completion. */
__declspec(noinline) std::uint8_t __fastcall authored_body(void* manager,
                                                           std::uint8_t flags,
                                                           std::uint32_t variant,
                                                           std::int64_t selection,
                                                           std::uint8_t policy,
                                                           std::uint64_t nonce,
                                                           std::int64_t session,
                                                           std::uint32_t source,
                                                           std::uint32_t destination) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityRouteAuthored, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<AuthoredInitializer>(lease.original);
    std::uint8_t result = 0;
    __try {
        std::int32_t identity = -1;
        const bool readable = read_identity(manager, identity);
        if (lease.accepting && readable && first(g_authoredCalled, identity)) {
            report(Route::authored, "called", identity);
        }
        if (call != nullptr) {
            result = call(
                manager, flags, variant, selection, policy, nonce, session, source, destination);
        }
        std::atomic<std::uint32_t>& observations =
            result != 0 ? g_authoredSucceeded : g_authoredFailed;
        if (lease.accepting && readable && first(observations, identity)) {
            report(Route::authored, result != 0 ? "ok" : "fail", identity);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* record_entry_point() noexcept {
    return reinterpret_cast<void*>(&record_body);
}

void* local_entry_point() noexcept {
    return reinterpret_cast<void*>(&local_body);
}

void* authored_entry_point() noexcept {
    return reinterpret_cast<void*>(&authored_body);
}

void reset() noexcept {
    g_recordLocal.store(0, std::memory_order_release);
    g_recordAuthored.store(0, std::memory_order_release);
    g_localCalled.store(0, std::memory_order_release);
    g_localSucceeded.store(0, std::memory_order_release);
    g_localFailed.store(0, std::memory_order_release);
    g_authoredCalled.store(0, std::memory_order_release);
    g_authoredSucceeded.store(0, std::memory_order_release);
    g_authoredFailed.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::network::activity_route_probe
