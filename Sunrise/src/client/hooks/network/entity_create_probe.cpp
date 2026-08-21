#include "entity_create_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::entity_create_probe {
namespace {

constexpr std::size_t kSeenCapacity = 64;
constexpr std::size_t kFlushedCaptureCapacity = 64;
/** One actor-like native record is 3,745 bits, including 472 newly flushed bytes. */
constexpr std::size_t kExactCaptureCapacity = 512;
/** Keeps each hexadecimal capture line comfortably below the shared 1 KiB log limit. */
constexpr std::size_t kExactCaptureChunkCapacity = 352;
constexpr std::uint32_t kActorLikeRsat = 0x80EF143E;

std::array<std::atomic_uint64_t, kSeenCapacity> g_seen{};
std::atomic_bool g_actorLikeCaptured{};
thread_local std::uint32_t g_recordDepth{};
thread_local std::uint32_t g_recordRsat{};

using Encoder = std::uint8_t(__fastcall*)(
    void*, void*, std::uint32_t, const void*, std::uint64_t, std::uint32_t);

/** Native bit-writer fields needed to isolate one object body's encoded bits. */
struct WriterSnapshot {
    const std::byte* begin{};
    const std::byte* end{};
    std::int32_t flushedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    const std::byte* cursor{};
};

/** Copies one exact cursor span while retaining the two partial accumulator states separately. */
[[nodiscard]] std::size_t
capture_exact(const WriterSnapshot& before,
              const WriterSnapshot& after,
              std::array<std::byte, kExactCaptureCapacity>& output) noexcept {
    const auto bufferBegin = reinterpret_cast<std::uintptr_t>(before.begin);
    const auto bufferEnd = reinterpret_cast<std::uintptr_t>(before.end);
    const auto begin = reinterpret_cast<std::uintptr_t>(before.cursor);
    const auto end = reinterpret_cast<std::uintptr_t>(after.cursor);
    if (bufferBegin == 0 || bufferEnd < bufferBegin || before.begin != after.begin
        || before.end != after.end || begin < bufferBegin || end < begin || end > bufferEnd
        || end - begin > output.size()) {
        return 0;
    }
    const std::size_t size = static_cast<std::size_t>(end - begin);
    __try {
        if (size != 0) {
            std::memcpy(output.data(), before.cursor, size);
        }
        return size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/** Emits one exact, bounded actor record as a metadata line followed by two or fewer hex chunks. */
void report_exact(std::uint32_t entity,
                  std::uint8_t flags,
                  std::uint32_t rsat,
                  const WriterSnapshot& before,
                  const WriterSnapshot& after) noexcept {
    if (rsat != kActorLikeRsat || after.totalBits <= before.totalBits
        || after.flushedBits < before.flushedBits) {
        return;
    }
    std::array<std::byte, kExactCaptureCapacity> captured{};
    const std::size_t capturedSize = capture_exact(before, after, captured);
    const std::int32_t bitDelta = after.totalBits - before.totalBits;
    const std::int64_t reconstructedBits =
        static_cast<std::int64_t>(capturedSize) * 8 + after.pendingBits - before.pendingBits;
    const bool exact =
        capturedSize != 0 && reconstructedBits == bitDelta
        && after.flushedBits - before.flushedBits == static_cast<std::int32_t>(capturedSize * 8);
    if (!exact || g_actorLikeCaptured.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    std::array<char, core::log::kLineCapacity> header{};
    const int headerWritten =
        std::snprintf(header.data(),
                      header.size(),
                      "ev=gameplay stage=entity-record-capture result=ok rsat=0x%08X entity=0x%08X "
                      "flags=0x%02X bits=%d raw_bytes=%zu prefix_bits=%u prefix_accum=0x%016llX "
                      "suffix_bits=%u suffix_accum=0x%016llX before_total=%d before_flushed=%d "
                      "after_total=%d after_flushed=%d",
                      rsat,
                      entity,
                      static_cast<unsigned>(flags),
                      bitDelta,
                      capturedSize,
                      static_cast<unsigned>(before.pendingBits),
                      static_cast<unsigned long long>(before.accumulator),
                      static_cast<unsigned>(after.pendingBits),
                      static_cast<unsigned long long>(after.accumulator),
                      before.totalBits,
                      before.flushedBits,
                      after.totalBits,
                      after.flushedBits);
    if (headerWritten > 0 && static_cast<std::size_t>(headerWritten) < header.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {header.data(), static_cast<std::size_t>(headerWritten)});
    }

    const std::size_t partCount =
        (capturedSize + kExactCaptureChunkCapacity - 1) / kExactCaptureChunkCapacity;
    for (std::size_t part = 0; part < partCount; ++part) {
        const std::size_t offset = part * kExactCaptureChunkCapacity;
        const std::size_t count = (capturedSize - offset) < kExactCaptureChunkCapacity
                                      ? capturedSize - offset
                                      : kExactCaptureChunkCapacity;
        std::array<char, core::log::kLineCapacity> line{};
        const int prefix = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=gameplay stage=entity-record-wire rsat=0x%08X "
                                         "entity=0x%08X part=%zu/%zu offset=%zu bytes=%zu hex=",
                                         rsat,
                                         entity,
                                         part + 1,
                                         partCount,
                                         offset,
                                         count);
        if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
            continue;
        }
        std::size_t used = static_cast<std::size_t>(prefix);
        if (core::log::append_hex(
                line, used, std::span<const std::byte>{captured.data() + offset, count})) {
            core::log::write(
                core::log::Channel::client, core::log::Level::info, {line.data(), used});
        }
    }
}

/** Converts a bounded byte capture to a compact uppercase hexadecimal field. */
void hex(std::span<const std::byte> input, std::span<char> output) noexcept {
    constexpr char kDigits[] = "0123456789ABCDEF";
    if (output.empty()) {
        return;
    }
    const std::size_t count =
        input.size() < (output.size() - 1) / 2 ? input.size() : (output.size() - 1) / 2;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<std::uint8_t>(input[index]);
        output[index * 2] = kDigits[value >> 4];
        output[index * 2 + 1] = kDigits[value & 0x0F];
    }
    output[count * 2] = '\0';
}

/** Copies bytes flushed by the object-body call when cursor movement is bounded and readable. */
[[nodiscard]] std::size_t
capture_flushed(const WriterSnapshot& before,
                const WriterSnapshot& after,
                std::array<std::byte, kFlushedCaptureCapacity>& output) noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(before.cursor);
    const auto end = reinterpret_cast<std::uintptr_t>(after.cursor);
    if (begin == 0 || end < begin || end - begin > output.size()) {
        return 0;
    }
    const std::size_t size = static_cast<std::size_t>(end - begin);
    __try {
        if (size != 0) {
            std::memcpy(output.data(), before.cursor, size);
        }
        return size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/** Reads the create flags and writer without modifying either native object. */
[[nodiscard]] bool inspect(const void* record,
                           const void* writerAddress,
                           std::uint8_t& flags,
                           WriterSnapshot& writer) noexcept {
    if (record == nullptr || writerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const recordBytes = static_cast<const std::byte*>(record);
        flags = std::to_integer<std::uint8_t>(recordBytes[0x38]);
        const auto* const writerBytes = static_cast<const std::byte*>(writerAddress);
        std::memcpy(&writer.begin, writerBytes, sizeof writer.begin);
        std::memcpy(&writer.end, writerBytes + 0x08, sizeof writer.end);
        std::memcpy(&writer.flushedBits, writerBytes + 0x20, sizeof writer.flushedBits);
        std::memcpy(&writer.totalBits, writerBytes + 0x24, sizeof writer.totalBits);
        std::memcpy(&writer.accumulator, writerBytes + 0x28, sizeof writer.accumulator);
        std::memcpy(&writer.pendingBits, writerBytes + 0x30, sizeof writer.pendingBits);
        std::memcpy(&writer.cursor, writerBytes + 0x38, sizeof writer.cursor);
        return writer.pendingBits <= 64 && writer.begin != nullptr
               && reinterpret_cast<std::uintptr_t>(writer.end)
                      >= reinterpret_cast<std::uintptr_t>(writer.begin)
               && reinterpret_cast<std::uintptr_t>(writer.cursor)
                      >= reinterpret_cast<std::uintptr_t>(writer.begin)
               && reinterpret_cast<std::uintptr_t>(writer.cursor)
                      <= reinterpret_cast<std::uintptr_t>(writer.end);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** @return True only for the first observed create of this exact replicated entity handle. */
[[nodiscard]] bool record_once(std::uint32_t entity) noexcept {
    const std::uint64_t key = static_cast<std::uint64_t>(entity) + 1;
    for (std::atomic_uint64_t& seen : g_seen) {
        std::uint64_t current = seen.load(std::memory_order_relaxed);
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
    }
    return false;
}

/** Preserves native encoding and reports successful object bodies that include creation. */
__declspec(noinline) std::uint8_t __fastcall encode_body(void* manager,
                                                         void* writerAddress,
                                                         std::uint32_t entity,
                                                         const void* record,
                                                         std::uint64_t updateContext,
                                                         std::uint32_t auxiliary) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::entityCreateEncoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Encoder>(lease.original);
    std::uint8_t flags = 0;
    WriterSnapshot before{};
    WriterSnapshot after{};
    const bool beforeReadable = inspect(record, writerAddress, flags, before);
    const bool recordRoot = g_recordDepth++ == 0;
    if (recordRoot) {
        g_recordRsat = 0;
    }
    std::uint8_t result = 0;
    __try {
        if (call != nullptr) {
            result = call(manager, writerAddress, entity, record, updateContext, auxiliary);
        }
        std::uint8_t ignoredFlags = 0;
        const bool afterReadable =
            lease.accepting && inspect(record, writerAddress, ignoredFlags, after);
        if (result != 0 && recordRoot && beforeReadable && afterReadable) {
            report_exact(entity, flags, g_recordRsat, before, after);
        }
        if (result != 0 && beforeReadable && afterReadable && (flags & 1U) != 0
            && record_once(entity)) {
            const std::int32_t bitDelta = after.totalBits - before.totalBits;
            const bool scalarValid = bitDelta > 0 && bitDelta <= 64 && before.cursor == after.cursor
                                     && before.flushedBits == after.flushedBits;
            std::uint64_t appended = 0;
            if (scalarValid) {
                appended = bitDelta == 64 ? after.accumulator
                                          : after.accumulator & ((1ULL << bitDelta) - 1ULL);
            }

            std::array<std::byte, kFlushedCaptureCapacity> flushed{};
            const std::size_t flushedSize = capture_flushed(before, after, flushed);
            std::array<char, kFlushedCaptureCapacity * 2 + 1> flushedHex{};
            hex(std::span<const std::byte>{flushed.data(), flushedSize}, flushedHex);

            std::array<char, 768> line{};
            const int written =
                std::snprintf(line.data(),
                              line.size(),
                              "ev=gameplay stage=entity-create entity=0x%08X flags=0x%02X bits=%d "
                              "context=0x%016llX auxiliary=0x%08X "
                              "before[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                              "after[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                              "flushed_bytes=%zu flushed_hex=%s scalar=%u append=0x%016llX",
                              entity,
                              static_cast<unsigned>(flags),
                              bitDelta,
                              static_cast<unsigned long long>(updateContext),
                              auxiliary,
                              before.totalBits,
                              before.flushedBits,
                              static_cast<unsigned>(before.pendingBits),
                              static_cast<unsigned long long>(before.accumulator),
                              static_cast<const void*>(before.cursor),
                              after.totalBits,
                              after.flushedBits,
                              static_cast<unsigned>(after.pendingBits),
                              static_cast<unsigned long long>(after.accumulator),
                              static_cast<const void*>(after.cursor),
                              flushedSize,
                              flushedHex.data(),
                              scalarValid ? 1U : 0U,
                              static_cast<unsigned long long>(appended));
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        if (recordRoot) {
            g_recordRsat = 0;
        }
        --g_recordDepth;
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* encoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&encode_body);
}

void observe_sobject_rsat(std::uint32_t rsat) noexcept {
    if (g_recordDepth != 0 && (g_recordRsat == 0 || rsat == kActorLikeRsat)) {
        g_recordRsat = rsat;
    }
}

void reset() noexcept {
    for (std::atomic_uint64_t& seen : g_seen) {
        seen.store(0, std::memory_order_relaxed);
    }
    g_actorLikeCaptured.store(false, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::network::entity_create_probe
