#include "sobject_native_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../state/gameplay/definition.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::sobject_native_probe {
namespace {

constexpr std::size_t kSeenCapacity = 4096;
/** Package-backed handles encode a 13-bit entry below the package id. */
constexpr std::uint32_t kTagBase = 0x80800000;
constexpr std::uint32_t kTagEntryBits = 13;
constexpr std::uint32_t kTagEntryMask = (1U << kTagEntryBits) - 1U;
/** Enough repeated target registrations to cover the control and atomic-create slots. */
constexpr std::uint32_t kFirstEntityReportLimit = 8;

std::array<std::atomic_uint32_t, kSeenCapacity> g_seen{};
/** Unlike the general catalog, the entity experiment must retain repeat uses of one RSAT. */
std::atomic_uint32_t g_firstEntityRegistrations{};

using Registration = void(__fastcall*)(std::uint32_t, std::uint32_t, std::uint8_t);

/** @return True only for the first observed native construction using this RSAT. */
[[nodiscard]] bool record_once(std::uint32_t rsat) noexcept {
    // Zero is the empty marker and cannot name an installed 0x80800000-based RSAT.
    if (rsat == 0) {
        return false;
    }
    const std::uint32_t key = rsat;
    std::size_t index = (static_cast<std::uint64_t>(key) * 0x9E3779B1ULL) % g_seen.size();
    for (std::size_t probe = 0; probe < g_seen.size(); ++probe) {
        std::atomic_uint32_t& seen = g_seen[index];
        std::uint32_t current = seen.load(std::memory_order_relaxed);
        if (current == key) {
            return false;
        }
        if (current == 0
            && seen.compare_exchange_strong(
                current, key, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
        if (current == key) {
            return false;
        }
        index = (index + 1) % g_seen.size();
    }
    return false;
}

/** Preserves native dependency registration and reports the resolved RSAT once. */
__declspec(noinline) void __fastcall
registration_body(std::uint32_t objectId, std::uint32_t rsat, std::uint8_t unbound) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectNativeRegistration, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Registration>(lease.original);
    __try {
        if (call != nullptr) {
            call(objectId, rsat, unbound);
        }
        const bool firstEntity = rsat == state::gameplay::kFirstEntityRsat;
        std::uint32_t occurrence = 0;
        bool report = false;
        if (lease.accepting) {
            occurrence =
                firstEntity ? g_firstEntityRegistrations.fetch_add(1, std::memory_order_relaxed) + 1
                            : 0;
            report = firstEntity ? occurrence <= kFirstEntityReportLimit : record_once(rsat);
        }
        if (report) {
            const std::uint32_t handle = rsat >= kTagBase ? rsat - kTagBase : 0;
            const auto packageId = static_cast<std::uint16_t>(handle >> kTagEntryBits);
            const auto entryIndex = static_cast<std::uint16_t>(handle & kTagEntryMask);
            std::array<char, 192> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=gameplay stage=sobject-native "
                                              "object=0x%08X rsat=0x%08X package=0x%04X "
                                              "entry=0x%04X unbound=%u target=%u occurrence=%u",
                                              objectId,
                                              rsat,
                                              static_cast<unsigned>(packageId),
                                              static_cast<unsigned>(entryIndex),
                                              static_cast<unsigned>(unbound),
                                              firstEntity ? 1U : 0U,
                                              occurrence);
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

} // namespace

void* registration_entry_point() noexcept {
    return reinterpret_cast<void*>(&registration_body);
}

void reset() noexcept {
    for (std::atomic_uint32_t& seen : g_seen) {
        seen.store(0, std::memory_order_relaxed);
    }
    g_firstEntityRegistrations.store(0, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::network::sobject_native_probe
