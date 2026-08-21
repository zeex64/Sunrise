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
#include "entity_slot_probe.h"
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
constexpr std::size_t kGateReportCapacity = 16;

using CollectorGate = bool(__fastcall*)(void*);
using Collector = int(__fastcall*)(void*, std::uint32_t, std::uint32_t, void*, int, std::uint32_t*);

struct GateSnapshot {
    const void* self{};
    std::uint32_t value28{};
    std::uint32_t value2C{};
    std::uint32_t value30{};
    std::uint32_t value34{};
    std::uint8_t enabledA{};
    std::uint8_t enabledB{};
    bool readable{};
};

struct Snapshot {
    std::array<std::uint32_t, kCandidateSampleCount> candidates{};
    const void* manager{};
    std::uint64_t planToken{};
    std::uint64_t mappedToken{};
    std::uint32_t watchedEntity{};
    std::uint32_t supportMask{};
    std::uint32_t namespacePriority{};
    std::uint16_t objectFlags{};
    std::uint16_t namespaceFlags{};
    std::int32_t namespaceId{-1};
    std::int32_t handlerNamespaceId{-1};
    std::int32_t mappedNamespaceId{-1};
    std::int32_t globalSlot{-1};
    std::int16_t internalIndex{-1};
    std::uint16_t activeCount{};
    std::uint16_t planSlot{};
    std::uint16_t mappedSlot{};
    std::uint8_t enabledA{};
    std::uint8_t enabledB{};
    std::uint8_t mappedHandleGeneration{};
    std::uint8_t mappedReservedGeneration{};
    std::uint8_t mappedObjectGeneration{};
    std::int8_t ownerSelector{-1};
    std::int8_t metadataOwner{-1};
    std::uint8_t candidateCount{};
    bool managerNamespaceReadable{};
    bool managerMapped{};
    bool mappedCandidate{};
    bool watched{};
    bool occupied{};
    bool targetActive{};
    bool dirty{};
    bool targetSupported{};
    bool ownerMatches{};
    bool targetEligible{};
    bool targetCandidate{};
    bool passiveTarget{};
    bool readable{};
};

SRWLOCK g_seenLock{SRWLOCK_INIT};
std::array<std::uint64_t, kAmbientReportCapacity> g_ambientSeen{};
std::array<std::uint64_t, kWatchedReportCapacity> g_watchedSeen{};
std::array<std::uint64_t, kGateReportCapacity> g_gateSeen{};
std::size_t g_ambientSeenCount{};
std::size_t g_watchedSeenCount{};
std::size_t g_gateSeenCount{};

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

/** Copies the complete leaf-predicate input without retaining game-owned storage. */
[[nodiscard]] bool inspect_gate(const void* self, GateSnapshot& output) noexcept {
    output = {};
    output.self = self;
    if (self == nullptr) {
        return false;
    }

    __try {
        const auto* const bytes = static_cast<const std::byte*>(self);
        output.enabledA = std::to_integer<std::uint8_t>(bytes[9]);
        output.enabledB = std::to_integer<std::uint8_t>(bytes[10]);
        std::memcpy(&output.value28, bytes + 0x28, sizeof output.value28);
        std::memcpy(&output.value2C, bytes + 0x2C, sizeof output.value2C);
        std::memcpy(&output.value30, bytes + 0x30, sizeof output.value30);
        std::memcpy(&output.value34, bytes + 0x34, sizeof output.value34);
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** @return Stable identity for one exact collector-dispatch gate state. */
[[nodiscard]] std::uint64_t gate_snapshot_key(const GateSnapshot& snapshot, bool result) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    mix(hash, snapshot.self);
    mix(hash, snapshot.enabledA);
    mix(hash, snapshot.enabledB);
    mix(hash, snapshot.value28);
    mix(hash, snapshot.value2C);
    mix(hash, snapshot.value30);
    mix(hash, snapshot.value34);
    mix(hash, result);
    return hash == 0 ? 1 : hash;
}

/** @return One-based report number for a new gate state, or zero for duplicate/full. */
[[nodiscard]] std::uint32_t reserve_gate_report(std::uint64_t key) noexcept {
    AcquireSRWLockExclusive(&g_seenLock);
    for (std::size_t index = 0; index < g_gateSeenCount; ++index) {
        if (g_gateSeen[index] == key) {
            ReleaseSRWLockExclusive(&g_seenLock);
            return 0;
        }
    }
    if (g_gateSeenCount >= g_gateSeen.size()) {
        ReleaseSRWLockExclusive(&g_seenLock);
        return 0;
    }
    g_gateSeen[g_gateSeenCount] = key;
    const auto report = static_cast<std::uint32_t>(++g_gateSeenCount);
    ReleaseSRWLockExclusive(&g_seenLock);
    return report;
}

/** Emits one bounded state from the leaf predicate immediately above collector dispatch. */
void report_gate(const GateSnapshot& snapshot, bool result) noexcept {
    if (!snapshot.readable) {
        return;
    }
    const std::uint32_t occurrence = reserve_gate_report(gate_snapshot_key(snapshot, result));
    if (occurrence == 0) {
        return;
    }
    std::array<char, 256> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=entity-collector-gate occurrence=%u self=%p enabled=%u/%u "
                      "v28=0x%08X v2c=0x%08X v30=0x%08X v34=0x%08X result=%u",
                      occurrence,
                      snapshot.self,
                      static_cast<unsigned>(snapshot.enabledA),
                      static_cast<unsigned>(snapshot.enabledB),
                      snapshot.value28,
                      snapshot.value2C,
                      snapshot.value30,
                      snapshot.value34,
                      result ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reads one collector result after the original has updated per-namespace eligibility state. */
[[nodiscard]] bool inspect(void* handler,
                           int emitted,
                           int capacity,
                           const std::uint32_t* candidates,
                           Snapshot& output,
                           const Snapshot* prior) noexcept {
    output = {};
    output.namespaceId = -1;
    output.handlerNamespaceId = -1;
    output.mappedNamespaceId = -1;
    output.globalSlot = -1;
    output.internalIndex = -1;
    if (handler == nullptr) {
        return false;
    }

    __try {
        const auto* const bytes = static_cast<const std::byte*>(handler);
        output.enabledA = std::to_integer<std::uint8_t>(bytes[9]);
        output.enabledB = std::to_integer<std::uint8_t>(bytes[10]);
        std::memcpy(&output.handlerNamespaceId, bytes + 0xC, sizeof output.handlerNamespaceId);
        std::memcpy(&output.manager, bytes + 0x10, sizeof output.manager);

        // Handler +0xC is a collector-local selector and has remained zero for managers whose live
        // providers are namespace 1 or 2. Read the authoritative namespace directly from that
        // provider. A unique exact-manager capture annotates the observation but cannot select the
        // namespace because native manager storage is reused across public-session generations.
        output.managerNamespaceReadable =
            entity_slot_probe::inspect_namespace(output.manager, output.namespaceId);
        if (!output.managerNamespaceReadable) {
            output.namespaceId = output.handlerNamespaceId;
        }
        entity_slot_probe::ViewCapture managerCapture{};
        if (entity_slot_probe::find_by_manager(output.manager, managerCapture)) {
            output.managerMapped = true;
            output.mappedNamespaceId = managerCapture.namespaceId;
            output.mappedToken = managerCapture.token;
            output.mappedCandidate = managerCapture.candidatePresent;
            output.mappedSlot = managerCapture.slot;
            output.mappedHandleGeneration = managerCapture.handleGeneration;
            output.mappedReservedGeneration = managerCapture.reservedGeneration;
            output.mappedObjectGeneration = managerCapture.objectGeneration;
        }
        const bool reusePassive = prior != nullptr && prior->passiveTarget
                                  && prior->manager == output.manager
                                  && prior->namespaceId == output.namespaceId;
        sobject_bind_probe::EntityDebugSnapshot plan{};
        if (reusePassive) {
            output.watched = true;
            output.passiveTarget = true;
            output.planToken = prior->planToken;
            output.planSlot = prior->planSlot;
            output.watchedEntity = prior->watchedEntity;
        } else if (sobject_bind_probe::debug_snapshot(plan) && plan.present
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
        // Before the first server-authored create there is deliberately no bind watch or debug
        // plan. Resolve the passive safe-slot capture by the collector's exact native manager so
        // the enrollment diagnostic does not depend on the create it is meant to unblock.
        if (!output.watched && output.manager != nullptr) {
            std::uint16_t candidateSlot = 0;
            if (entity_slot_probe::inspect_candidate_slot(
                    output.manager, output.namespaceId, candidateSlot)) {
                output.watched = true;
                output.passiveTarget = true;
                if (output.managerMapped && output.mappedNamespaceId == output.namespaceId) {
                    output.planToken = output.mappedToken;
                }
                output.planSlot = candidateSlot;
                output.watchedEntity = candidateSlot;
            }
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
        output.handlerNamespaceId = -1;
        output.mappedNamespaceId = -1;
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
    mix(hash, after.handlerNamespaceId);
    mix(hash, after.mappedNamespaceId);
    mix(hash, emitted);
    mix(hash, view);
    mix(hash, lane);
    mix(hash, after.planToken);
    mix(hash, after.mappedToken);
    mix(hash, after.planSlot);
    mix(hash, after.mappedSlot);
    mix(hash, after.mappedHandleGeneration);
    mix(hash, after.mappedReservedGeneration);
    mix(hash, after.mappedObjectGeneration);
    mix(hash, after.managerMapped);
    mix(hash, after.managerNamespaceReadable);
    mix(hash, after.mappedCandidate);
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
    mix(hash, after.passiveTarget);
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

/** Emits one deduplicated collector state, including bounded empty ambient call proof. */
void report(const Snapshot& before,
            const Snapshot& after,
            int emitted,
            int capacity,
            std::uint32_t view,
            std::uint32_t lane,
            const void* context) noexcept {
    if (!after.readable) {
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
    const bool watchedBudget = emitted > 0 || (after.watched && !after.passiveTarget);
    const std::uint32_t occurrence =
        reserve_report(snapshot_key(before, after, emitted, view, lane), watchedBudget);
    if (occurrence == 0) {
        return;
    }
    std::array<char, 896> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=scheduler-entity-collector occurrence=%u ns=%d raw_ns=%d "
                      "live_ns=%u manager=%p mapped=%u map_ns=%d map_token=0x%016llX "
                      "map_candidate=%u map_slot=%u map_gen=%u/%u/%u view=%u lane=%u "
                      "context=%p enabled=%u/%u active=%u emitted=%d capacity=%d watched=%u "
                      "plan=0x%016llX/%u entity=0x%08X internal=%d "
                      "occupied=%u collector=%u dirty=%u "
                      "supported=%u owner=%d/%d owner_ok=%u object_flags=0x%04X "
                      "ns_flags=0x%04X->0x%04X priority=0x%08X->0x%08X eligible=%u candidate=%u "
                      "c0=0x%08X c1=0x%08X c2=0x%08X c3=0x%08X",
                      occurrence,
                      after.namespaceId,
                      after.handlerNamespaceId,
                      after.managerNamespaceReadable ? 1U : 0U,
                      after.manager,
                      after.managerMapped ? 1U : 0U,
                      after.mappedNamespaceId,
                      static_cast<unsigned long long>(after.mappedToken),
                      after.mappedCandidate ? 1U : 0U,
                      static_cast<unsigned>(after.mappedSlot),
                      static_cast<unsigned>(after.mappedHandleGeneration),
                      static_cast<unsigned>(after.mappedReservedGeneration),
                      static_cast<unsigned>(after.mappedObjectGeneration),
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

/** Preserves the native leaf predicate and records the state that controls collector dispatch. */
__declspec(noinline) bool __fastcall collector_gate_body(void* self) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::schedulerEntityCollectorGate, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CollectorGate>(lease.original);
    bool result = false;
    __try {
        GateSnapshot snapshot{};
        const bool readable = lease.accepting && inspect_gate(self, snapshot);
        if (call != nullptr) {
            result = call(self);
        }
        if (readable) {
            report_gate(snapshot, result);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
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
            (void)inspect(handler, 0, 0, nullptr, before, nullptr);
        }
        if (call != nullptr) {
            result = call(handler, view, lane, viewContext, capacity, candidates);
        }
        if (lease.accepting) {
            Snapshot snapshot{};
            if (inspect(handler, result, capacity, candidates, snapshot, &before)) {
                const bool comparable = before.watched && snapshot.watched
                                        && before.manager == snapshot.manager
                                        && before.namespaceId == snapshot.namespaceId
                                        && before.planSlot == snapshot.planSlot;
                if (!comparable) {
                    before.namespaceFlags = snapshot.namespaceFlags;
                    before.namespacePriority = snapshot.namespacePriority;
                }
                report(before, snapshot, result, capacity, view, lane, viewContext);
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* gate_entry_point() noexcept {
    return reinterpret_cast<void*>(&collector_gate_body);
}

void* collector_entry_point() noexcept {
    return reinterpret_cast<void*>(&collector_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_seenLock);
    g_ambientSeen.fill(0);
    g_watchedSeen.fill(0);
    g_gateSeen.fill(0);
    g_ambientSeenCount = 0;
    g_watchedSeenCount = 0;
    g_gateSeenCount = 0;
    ReleaseSRWLockExclusive(&g_seenLock);
}

} // namespace sunrise::client::hooks::network::scheduler_entity_collector_probe
