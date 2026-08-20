#include "scheduler_entity_collector_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "../../../core/logging/log.h"
#include "../../targets/game.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"
#include "sobject_bind_probe.h"

namespace sunrise::client::hooks::network::scheduler_entity_collector_probe {
namespace {

constexpr std::uint32_t kEntitySlotMask = 0x1FFF;
constexpr std::size_t kActiveBitsOffset = 0xC920;
constexpr std::size_t kOccupiedBitsOffset = 0xC520;
constexpr std::size_t kDirtyBitsOffset = 0xCA20;
constexpr std::size_t kActiveWordCount = 32;
constexpr std::size_t kTypeMapOffset = 0x114;
constexpr std::size_t kTypeMapStride = 6;
constexpr std::size_t kObjectStride = 0x70;
constexpr std::size_t kCandidateSampleCount = 4;
constexpr std::size_t kAmbientReportCapacity = 16;
constexpr std::size_t kWatchedReportCapacity = 64;

using Collector = int(__fastcall*)(void*, std::uint32_t, std::uint32_t, void*, int, std::uint32_t*);

struct Snapshot {
    std::array<std::uint32_t, kCandidateSampleCount> candidates{};
    const void* manager{};
    std::uint64_t planToken{};
    std::uint32_t watchedEntity{};
    std::uint32_t supportMask{};
    std::uint32_t namespacePriority{};
    std::uint16_t objectFlags{};
    std::uint16_t namespaceFlags{};
    std::int32_t namespaceId{-1};
    std::int32_t globalSlot{-1};
    std::int16_t internalIndex{-1};
    std::uint16_t activeCount{};
    std::uint16_t planSlot{};
    std::uint8_t enabledA{};
    std::uint8_t enabledB{};
    std::int8_t ownerSelector{-1};
    std::int8_t metadataOwner{-1};
    std::uint8_t candidateCount{};
    bool watched{};
    bool occupied{};
    bool targetActive{};
    bool dirty{};
    bool targetSupported{};
    bool ownerMatches{};
    bool targetEligible{};
    bool targetCandidate{};
    bool readable{};
};

SRWLOCK g_seenLock{SRWLOCK_INIT};
std::array<std::uint64_t, kAmbientReportCapacity> g_ambientSeen{};
std::array<std::uint64_t, kWatchedReportCapacity> g_watchedSeen{};
std::size_t g_ambientSeenCount{};
std::size_t g_watchedSeenCount{};

/** FNV-1a mixes one trivially copied field into a bounded diagnostic fingerprint. */
template <typename Value> void mix(std::uint64_t& hash, const Value& value) noexcept {
    for (const std::byte byte : std::as_bytes(std::span{&value, 1})) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
}

/** @return Population count without depending on compiler-specific bit intrinsics. */
[[nodiscard]] std::uint16_t bit_count(std::uint32_t value) noexcept {
    std::uint16_t count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

/** Reads one collector result after the original has updated per-namespace eligibility state. */
[[nodiscard]] bool inspect(void* handler,
                           int emitted,
                           int capacity,
                           const std::uint32_t* candidates,
                           Snapshot& output) noexcept {
    output = {};
    output.namespaceId = -1;
    output.globalSlot = -1;
    output.internalIndex = -1;
    if (handler == nullptr) {
        return false;
    }

    __try {
        const auto* const bytes = static_cast<const std::byte*>(handler);
        output.enabledA = std::to_integer<std::uint8_t>(bytes[9]);
        output.enabledB = std::to_integer<std::uint8_t>(bytes[10]);
        std::memcpy(&output.namespaceId, bytes + 0xC, sizeof output.namespaceId);
        std::memcpy(&output.manager, bytes + 0x10, sizeof output.manager);
        sobject_bind_probe::EntityDebugSnapshot plan{};
        if (sobject_bind_probe::debug_snapshot(plan) && plan.present
            && plan.namespaceId == output.namespaceId) {
            output.watched = true;
            output.planToken = plan.token;
            output.planSlot = plan.slot;
            output.watchedEntity = plan.entityId != 0 ? plan.entityId : plan.slot;
        } else {
            output.watched =
                sobject_bind_probe::first_watched(output.namespaceId, output.watchedEntity);
            output.planSlot = static_cast<std::uint16_t>(output.watchedEntity & kEntitySlotMask);
        }

        const bool outputReadable = emitted >= 0 && emitted <= 0x400 && capacity >= emitted
                                    && (emitted == 0 || candidates != nullptr);
        const int boundedCount = outputReadable ? emitted : 0;
        output.candidateCount =
            static_cast<std::uint8_t>(boundedCount < static_cast<int>(output.candidates.size())
                                          ? boundedCount
                                          : static_cast<int>(output.candidates.size()));
        for (std::size_t index = 0; index < output.candidateCount; ++index) {
            output.candidates[index] = candidates[index];
        }
        if (output.watched && outputReadable) {
            for (int index = 0; index < boundedCount; ++index) {
                if ((candidates[index] & kEntitySlotMask)
                    == (output.watchedEntity & kEntitySlotMask)) {
                    output.targetCandidate = true;
                    break;
                }
            }
        }

        if (output.manager == nullptr) {
            output.readable = outputReadable;
            return output.readable;
        }
        const auto* const manager = static_cast<const std::byte*>(output.manager);
        const auto* const activeWords =
            reinterpret_cast<const std::uint32_t*>(manager + kActiveBitsOffset);
        for (std::size_t word = 0; word < kActiveWordCount; ++word) {
            output.activeCount =
                static_cast<std::uint16_t>(output.activeCount + bit_count(activeWords[word]));
        }
        if (!output.watched) {
            output.readable = outputReadable;
            return output.readable;
        }

        const std::uint32_t slot = output.watchedEntity & kEntitySlotMask;
        std::memcpy(&output.internalIndex,
                    manager + kTypeMapOffset + static_cast<std::size_t>(slot) * kTypeMapStride,
                    sizeof output.internalIndex);
        const auto& resolved = targets::game::network::get();
        if (output.internalIndex < 0 || output.internalIndex >= 0x400
            || resolved.sobjectObjectTable == nullptr) {
            output.readable = outputReadable;
            return output.readable;
        }

        const auto internal = static_cast<std::uint16_t>(output.internalIndex);
        const std::uint32_t bit = 1U << (internal & 31U);
        const std::size_t wordOffset = (internal >> 5U) * sizeof(std::uint32_t);
        std::uint32_t occupiedWord = 0;
        std::uint32_t activeWord = 0;
        std::uint32_t dirtyWord = 0;
        std::memcpy(&occupiedWord, manager + kOccupiedBitsOffset + wordOffset, sizeof occupiedWord);
        std::memcpy(&activeWord, manager + kActiveBitsOffset + wordOffset, sizeof activeWord);
        std::memcpy(&dirtyWord, manager + kDirtyBitsOffset + wordOffset, sizeof dirtyWord);
        output.occupied = (occupiedWord & bit) != 0;
        output.targetActive = (activeWord & bit) != 0;
        output.dirty = (dirtyWord & bit) != 0;

        const auto* const object =
            resolved.sobjectObjectTable + static_cast<std::size_t>(internal) * kObjectStride;
        output.globalSlot = output.internalIndex;
        std::memcpy(&output.ownerSelector, object + 1, sizeof output.ownerSelector);
        std::memcpy(&output.objectFlags, object + 0x50, sizeof output.objectFlags);
        const std::byte* metadata = nullptr;
        std::memcpy(&metadata, object + 0x60, sizeof metadata);
        if (metadata != nullptr) {
            std::memcpy(&output.metadataOwner, metadata, sizeof output.metadataOwner);
            std::memcpy(&output.supportMask, metadata + 4, sizeof output.supportMask);
        }
        output.targetSupported = output.namespaceId >= 0 && output.namespaceId < 32
                                 && ((output.supportMask >> output.namespaceId) & 1U) != 0;
        output.ownerMatches =
            output.ownerSelector == -1 || output.metadataOwner == output.namespaceId;

        if (metadata != nullptr && output.namespaceId >= 0 && output.namespaceId < 32) {
            const auto* const namespaceState =
                metadata + static_cast<std::size_t>(output.namespaceId) * 0x40;
            std::memcpy(
                &output.namespacePriority, namespaceState + 0x40, sizeof output.namespacePriority);
            std::memcpy(
                &output.namespaceFlags, namespaceState + 0x50, sizeof output.namespaceFlags);
        }
        output.targetEligible = output.enabledA != 0 && output.enabledB != 0 && output.targetActive
                                && output.targetSupported && output.ownerMatches
                                && (output.namespaceFlags & 0x80U) != 0
                                && (output.objectFlags & 0x0200U) == 0;
        output.readable = outputReadable;
        return output.readable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.namespaceId = -1;
        output.globalSlot = -1;
        output.internalIndex = -1;
        return false;
    }
}

/** @return Stable key for one meaningful collector/candidate state. */
[[nodiscard]] std::uint64_t snapshot_key(const Snapshot& before,
                                         const Snapshot& after,
                                         int emitted,
                                         std::uint32_t view,
                                         std::uint32_t lane) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    mix(hash, after.namespaceId);
    mix(hash, emitted);
    mix(hash, view);
    mix(hash, lane);
    mix(hash, after.planToken);
    mix(hash, after.planSlot);
    mix(hash, after.activeCount);
    mix(hash, after.watchedEntity);
    mix(hash, after.globalSlot);
    mix(hash, after.enabledA);
    mix(hash, after.enabledB);
    mix(hash, after.supportMask);
    mix(hash, after.occupied);
    mix(hash, after.targetActive);
    mix(hash, after.dirty);
    mix(hash, after.targetSupported);
    mix(hash, after.ownerSelector);
    mix(hash, after.metadataOwner);
    mix(hash, after.ownerMatches);
    mix(hash, after.targetEligible);
    mix(hash, after.objectFlags);
    mix(hash, before.namespacePriority);
    mix(hash, after.namespacePriority);
    mix(hash, before.namespaceFlags);
    mix(hash, after.namespaceFlags);
    mix(hash, after.targetCandidate);
    for (const std::uint32_t candidate : after.candidates) {
        mix(hash, candidate);
    }
    return hash == 0 ? 1 : hash;
}

/** @return One-based report number for a new state, or zero for duplicate/full. */
[[nodiscard]] std::uint32_t reserve_report(std::uint64_t key, bool watched) noexcept {
    AcquireSRWLockExclusive(&g_seenLock);
    std::span<std::uint64_t> seen =
        watched ? std::span<std::uint64_t>{g_watchedSeen} : std::span<std::uint64_t>{g_ambientSeen};
    std::size_t& seenCount = watched ? g_watchedSeenCount : g_ambientSeenCount;
    for (std::size_t index = 0; index < seenCount; ++index) {
        if (seen[index] == key) {
            ReleaseSRWLockExclusive(&g_seenLock);
            return 0;
        }
    }
    if (seenCount >= seen.size()) {
        ReleaseSRWLockExclusive(&g_seenLock);
        return 0;
    }
    seen[seenCount] = key;
    const auto report = static_cast<std::uint32_t>(++seenCount);
    ReleaseSRWLockExclusive(&g_seenLock);
    return report;
}

/** Emits one deduplicated collector state; ordinary empty/unwatched calls stay silent. */
void report(const Snapshot& before,
            const Snapshot& after,
            int emitted,
            int capacity,
            std::uint32_t view,
            std::uint32_t lane,
            const void* context) noexcept {
    if (!after.readable || (emitted <= 0 && !after.watched)) {
        return;
    }
    if (after.watched) {
        sobject_bind_probe::record_collector(after.namespaceId,
                                             after.watchedEntity,
                                             after.globalSlot,
                                             after.targetActive,
                                             after.targetEligible,
                                             after.targetCandidate,
                                             after.objectFlags,
                                             after.namespaceFlags,
                                             after.supportMask);
    }
    const std::uint32_t occurrence =
        reserve_report(snapshot_key(before, after, emitted, view, lane), after.watched);
    if (occurrence == 0) {
        return;
    }
    std::array<char, 640> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=scheduler-entity-collector occurrence=%u ns=%d view=%u lane=%u "
        "context=%p enabled=%u/%u active=%u emitted=%d capacity=%d watched=%u "
        "plan=0x%016llX/%u entity=0x%08X internal=%d "
        "occupied=%u collector=%u dirty=%u "
        "supported=%u owner=%d/%d owner_ok=%u object_flags=0x%04X "
        "ns_flags=0x%04X->0x%04X priority=0x%08X->0x%08X eligible=%u candidate=%u "
        "c0=0x%08X c1=0x%08X c2=0x%08X c3=0x%08X",
        occurrence,
        after.namespaceId,
        view,
        lane,
        context,
        static_cast<unsigned>(after.enabledA),
        static_cast<unsigned>(after.enabledB),
        static_cast<unsigned>(after.activeCount),
        emitted,
        capacity,
        after.watched ? 1U : 0U,
        static_cast<unsigned long long>(after.planToken),
        static_cast<unsigned>(after.planSlot),
        after.watchedEntity,
        after.internalIndex,
        after.occupied ? 1U : 0U,
        after.targetActive ? 1U : 0U,
        after.dirty ? 1U : 0U,
        after.targetSupported ? 1U : 0U,
        static_cast<int>(after.ownerSelector),
        static_cast<int>(after.metadataOwner),
        after.ownerMatches ? 1U : 0U,
        static_cast<unsigned>(after.objectFlags),
        static_cast<unsigned>(before.namespaceFlags),
        static_cast<unsigned>(after.namespaceFlags),
        before.namespacePriority,
        after.namespacePriority,
        after.targetEligible ? 1U : 0U,
        after.targetCandidate ? 1U : 0U,
        after.candidates[0],
        after.candidates[1],
        after.candidates[2],
        after.candidates[3]);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Preserves the native collector and observes only its completed candidate output. */
__declspec(noinline) int __fastcall collector_body(void* handler,
                                                   std::uint32_t view,
                                                   std::uint32_t lane,
                                                   void* viewContext,
                                                   int capacity,
                                                   std::uint32_t* candidates) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::schedulerEntityCollector, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Collector>(lease.original);
    int result = 0;
    __try {
        Snapshot before{};
        if (lease.accepting) {
            (void)inspect(handler, 0, 0, nullptr, before);
        }
        if (call != nullptr) {
            result = call(handler, view, lane, viewContext, capacity, candidates);
        }
        if (lease.accepting) {
            Snapshot snapshot{};
            if (inspect(handler, result, capacity, candidates, snapshot)) {
                report(before, snapshot, result, capacity, view, lane, viewContext);
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* collector_entry_point() noexcept {
    return reinterpret_cast<void*>(&collector_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_seenLock);
    g_ambientSeen.fill(0);
    g_watchedSeen.fill(0);
    g_ambientSeenCount = 0;
    g_watchedSeenCount = 0;
    ReleaseSRWLockExclusive(&g_seenLock);
}

} // namespace sunrise::client::hooks::network::scheduler_entity_collector_probe
