#include "entity_slot_probe.h"

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

namespace sunrise::client::hooks::network::entity_slot_probe {
namespace {

constexpr std::size_t kEntityCapacity = 0x2000;
constexpr std::size_t kBitsetWordCount = kEntityCapacity / 32;
constexpr std::size_t kCandidateCapacity = 8;
constexpr std::size_t kSeenSnapshotCapacity = 32;
constexpr std::size_t kEntryBaseOffset = 0x114;
constexpr std::size_t kEntryStride = 6;
constexpr std::size_t kFreeBitsetOffset = 0xC118;
constexpr std::size_t kOccupiedBitsetOffset = 0xC520;

std::array<std::atomic_uint64_t, kSeenSnapshotCapacity> g_seenSnapshots{};

using Decoder = int(__fastcall*)(void*, void*, void*, void*, int, void*, int*);

struct Candidate {
    std::uint16_t slot{};
    std::uint8_t handleGeneration{};
    std::uint8_t reservedGeneration{};
    std::uint8_t objectGeneration{};
};

struct Snapshot {
    const std::byte* manager{};
    std::int32_t namespaceId{-1};
    std::uint32_t freeCount{};
    std::uint32_t occupiedCount{};
    std::uint32_t availableCount{};
    std::array<Candidate, kCandidateCapacity> candidates{};
    std::size_t candidateCount{};
};

/** @return Population count without depending on compiler-specific intrinsics. */
[[nodiscard]] unsigned bit_count(std::uint32_t value) noexcept {
    value -= (value >> 1U) & 0x55555555U;
    value = (value & 0x33333333U) + ((value >> 2U) & 0x33333333U);
    value = (value + (value >> 4U)) & 0x0F0F0F0FU;
    return (value * 0x01010101U) >> 24U;
}

/** Reads native free/occupied maps and pristine candidate generations without modifying them. */
[[nodiscard]] bool inspect_manager(const void* managerAddress, Snapshot& output) noexcept {
    output = {};
    output.namespaceId = -1;
    if (managerAddress == nullptr) {
        return false;
    }
    __try {
        output.manager = static_cast<const std::byte*>(managerAddress);
        const auto* const provider = *reinterpret_cast<const std::byte* const*>(output.manager + 8);
        if (provider != nullptr) {
            std::memcpy(&output.namespaceId, provider + 8, sizeof output.namespaceId);
        }

        const auto* const freeWords =
            reinterpret_cast<const std::uint32_t*>(output.manager + kFreeBitsetOffset);
        const auto* const occupiedWords =
            reinterpret_cast<const std::uint32_t*>(output.manager + kOccupiedBitsetOffset);
        for (std::size_t word = 0; word < kBitsetWordCount; ++word) {
            output.freeCount += bit_count(freeWords[word]);
            output.occupiedCount += bit_count(occupiedWords[word]);
        }

        for (std::size_t slot = 0; slot < kEntityCapacity; ++slot) {
            const std::uint32_t mask = 1U << (slot & 31U);
            const bool free = (freeWords[slot >> 5U] & mask) != 0;
            const bool occupied = (occupiedWords[slot >> 5U] & mask) != 0;
            const auto* const entry = output.manager + kEntryBaseOffset + slot * kEntryStride;
            std::int16_t descriptor = 0;
            std::memcpy(&descriptor, entry, sizeof descriptor);
            if (!free || occupied || descriptor != -1) {
                continue;
            }
            ++output.availableCount;
            if (output.candidateCount < output.candidates.size()) {
                Candidate& candidate = output.candidates[output.candidateCount++];
                candidate.slot = static_cast<std::uint16_t>(slot);
                candidate.handleGeneration = std::to_integer<std::uint8_t>(entry[2]) & 0x0FU;
                candidate.reservedGeneration = std::to_integer<std::uint8_t>(entry[3]) & 0x0FU;
                candidate.objectGeneration = std::to_integer<std::uint8_t>(entry[4]);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.namespaceId = -1;
        return false;
    }
}

/** FNV-1a keeps manager state snapshots bounded without retaining native pointers to compare. */
void mix(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof value; ++index) {
        hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
        hash *= 1099511628211ULL;
    }
}

/** @return True only for the first observation of this exact slot-map snapshot. */
[[nodiscard]] bool record_once(const Snapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    mix(hash, reinterpret_cast<std::uintptr_t>(snapshot.manager));
    mix(hash, static_cast<std::uint32_t>(snapshot.namespaceId));
    mix(hash, snapshot.freeCount);
    mix(hash, snapshot.occupiedCount);
    mix(hash, snapshot.availableCount);
    for (const Candidate& candidate : snapshot.candidates) {
        mix(hash, candidate.slot);
        mix(hash, candidate.handleGeneration);
        mix(hash, candidate.reservedGeneration);
        mix(hash, candidate.objectGeneration);
    }
    if (hash == 0) {
        hash = 1;
    }
    for (std::atomic_uint64_t& seen : g_seenSnapshots) {
        std::uint64_t current = seen.load(std::memory_order_relaxed);
        if (current == hash) {
            return false;
        }
        if (current == 0
            && seen.compare_exchange_strong(
                current, hash, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
        if (current == hash) {
            return false;
        }
    }
    return false;
}

/** Emits one bounded slot-map snapshot for a newly observed native entity manager. */
void report(const Snapshot& snapshot, int result) noexcept {
    std::array<char, 1024> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=gameplay stage=entity-slots manager=%p namespace=%d "
                                "free=%u occupied=%u available=%u result=%d",
                                static_cast<const void*>(snapshot.manager),
                                snapshot.namespaceId,
                                snapshot.freeCount,
                                snapshot.occupiedCount,
                                snapshot.availableCount,
                                result);
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    std::size_t used = static_cast<std::size_t>(written);
    for (std::size_t index = 0; index < snapshot.candidateCount; ++index) {
        const Candidate& candidate = snapshot.candidates[index];
        written = std::snprintf(line.data() + used,
                                line.size() - used,
                                " c%zu[slot=%u hgen=%u rgen=%u ogen=%u]",
                                index,
                                static_cast<unsigned>(candidate.slot),
                                static_cast<unsigned>(candidate.handleGeneration),
                                static_cast<unsigned>(candidate.reservedGeneration),
                                static_cast<unsigned>(candidate.objectGeneration));
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size() - used) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
}

/** Preserves inbound entity-list decoding and reports the authoritative slot maps once. */
__declspec(noinline) int __fastcall decode_list(void* context,
                                                void* view,
                                                void* control,
                                                void* reader,
                                                int capacity,
                                                void* records,
                                                int* count) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::entitySlotDecoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Decoder>(lease.original);
    int result = 0;
    __try {
        if (call != nullptr) {
            result = call(context, view, control, reader, capacity, records, count);
        }
        if (lease.accepting && context != nullptr) {
            const void* manager = nullptr;
            __try {
                const auto* const bytes = static_cast<const std::byte*>(context);
                std::memcpy(&manager, bytes + 0x10, sizeof manager);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                manager = nullptr;
            }
            observe_manager(manager, result);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* decoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&decode_list);
}

void observe_manager(const void* manager, int result) noexcept {
    Snapshot snapshot{};
    if (inspect_manager(manager, snapshot) && record_once(snapshot)) {
        report(snapshot, result);
    }
}

void reset() noexcept {
    for (std::atomic_uint64_t& seen : g_seenSnapshots) {
        seen.store(0, std::memory_order_relaxed);
    }
}

} // namespace sunrise::client::hooks::network::entity_slot_probe
