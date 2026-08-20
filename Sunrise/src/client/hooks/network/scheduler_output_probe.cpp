#include "scheduler_output_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::scheduler_output_probe {
namespace {

constexpr std::uint8_t kLaneCount = 4;
constexpr std::uint8_t kMaximumViews = 3;
constexpr std::uint8_t kMaximumCalls = kLaneCount * kMaximumViews;
constexpr std::size_t kSeenCapacity = 32;
constexpr std::size_t kNativeLaneCapacityBytes = 0x2000;
constexpr std::uintptr_t kNativeLaneWriterStride = 0xD8;
constexpr ULONGLONG kPendingLifetimeMilliseconds = 500;
constexpr std::uint16_t kZeroViewSignatureBits = 130;
constexpr std::uint16_t kPerViewSignatureBits = 72;

enum class Lane : std::uint8_t {
    event,
    mask,
    entity,
    fixed,
};

struct WriterSnapshot {
    const std::byte* begin{};
    const std::byte* end{};
    const std::byte* cursor{};
    std::int32_t capacityBytes{};
    std::int32_t flushedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    std::int32_t checkpointDepth{};
    bool readable{};
};

struct LaneRecord {
    std::uintptr_t writerAddress{};
    std::int32_t beforeBits{};
    std::int32_t afterBits{};
    Lane lane{Lane::event};
};

struct PendingFrame {
    std::array<LaneRecord, kMaximumCalls> records{};
    std::uint64_t generation{};
    ULONGLONG startedAt{};
    ULONGLONG lastAt{};
    DWORD threadId{};
    std::uint8_t count{};
    bool active{};
};

struct LaneCall {
    WriterSnapshot before{};
    const void* writer{};
    std::uint64_t generation{};
    std::uint8_t ordinal{};
    Lane lane{Lane::event};
    bool tracked{};
};

using Finalizer = void(__fastcall*)(void*, std::int32_t, void*);

thread_local PendingFrame g_pending{};
std::atomic_uint64_t g_generation{1};
SRWLOCK g_seenLock{SRWLOCK_INIT};
std::array<std::uint64_t, kSeenCapacity> g_seen{};
std::size_t g_seenCount{};

/** @return Expected handler lane for one zero-based finalizer ordinal. */
[[nodiscard]] constexpr Lane lane_for(std::uint8_t ordinal) noexcept {
    switch (ordinal % kLaneCount) {
    case 0:
        return Lane::event;
    case 1:
        return Lane::mask;
    case 2:
        return Lane::entity;
    default:
        return Lane::fixed;
    }
}

/** Clears the current thread's tentative frame without touching native state. */
void clear_pending() noexcept {
    g_pending = {};
}

/** Starts a tentative frame at the first event finalizer. */
void start_pending(std::uint64_t generation, ULONGLONG now) noexcept {
    g_pending = {};
    g_pending.generation = generation;
    g_pending.startedAt = now;
    g_pending.lastAt = now;
    g_pending.threadId = GetCurrentThreadId();
    g_pending.active = true;
}

/** Reads the native bit writer without changing its checkpoint or accumulator. */
[[nodiscard]] bool inspect_writer(const void* writerAddress, WriterSnapshot& output) noexcept {
    output = {};
    if (writerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(writerAddress);
        std::memcpy(&output.begin, bytes, sizeof output.begin);
        std::memcpy(&output.end, bytes + 0x08, sizeof output.end);
        std::memcpy(&output.capacityBytes, bytes + 0x10, sizeof output.capacityBytes);
        std::memcpy(&output.flushedBits, bytes + 0x20, sizeof output.flushedBits);
        std::memcpy(&output.totalBits, bytes + 0x24, sizeof output.totalBits);
        std::memcpy(&output.accumulator, bytes + 0x28, sizeof output.accumulator);
        std::memcpy(&output.pendingBits, bytes + 0x30, sizeof output.pendingBits);
        std::memcpy(&output.cursor, bytes + 0x38, sizeof output.cursor);
        std::memcpy(&output.checkpointDepth, bytes + 0x40, sizeof output.checkpointDepth);

        const auto begin = reinterpret_cast<std::uintptr_t>(output.begin);
        const auto end = reinterpret_cast<std::uintptr_t>(output.end);
        const auto cursor = reinterpret_cast<std::uintptr_t>(output.cursor);
        const bool pointers = begin != 0 && end >= begin && cursor >= begin && cursor <= end;
        const std::uintptr_t span = pointers ? end - begin : 0;
        output.readable =
            pointers && span != 0 && span <= kNativeLaneCapacityBytes && output.capacityBytes > 0
            && output.capacityBytes <= static_cast<std::int32_t>(kNativeLaneCapacityBytes)
            && output.pendingBits <= 64 && output.flushedBits >= 0
            && output.totalBits >= output.flushedBits
            && output.totalBits <= static_cast<std::int32_t>(kNativeLaneCapacityBytes * 8)
            && output.checkpointDepth >= 0 && output.checkpointDepth <= 32;
        return output.readable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Opens one strict finalizer call, resetting a stale or new-frame tentative sequence. */
[[nodiscard]] LaneCall begin_lane(bool entityFinalizer, const void* writer) noexcept {
    LaneCall call{};
    const std::uint64_t generation = g_generation.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    WriterSnapshot before{};
    if (!inspect_writer(writer, before)) {
        clear_pending();
        return call;
    }

    if (g_pending.active
        && (g_pending.generation != generation
            || now - g_pending.lastAt > kPendingLifetimeMilliseconds)) {
        clear_pending();
    }

    if (g_pending.active && g_pending.count != 0) {
        const auto previous = g_pending.records[g_pending.count - 1].writerAddress;
        const auto current = reinterpret_cast<std::uintptr_t>(writer);
        const bool contiguous =
            previous <= std::numeric_limits<std::uintptr_t>::max() - kNativeLaneWriterStride
            && current == previous + kNativeLaneWriterStride;
        const Lane expectedLane =
            g_pending.count < kMaximumCalls ? lane_for(g_pending.count) : Lane::event;
        const bool expectedFinalizer = (expectedLane == Lane::entity) == entityFinalizer;
        if (g_pending.count >= kMaximumCalls || !contiguous || !expectedFinalizer) {
            clear_pending();
        }
    }

    if (!g_pending.active) {
        if (entityFinalizer) {
            return call;
        }
        start_pending(generation, now);
    }
    if (g_pending.count >= kMaximumCalls) {
        clear_pending();
        return call;
    }
    const Lane lane = lane_for(g_pending.count);
    if ((lane == Lane::entity) != entityFinalizer) {
        clear_pending();
        return call;
    }

    call.before = before;
    call.writer = writer;
    call.generation = generation;
    call.ordinal = g_pending.count;
    call.lane = lane;
    call.tracked = true;
    return call;
}

/** Completes one lane only when the writer and tentative frame remained coherent. */
void finish_lane(const LaneCall& call) noexcept {
    if (!call.tracked) {
        return;
    }
    WriterSnapshot after{};
    const std::uint64_t generation = g_generation.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    const std::int64_t delta = [&]() noexcept -> std::int64_t {
        if (!inspect_writer(call.writer, after)) {
            return -1;
        }
        return static_cast<std::int64_t>(after.totalBits)
               - static_cast<std::int64_t>(call.before.totalBits);
    }();
    if (!g_pending.active || g_pending.generation != call.generation
        || generation != call.generation || g_pending.threadId != GetCurrentThreadId()
        || g_pending.count != call.ordinal || lane_for(call.ordinal) != call.lane || delta != 1) {
        clear_pending();
        return;
    }

    LaneRecord& record = g_pending.records[g_pending.count];
    record.writerAddress = reinterpret_cast<std::uintptr_t>(call.writer);
    record.beforeBits = call.before.totalBits;
    record.afterBits = after.totalBits;
    record.lane = call.lane;
    ++g_pending.count;
    g_pending.lastAt = now;
}

/** Preserves one native finalizer and snapshots its completed lane while calls are accepted. */
template <HookSlot Slot, bool EntityFinalizer>
__declspec(noinline) void __fastcall
finalize_lane(void* context, std::int32_t schedulerArgument, void* writer) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, Slot, coordinator::ConsumerKind::none);
    const auto original = reinterpret_cast<Finalizer>(lease.original);
    LaneCall trace{};
    __try {
        if (lease.accepting) {
            trace = begin_lane(EntityFinalizer, writer);
        }
        if (original != nullptr) {
            original(context, schedulerArgument, writer);
        }
        finish_lane(trace);
    } __finally {
        coordinator::g_callEgress();
    }
}

/** FNV-1a mixes one trivially copied field into a shape fingerprint. */
template <typename Value> void mix(std::uint64_t& hash, const Value& value) noexcept {
    const auto bytes = std::as_bytes(std::span{&value, 1});
    for (const std::byte byte : bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
}

/** @return Stable fingerprint for one complete signature and finalized lane-width shape. */
[[nodiscard]] std::uint64_t shape_key(const PendingFrame& frame,
                                      std::uint16_t signatureBits,
                                      std::span<const std::byte, 16> signature) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    mix(hash, signatureBits);
    for (const std::byte byte : signature) {
        mix(hash, byte);
    }
    mix(hash, frame.count);
    for (std::size_t index = 0; index < frame.count; ++index) {
        const LaneRecord& record = frame.records[index];
        mix(hash, record.lane);
        mix(hash, record.afterBits);
    }
    return hash == 0 ? 1 : hash;
}

/** @return One-based bounded shape number, or zero for a duplicate/full registry. */
[[nodiscard]] std::uint32_t reserve_shape(std::uint64_t key) noexcept {
    AcquireSRWLockExclusive(&g_seenLock);
    for (std::size_t index = 0; index < g_seenCount; ++index) {
        if (g_seen[index] == key) {
            ReleaseSRWLockExclusive(&g_seenLock);
            return 0;
        }
    }
    if (g_seenCount >= g_seen.size()) {
        ReleaseSRWLockExclusive(&g_seenLock);
        return 0;
    }
    g_seen[g_seenCount] = key;
    const auto number = static_cast<std::uint32_t>(++g_seenCount);
    ReleaseSRWLockExclusive(&g_seenLock);
    return number;
}

/** @return One through three for a supported native scheduler signature width. */
[[nodiscard]] std::uint8_t signature_views(std::uint16_t bitCount) noexcept {
    if (bitCount <= kZeroViewSignatureBits) {
        return 0;
    }
    const std::uint16_t remainder = bitCount - kZeroViewSignatureBits;
    if (remainder % kPerViewSignatureBits != 0) {
        return 0;
    }
    const auto views = static_cast<std::uint8_t>(remainder / kPerViewSignatureBits);
    return views <= kMaximumViews ? views : 0;
}

/** Emits one compact shape line after signature validation and duplicate filtering. */
void report_frame(const PendingFrame& frame,
                  std::uint32_t shape,
                  std::uint16_t signatureBits,
                  std::uint8_t views,
                  const std::array<std::byte, 16>& signature) noexcept {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::memcpy(&first, signature.data(), sizeof first);
    std::memcpy(&second, signature.data() + sizeof first, sizeof second);

    std::uint32_t handlerBits = 0;
    for (std::size_t index = 0; index < frame.count; ++index) {
        handlerBits += static_cast<std::uint32_t>(frame.records[index].afterBits);
    }
    const std::uint32_t predictedBits = 1U + signatureBits + handlerBits;
    std::array<char, 1280> line{};
    int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=scheduler-outbound-shape shape=%u status=complete tid=%lu "
                      "signature=%016llX%016llX sig_bits=%u views=%u calls=%u handler_bits=%u "
                      "predicted_bits=%u age_ms=%llu",
                      shape,
                      static_cast<unsigned long>(frame.threadId),
                      static_cast<unsigned long long>(first),
                      static_cast<unsigned long long>(second),
                      static_cast<unsigned>(signatureBits),
                      static_cast<unsigned>(views),
                      static_cast<unsigned>(frame.count),
                      handlerBits,
                      predictedBits,
                      static_cast<unsigned long long>(frame.lastAt - frame.startedAt));
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    std::size_t used = static_cast<std::size_t>(written);
    for (std::uint8_t view = 0; view < views; ++view) {
        const std::size_t base = static_cast<std::size_t>(view) * kLaneCount;
        const LaneRecord& event = frame.records[base];
        const LaneRecord& mask = frame.records[base + 1];
        const LaneRecord& entity = frame.records[base + 2];
        const LaneRecord& fixed = frame.records[base + 3];
        const auto delta = [](const LaneRecord& record) noexcept {
            return record.afterBits - record.beforeBits;
        };
        written = std::snprintf(line.data() + used,
                                line.size() - used,
                                " v%u[event=%d/+%d mask=%d/+%d entity=%d/+%d fixed=%d/+%d]",
                                static_cast<unsigned>(view),
                                event.afterBits,
                                delta(event),
                                mask.afterBits,
                                delta(mask),
                                entity.afterBits,
                                delta(entity),
                                fixed.afterBits,
                                delta(fixed));
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size() - used) {
            return;
        }
        used += static_cast<std::size_t>(written);
    }
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
}

} // namespace

void* zero_finalizer_entry_point() noexcept {
    return reinterpret_cast<void*>(&finalize_lane<HookSlot::schedulerZeroFinalizer, false>);
}

void* entity_finalizer_entry_point() noexcept {
    return reinterpret_cast<void*>(&finalize_lane<HookSlot::schedulerEntityFinalizer, true>);
}

void commit_signature(std::uint16_t bitCount, const std::array<std::byte, 16>& value) noexcept {
    const std::uint8_t views = signature_views(bitCount);
    const std::uint64_t generation = g_generation.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    if (!g_pending.active || views == 0 || g_pending.generation != generation
        || g_pending.threadId != GetCurrentThreadId()
        || now - g_pending.lastAt > kPendingLifetimeMilliseconds
        || g_pending.count != static_cast<std::uint8_t>(views * kLaneCount)) {
        clear_pending();
        return;
    }

    PendingFrame frame = g_pending;
    clear_pending();
    const std::uint64_t key = shape_key(frame, bitCount, value);
    const std::uint32_t shape = reserve_shape(key);
    if (shape != 0) {
        report_frame(frame, shape, bitCount, views, value);
    }
}

void reset() noexcept {
    (void)g_generation.fetch_add(1, std::memory_order_acq_rel);
    clear_pending();
    AcquireSRWLockExclusive(&g_seenLock);
    g_seen.fill(0);
    g_seenCount = 0;
    ReleaseSRWLockExclusive(&g_seenLock);
}

} // namespace sunrise::client::hooks::network::scheduler_output_probe
