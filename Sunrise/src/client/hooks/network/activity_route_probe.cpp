#include "activity_route_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::activity_route_probe {
namespace {

/** The native manager stores its orbit/mission identity index at this offset. */
constexpr std::size_t kIdentityOffset = 0x854;
/** This byte in the 0xa8-byte start record selects local zero versus authored nonzero. */
constexpr std::size_t kRecordRouteOffset = 0x12;
/** Exact size registered for requested-remote-join-data and remote-join-data. */
constexpr std::size_t kRecordSize = 0xA8;
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

/** Copies one native route record without trusting an unavailable or stale pointer. */
[[nodiscard]] bool read_record(std::int64_t record,
                               std::array<std::byte, kRecordSize>& output) noexcept {
    if (record == 0) {
        return false;
    }
    __try {
        std::memcpy(output.data(), reinterpret_cast<const void*>(record), output.size());
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

/** Reads an unaligned little-endian native value from the already-safe record copy. */
template <typename Value>
[[nodiscard]] Value record_value(const std::array<std::byte, kRecordSize>& record,
                                 std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, record.data() + offset, sizeof value);
    return value;
}

/** Emits the decoded bounds plus a one-shot raw record for field mapping. */
void report_record(std::int32_t identity,
                   const std::array<std::byte, kRecordSize>& record) noexcept {
    const std::uint8_t selector = std::to_integer<std::uint8_t>(record[kRecordRouteOffset]);
    std::array<char, 640> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=activity-route-record result=ok identity=%d selector=%u route=%s "
        "f00=%u f04=%u f08=%u f0c=0x%08X f10=%u f11=%u f13=%u id=0x%016llX "
        "a0=%u a1=%u a2=%u a3=%u bytes=",
        identity,
        static_cast<unsigned>(selector),
        selector == 0 ? "local" : "authored",
        record_value<std::uint32_t>(record, 0x00),
        record_value<std::uint32_t>(record, 0x04),
        record_value<std::uint32_t>(record, 0x08),
        record_value<std::uint32_t>(record, 0x0C),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(record[0x10])),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(record[0x11])),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(record[0x13])),
        static_cast<unsigned long long>(record_value<std::uint64_t>(record, 0x18)),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(record[0xA0])),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(record[0xA1])),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(record[0xA2])),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(record[0xA3])));
    if (written <= 0) {
        return;
    }
    std::size_t used = static_cast<std::size_t>(written);
    for (const std::byte value : record) {
        if (used + 2 >= line.size()) {
            break;
        }
        written = std::snprintf(
            line.data() + used, line.size() - used, "%02X", std::to_integer<unsigned>(value));
        if (written != 2) {
            break;
        }
        used += 2;
    }
    if (used > 0) {
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
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
        std::array<std::byte, kRecordSize> value{};
        if (lease.accepting && read_record(record, value)) {
            const std::uint8_t selector = std::to_integer<std::uint8_t>(value[kRecordRouteOffset]);
            std::atomic<std::uint32_t>& observations =
                selector == 0 ? g_recordLocal : g_recordAuthored;
            if (first(observations, identity)) {
                report_record(identity, value);
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
