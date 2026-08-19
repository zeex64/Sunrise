#include "view_readiness_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"
#include "view_message_probe.h"

namespace sunrise::client::hooks::network::view_readiness_probe {
namespace {

constexpr std::size_t kObservationCapacity = 8;
constexpr ULONGLONG kPendingReportIntervalMilliseconds = 5000;
constexpr std::size_t kManagerActiveBitsOffset = 0xC920;
constexpr std::size_t kManagerActiveWordCount = 32;

using Scan = std::uint64_t(__fastcall*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t);

struct HandlerSnapshot {
    const void* manager{};
    std::int32_t entityNamespace{-1};
    std::int32_t firstActive{-1};
    std::uint8_t enabled{};
    bool readable{};
};

struct Observation {
    const void* handler{};
    ULONGLONG lastReport{};
    std::uint64_t calls{};
    bool pending{};
    bool occupied{};
};

SRWLOCK g_observationLock{SRWLOCK_INIT};
std::array<Observation, kObservationCapacity> g_observations{};
std::size_t g_replacementCursor{};

/** Reads only the handler and active-set fields used by FUN_141713980. */
[[nodiscard]] bool inspect(const void* handlerAddress, HandlerSnapshot& output) noexcept {
    if (handlerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const handler = static_cast<const std::byte*>(handlerAddress);
        output.enabled = std::to_integer<std::uint8_t>(handler[9]);
        std::memcpy(&output.entityNamespace, handler + 0xC, sizeof output.entityNamespace);
        std::memcpy(&output.manager, handler + 0x10, sizeof output.manager);
        if (output.manager != nullptr) {
            const auto* const words = reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::byte*>(output.manager) + kManagerActiveBitsOffset);
            for (std::size_t word = 0; word < kManagerActiveWordCount; ++word) {
                const std::uint32_t bits = words[word];
                if (bits == 0) {
                    continue;
                }
                std::uint32_t mask = 1;
                for (std::size_t bit = 0; bit < 32; ++bit, mask <<= 1) {
                    if ((bits & mask) != 0) {
                        output.firstActive = static_cast<std::int32_t>(word * 32 + bit);
                        break;
                    }
                }
                break;
            }
        }
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.entityNamespace = -1;
        output.firstActive = -1;
        return false;
    }
}

/** Retains transitions and periodically samples a handler that remains pending. */
[[nodiscard]] bool
record(const void* handler, bool pending, ULONGLONG now, std::uint64_t& calls) noexcept {
    bool report = false;
    AcquireSRWLockExclusive(&g_observationLock);
    Observation* destination = nullptr;
    for (Observation& entry : g_observations) {
        if (entry.occupied && entry.handler == handler) {
            destination = &entry;
            break;
        }
        if (destination == nullptr && !entry.occupied) {
            destination = &entry;
        }
    }
    if (destination == nullptr) {
        destination = &g_observations[g_replacementCursor % g_observations.size()];
        ++g_replacementCursor;
    }
    if (!destination->occupied || destination->handler != handler) {
        *destination = {};
        destination->handler = handler;
        destination->pending = pending;
        destination->occupied = true;
        report = true;
    } else if (destination->pending != pending) {
        destination->pending = pending;
        report = true;
    } else if (pending && now - destination->lastReport >= kPendingReportIntervalMilliseconds) {
        report = true;
    }
    ++destination->calls;
    calls = destination->calls;
    if (report) {
        destination->lastReport = now;
    }
    ReleaseSRWLockExclusive(&g_observationLock);
    return report;
}

/** Preserves the readiness scan and exposes the condition that blocks stage five. */
__declspec(noinline) std::uint64_t __fastcall scan_body(void* handler,
                                                        std::uintptr_t second,
                                                        std::uintptr_t third,
                                                        std::uintptr_t fourth) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::viewReadinessScan, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Scan>(lease.original);
    std::uint64_t result = 0;
    __try {
        if (call != nullptr) {
            result = call(handler, second, third, fourth);
        }
        if (lease.accepting) {
            HandlerSnapshot snapshot{};
            (void)inspect(handler, snapshot);
            const bool pending = (result & 0xFFU) != 0;
            std::uint64_t calls = 0;
            if (record(handler, pending, GetTickCount64(), calls)) {
                std::uint64_t token = 0;
                (void)view_message_probe::token_for_entity_handler(handler, token);
                std::array<char, 320> line{};
                const int written = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=gameplay stage=view-readiness token=0x%llX result=%s handler=%p "
                    "manager=%p namespace=%d enabled=%u first_active=%d readable=%u calls=%llu",
                    static_cast<unsigned long long>(token),
                    pending ? "pending" : "ready",
                    handler,
                    snapshot.manager,
                    snapshot.entityNamespace,
                    static_cast<unsigned>(snapshot.enabled),
                    snapshot.firstActive,
                    snapshot.readable ? 1U : 0U,
                    static_cast<unsigned long long>(calls));
                if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                    core::log::write(core::log::Channel::client,
                                     core::log::Level::info,
                                     {line.data(), static_cast<std::size_t>(written)});
                }
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* scan_entry_point() noexcept {
    return reinterpret_cast<void*>(&scan_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_observationLock);
    g_observations = {};
    g_replacementCursor = 0;
    ReleaseSRWLockExclusive(&g_observationLock);
}

} // namespace sunrise::client::hooks::network::view_readiness_probe
