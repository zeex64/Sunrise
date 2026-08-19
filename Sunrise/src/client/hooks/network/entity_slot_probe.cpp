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
#include "sobject_update_probe.h"

namespace sunrise::client::hooks::network::entity_slot_probe {
namespace {

constexpr std::size_t kEntityCapacity = 0x2000;
constexpr std::size_t kBitsetWordCount = kEntityCapacity / 32;
constexpr std::size_t kCandidateCapacity = 8;
constexpr std::size_t kSeenSnapshotCapacity = 32;
constexpr std::size_t kViewCaptureCapacity = 8;
constexpr std::size_t kEntryBaseOffset = 0x114;
constexpr std::size_t kEntryStride = 6;
constexpr std::size_t kFreeBitsetOffset = 0xC118;
constexpr std::size_t kOccupiedBitsetOffset = 0xC520;
constexpr std::size_t kDecodedRecordEntityOffset = 0x08;
constexpr std::size_t kDecodedRecordCellOffset = 0x02;
constexpr std::size_t kDecodedRecordMaskOffset = 0x18;
constexpr std::size_t kDecodedRecordCreateSizeOffset = 0x2C;
constexpr std::size_t kDecodedRecordUpdateSizeOffset = 0x2E;
constexpr std::size_t kDecodedRecordCreateOffset = 0x30;
constexpr std::size_t kDecodedRecordUpdateOffset = 0x38;
constexpr std::size_t kDecodedRecordFlagsOffset = 0x40;
constexpr std::size_t kDecodedRecordMaskBytes = 16;
constexpr std::size_t kDecodedRecordCreateBytes = 16;
/** Spatial scratch is 0x90 named-component bytes plus the bounded RSAT-defined region. */
constexpr std::size_t kDecodedRecordUpdateBytes = 256;

std::array<std::atomic_uint64_t, kSeenSnapshotCapacity> g_seenSnapshots{};
std::atomic_uint32_t g_decodeTraceBudget{};
SRWLOCK g_captureLock{SRWLOCK_INIT};
std::array<ViewCapture, kViewCaptureCapacity> g_viewCaptures{};
std::size_t g_captureCursor{};

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
    std::uint32_t occupiedLow{};
    std::array<Candidate, kCandidateCapacity> candidates{};
    std::size_t candidateCount{};
};

/** Native bit-reader fields needed to isolate one entity-list decode. */
struct ReaderSnapshot {
    const std::byte* begin{};
    const std::byte* end{};
    std::int32_t loadedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    const std::byte* cursor{};
};

/** Bounded copy of one decoded entity record while its codec buffers are still owned by the call.
 */
struct DecodedRecordSnapshot {
    std::array<std::byte, kDecodedRecordMaskBytes> mask{};
    std::array<std::byte, kDecodedRecordCreateBytes> create{};
    std::array<std::byte, kDecodedRecordUpdateBytes> update{};
    std::uint32_t entity{};
    std::uint16_t cell{};
    std::uint16_t flags{};
    std::int16_t createSize{};
    std::int16_t updateSize{};
    std::size_t createCopied{};
    std::size_t updateCopied{};
};

/** Copies one record and its bounded codec scratch without retaining native pointers. */
[[nodiscard]] bool inspect_decoded_record(const void* recordsAddress,
                                          int count,
                                          DecodedRecordSnapshot& output) noexcept {
    output = {};
    if (recordsAddress == nullptr || count <= 0) {
        return false;
    }
    __try {
        const auto* const record = static_cast<const std::byte*>(recordsAddress);
        std::memcpy(&output.cell, record + kDecodedRecordCellOffset, sizeof output.cell);
        std::memcpy(&output.entity, record + kDecodedRecordEntityOffset, sizeof output.entity);
        std::memcpy(output.mask.data(), record + kDecodedRecordMaskOffset, output.mask.size());
        std::memcpy(
            &output.createSize, record + kDecodedRecordCreateSizeOffset, sizeof output.createSize);
        std::memcpy(
            &output.updateSize, record + kDecodedRecordUpdateSizeOffset, sizeof output.updateSize);
        std::memcpy(&output.flags, record + kDecodedRecordFlagsOffset, sizeof output.flags);
        const std::byte* create = nullptr;
        const std::byte* update = nullptr;
        std::memcpy(&create, record + kDecodedRecordCreateOffset, sizeof create);
        std::memcpy(&update, record + kDecodedRecordUpdateOffset, sizeof update);
        if (output.createSize > 0 && create != nullptr) {
            output.createCopied = static_cast<std::size_t>(output.createSize);
            if (output.createCopied > output.create.size()) {
                output.createCopied = output.create.size();
            }
            std::memcpy(output.create.data(), create, output.createCopied);
        }
        if (output.updateSize > 0 && update != nullptr) {
            output.updateCopied = static_cast<std::size_t>(output.updateSize);
            if (output.updateCopied > output.update.size()) {
                output.updateCopied = output.update.size();
            }
            std::memcpy(output.update.data(), update, output.updateCopied);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Converts a bounded byte range to uppercase hexadecimal. */
void record_hex(const std::byte* input, std::size_t count, char* output) noexcept {
    constexpr char kDigits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<std::uint8_t>(input[index]);
        output[index * 2] = kDigits[value >> 4];
        output[index * 2 + 1] = kDigits[value & 0x0F];
    }
    output[count * 2] = '\0';
}

/** Reports the baseline/update scratch produced by one successful native entity decode. */
void report_decoded_record(const void* recordsAddress, int count) noexcept {
    DecodedRecordSnapshot snapshot{};
    if (!inspect_decoded_record(recordsAddress, count, snapshot)) {
        return;
    }
    std::array<char, kDecodedRecordMaskBytes * 2 + 1> maskHex{};
    std::array<char, kDecodedRecordCreateBytes * 2 + 1> createHex{};
    std::array<char, kDecodedRecordUpdateBytes * 2 + 1> updateHex{};
    record_hex(snapshot.mask.data(), snapshot.mask.size(), maskHex.data());
    record_hex(snapshot.create.data(), snapshot.createCopied, createHex.data());
    record_hex(snapshot.update.data(), snapshot.updateCopied, updateHex.data());
    std::array<char, 1024> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=entity-record entity=0x%08X cell=0x%04X flags=0x%04X "
                      "create_size=%d create_copied=%zu create=%s update_size=%d update_copied=%zu "
                      "mask=%s update=%s",
                      snapshot.entity,
                      static_cast<unsigned>(snapshot.cell),
                      static_cast<unsigned>(snapshot.flags),
                      static_cast<int>(snapshot.createSize),
                      snapshot.createCopied,
                      createHex.data(),
                      static_cast<int>(snapshot.updateSize),
                      snapshot.updateCopied,
                      maskHex.data(),
                      updateHex.data());
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    sobject_update_probe::probe_decoded_record({snapshot.create.data(), snapshot.createCopied},
                                               {snapshot.update.data(), snapshot.updateCopied},
                                               {snapshot.mask.data(), snapshot.mask.size()});
}

/** Reads the native reader without changing its cursor or accumulator. */
[[nodiscard]] bool inspect_reader(const void* readerAddress, ReaderSnapshot& output) noexcept {
    output = {};
    if (readerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(readerAddress);
        std::memcpy(&output.begin, bytes, sizeof output.begin);
        std::memcpy(&output.end, bytes + 0x08, sizeof output.end);
        std::memcpy(&output.loadedBits, bytes + 0x20, sizeof output.loadedBits);
        std::memcpy(&output.totalBits, bytes + 0x24, sizeof output.totalBits);
        std::memcpy(&output.accumulator, bytes + 0x28, sizeof output.accumulator);
        std::memcpy(&output.pendingBits, bytes + 0x30, sizeof output.pendingBits);
        std::memcpy(&output.cursor, bytes + 0x38, sizeof output.cursor);
        return output.pendingBits <= 64 && output.begin != nullptr
               && reinterpret_cast<std::uintptr_t>(output.end)
                      >= reinterpret_cast<std::uintptr_t>(output.begin);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Resolves the authoritative manager and namespace from the decoder context. */
[[nodiscard]] const void* manager_from_context(const void* context,
                                               std::int32_t& namespaceId) noexcept {
    namespaceId = -1;
    if (context == nullptr) {
        return nullptr;
    }
    const void* manager = nullptr;
    __try {
        const auto* const bytes = static_cast<const std::byte*>(context);
        std::memcpy(&manager, bytes + 0x10, sizeof manager);
        if (manager != nullptr) {
            const std::byte* provider = nullptr;
            std::memcpy(&provider, static_cast<const std::byte*>(manager) + 8, sizeof provider);
            if (provider != nullptr) {
                std::memcpy(&namespaceId, provider + 8, sizeof namespaceId);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        manager = nullptr;
        namespaceId = -1;
    }
    return manager;
}

/** Claims one armed decoder report without allowing concurrent calls to exceed the budget. */
[[nodiscard]] bool claim_decode_trace() noexcept {
    std::uint32_t budget = g_decodeTraceBudget.load(std::memory_order_relaxed);
    while (budget != 0) {
        if (g_decodeTraceBudget.compare_exchange_weak(
                budget, budget - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

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
        output.occupiedLow = occupiedWords[0];
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
    mix(hash, snapshot.occupiedLow);
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
void report(const Snapshot& snapshot,
            int result,
            std::uint64_t token = 0,
            std::uint64_t schedulerKey = 0,
            std::uint8_t schedulerTag = 0) noexcept {
    std::array<char, 1024> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=gameplay stage=entity-slots manager=%p namespace=%d "
                                "free=%u occupied=%u occupied_low=0x%08X available=%u "
                                "result=%d token=0x%llX "
                                "key=0x%llX tag=%u",
                                static_cast<const void*>(snapshot.manager),
                                snapshot.namespaceId,
                                snapshot.freeCount,
                                snapshot.occupiedCount,
                                snapshot.occupiedLow,
                                snapshot.availableCount,
                                result,
                                static_cast<unsigned long long>(token),
                                static_cast<unsigned long long>(schedulerKey),
                                static_cast<unsigned>(schedulerTag));
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

/** @return True when publishing changed this token's captured view state. */
[[nodiscard]] bool publish(const ViewCapture& capture) noexcept {
    AcquireSRWLockExclusive(&g_captureLock);
    ViewCapture* destination = nullptr;
    for (ViewCapture& current : g_viewCaptures) {
        if (current.token == capture.token) {
            destination = &current;
            break;
        }
        if (destination == nullptr && current.token == 0) {
            destination = &current;
        }
    }
    if (destination == nullptr) {
        destination = &g_viewCaptures[g_captureCursor % g_viewCaptures.size()];
        ++g_captureCursor;
    }
    const bool changed = !(*destination == capture);
    *destination = capture;
    ReleaseSRWLockExclusive(&g_captureLock);
    return changed;
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
    ReaderSnapshot before{};
    ReaderSnapshot after{};
    std::int32_t namespaceId = -1;
    const void* manager = nullptr;
    bool trace = false;
    int result = 0;
    __try {
        if (lease.accepting) {
            manager = manager_from_context(context, namespaceId);
            // FUN_141718510 is the shared direct-entity decoder for every scheduler namespace.
            // The armed server create is already restricted to one stable view, so trace whichever
            // namespace owns that view instead of silently discarding an initial-zone capture.
            trace = namespaceId >= 0 && g_decodeTraceBudget.load(std::memory_order_relaxed) != 0
                    && inspect_reader(reader, before) && claim_decode_trace();
        }
        if (call != nullptr) {
            result = call(context, view, control, reader, capacity, records, count);
        }
        if (trace) {
            const bool afterReadable = inspect_reader(reader, after);
            int decodedCount = -1;
            __try {
                if (count != nullptr) {
                    decodedCount = *count;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                decodedCount = -1;
            }
            std::array<char, 768> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=entity-list-decode namespace=%d result=%d count=%d "
                "capacity=%d readable=%u consumed=%d "
                "before[total=%d loaded=%d pending=%u accum=0x%016llX cursor=%p end=%p] "
                "after[total=%d loaded=%d pending=%u accum=0x%016llX cursor=%p end=%p]",
                namespaceId,
                result,
                decodedCount,
                capacity,
                afterReadable ? 1U : 0U,
                afterReadable && after.totalBits >= before.totalBits
                    ? after.totalBits - before.totalBits
                    : -1,
                before.totalBits,
                before.loadedBits,
                static_cast<unsigned>(before.pendingBits),
                static_cast<unsigned long long>(before.accumulator),
                static_cast<const void*>(before.cursor),
                static_cast<const void*>(before.end),
                after.totalBits,
                after.loadedBits,
                static_cast<unsigned>(after.pendingBits),
                static_cast<unsigned long long>(after.accumulator),
                static_cast<const void*>(after.cursor),
                static_cast<const void*>(after.end));
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
            if (result == 0 && decodedCount > 0) {
                report_decoded_record(records, decodedCount);
            }
        }
        if (lease.accepting && manager != nullptr) {
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

void arm_decoder_trace() noexcept {
    g_decodeTraceBudget.store(4, std::memory_order_relaxed);
}

void observe_view(std::uint64_t token, const void* view) noexcept {
    if (view == nullptr) {
        return;
    }
    const void* manager = nullptr;
    std::uint64_t schedulerKey = 0;
    std::uint8_t schedulerTag = 0;
    std::int32_t schedulerViewCount = -1;
    std::int32_t schedulerRemoteViewCount = -1;
    std::array<std::byte, 16> schedulerSignature{};
    std::array<std::byte, 16> schedulerRemoteSignature{};
    std::array<std::uint64_t, ViewCapture::kSchedulerViewCapacity> schedulerViewKeys{};
    std::array<std::uint8_t, ViewCapture::kSchedulerViewCapacity> schedulerViewTags{};
    std::array<std::uint64_t, ViewCapture::kSchedulerViewCapacity> schedulerRemoteViewKeys{};
    std::array<std::uint8_t, ViewCapture::kSchedulerViewCapacity> schedulerRemoteViewTags{};
    __try {
        const auto* const bytes = static_cast<const std::byte*>(view);
        std::memcpy(&manager, bytes + 0xB8, sizeof manager);
        const std::byte* identity = nullptr;
        std::memcpy(&identity, bytes + 0x48, sizeof identity);
        if (identity != nullptr) {
            std::memcpy(&schedulerKey, identity + 0x20, sizeof schedulerKey);
        }
        if (manager != nullptr) {
            const auto* const managerBytes = static_cast<const std::byte*>(manager);
            std::memcpy(&schedulerTag, managerBytes + 0xD024, sizeof schedulerTag);
        }
        const std::byte* schedulerOwner = nullptr;
        std::memcpy(&schedulerOwner, bytes + 0x68, sizeof schedulerOwner);
        if (schedulerOwner != nullptr) {
            const auto* const scheduler = schedulerOwner + 0x38;
            std::memcpy(schedulerSignature.data(), scheduler + 0x10, schedulerSignature.size());
            std::memcpy(&schedulerViewCount, scheduler + 0x20, sizeof schedulerViewCount);
            std::memcpy(
                schedulerRemoteSignature.data(), scheduler + 0x58, schedulerRemoteSignature.size());
            std::memcpy(
                &schedulerRemoteViewCount, scheduler + 0x68, sizeof schedulerRemoteViewCount);
            if (schedulerViewCount >= 0
                && schedulerViewCount
                       <= static_cast<std::int32_t>(ViewCapture::kSchedulerViewCapacity)) {
                for (std::size_t index = 0; index < ViewCapture::kSchedulerViewCapacity; ++index) {
                    const auto* const entry = scheduler + 0x28 + index * 0x10;
                    std::memcpy(&schedulerViewKeys[index], entry, sizeof schedulerViewKeys[index]);
                    std::memcpy(
                        &schedulerViewTags[index], entry + 8, sizeof schedulerViewTags[index]);
                }
            }
            if (schedulerRemoteViewCount >= 0
                && schedulerRemoteViewCount
                       <= static_cast<std::int32_t>(ViewCapture::kSchedulerViewCapacity)) {
                for (std::size_t index = 0; index < ViewCapture::kSchedulerViewCapacity; ++index) {
                    const auto* const entry = scheduler + 0x70 + index * 0x10;
                    std::memcpy(&schedulerRemoteViewKeys[index],
                                entry,
                                sizeof schedulerRemoteViewKeys[index]);
                    std::memcpy(&schedulerRemoteViewTags[index],
                                entry + 8,
                                sizeof schedulerRemoteViewTags[index]);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (token == 0) {
        token = schedulerKey;
    }
    if (token == 0) {
        return;
    }

    Snapshot snapshot{};
    if (!inspect_manager(manager, snapshot)) {
        return;
    }
    ViewCapture capture{};
    capture.manager = manager;
    capture.token = token;
    capture.schedulerKey = schedulerKey;
    capture.schedulerTag = schedulerTag;
    capture.namespaceId = snapshot.namespaceId;
    capture.freeCount = snapshot.freeCount;
    capture.occupiedCount = snapshot.occupiedCount;
    capture.availableCount = snapshot.availableCount;
    capture.occupiedLow = snapshot.occupiedLow;
    if (snapshot.candidateCount != 0) {
        const Candidate& candidate = snapshot.candidates[0];
        capture.slot = candidate.slot;
        capture.handleGeneration = candidate.handleGeneration;
        capture.reservedGeneration = candidate.reservedGeneration;
        capture.objectGeneration = candidate.objectGeneration;
        capture.candidatePresent = true;
    }
    if (schedulerViewCount >= 0
        && schedulerViewCount <= static_cast<std::int32_t>(capture.schedulerViewKeys.size())) {
        capture.schedulerViewCount = static_cast<std::uint8_t>(schedulerViewCount);
        capture.schedulerSignature = schedulerSignature;
        capture.schedulerViewKeys = schedulerViewKeys;
        capture.schedulerViewTags = schedulerViewTags;
        capture.schedulerSignatureValid = true;
    }
    if (schedulerRemoteViewCount >= 0
        && schedulerRemoteViewCount
               <= static_cast<std::int32_t>(capture.schedulerRemoteViewKeys.size())) {
        capture.schedulerRemoteViewCount = static_cast<std::uint8_t>(schedulerRemoteViewCount);
        capture.schedulerRemoteSignature = schedulerRemoteSignature;
        capture.schedulerRemoteViewKeys = schedulerRemoteViewKeys;
        capture.schedulerRemoteViewTags = schedulerRemoteViewTags;
        capture.schedulerRemoteSignatureValid = true;
    }
    const bool changed = publish(capture);
    if (record_once(snapshot)) {
        report(snapshot, 0, token, schedulerKey, schedulerTag);
    }
    if (changed) {
        std::uint64_t signatureFirst = 0;
        std::uint64_t signatureSecond = 0;
        std::uint64_t remoteSignatureFirst = 0;
        std::uint64_t remoteSignatureSecond = 0;
        std::memcpy(&signatureFirst, capture.schedulerSignature.data(), sizeof signatureFirst);
        std::memcpy(&signatureSecond,
                    capture.schedulerSignature.data() + sizeof signatureFirst,
                    sizeof signatureSecond);
        std::memcpy(&remoteSignatureFirst,
                    capture.schedulerRemoteSignature.data(),
                    sizeof remoteSignatureFirst);
        std::memcpy(&remoteSignatureSecond,
                    capture.schedulerRemoteSignature.data() + sizeof remoteSignatureFirst,
                    sizeof remoteSignatureSecond);
        std::array<char, 768> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=gameplay stage=entity-view token=0x%llX key=0x%llX tag=%u namespace=%d "
            "signature=%u value=%016llX%016llX count=%u "
            "e0=0x%llX/%u e1=0x%llX/%u e2=0x%llX/%u "
            "remote=%u value=%016llX%016llX count=%u "
            "r0=0x%llX/%u r1=0x%llX/%u r2=0x%llX/%u "
            "occupied_low=0x%08X candidate=%u slot=%u hgen=%u rgen=%u ogen=%u",
            static_cast<unsigned long long>(capture.token),
            static_cast<unsigned long long>(capture.schedulerKey),
            static_cast<unsigned>(capture.schedulerTag),
            capture.namespaceId,
            capture.schedulerSignatureValid ? 1U : 0U,
            static_cast<unsigned long long>(signatureFirst),
            static_cast<unsigned long long>(signatureSecond),
            static_cast<unsigned>(capture.schedulerViewCount),
            static_cast<unsigned long long>(capture.schedulerViewKeys[0]),
            static_cast<unsigned>(capture.schedulerViewTags[0]),
            static_cast<unsigned long long>(capture.schedulerViewKeys[1]),
            static_cast<unsigned>(capture.schedulerViewTags[1]),
            static_cast<unsigned long long>(capture.schedulerViewKeys[2]),
            static_cast<unsigned>(capture.schedulerViewTags[2]),
            capture.schedulerRemoteSignatureValid ? 1U : 0U,
            static_cast<unsigned long long>(remoteSignatureFirst),
            static_cast<unsigned long long>(remoteSignatureSecond),
            static_cast<unsigned>(capture.schedulerRemoteViewCount),
            static_cast<unsigned long long>(capture.schedulerRemoteViewKeys[0]),
            static_cast<unsigned>(capture.schedulerRemoteViewTags[0]),
            static_cast<unsigned long long>(capture.schedulerRemoteViewKeys[1]),
            static_cast<unsigned>(capture.schedulerRemoteViewTags[1]),
            static_cast<unsigned long long>(capture.schedulerRemoteViewKeys[2]),
            static_cast<unsigned>(capture.schedulerRemoteViewTags[2]),
            capture.occupiedLow,
            capture.candidatePresent ? 1U : 0U,
            static_cast<unsigned>(capture.slot),
            static_cast<unsigned>(capture.handleGeneration),
            static_cast<unsigned>(capture.reservedGeneration),
            static_cast<unsigned>(capture.objectGeneration));
        if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

bool find(std::uint64_t token, ViewCapture& output) noexcept {
    output = {};
    output.namespaceId = -1;
    AcquireSRWLockShared(&g_captureLock);
    bool found = false;
    for (const ViewCapture& capture : g_viewCaptures) {
        if (capture.token == token) {
            output = capture;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_captureLock);
    return found;
}

void reset() noexcept {
    for (std::atomic_uint64_t& seen : g_seenSnapshots) {
        seen.store(0, std::memory_order_relaxed);
    }
    g_decodeTraceBudget.store(0, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_captureLock);
    g_viewCaptures = {};
    g_captureCursor = 0;
    ReleaseSRWLockExclusive(&g_captureLock);
}

} // namespace sunrise::client::hooks::network::entity_slot_probe
