#include "sobject_update_probe.h"

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

namespace sunrise::client::hooks::network::sobject_update_probe {
namespace {

constexpr std::size_t kCreateBufferSize = 0x10;
constexpr std::size_t kMaskCaptureSize = 0x10;
constexpr std::size_t kSeenCapacity = 32;
constexpr std::size_t kFlushedCaptureCapacity = 64;

std::array<std::atomic_uint64_t, kSeenCapacity> g_seen{};

using Encoder = std::uint64_t(__fastcall*)(void*, const void*);

/** Native bit-writer fields needed to isolate the bits appended by one codec call. */
struct WriterSnapshot {
    std::int32_t flushedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    const std::byte* cursor{};
};

/** Stable, shallow fields read from the kind-0 update context before native encoding. */
struct UpdateSnapshot {
    const void* dirty{};
    const void* sent{};
    const void* createBuffer{};
    std::int64_t componentBase{};
    void* writerAddress{};
    std::uint8_t diagnostics{};
    std::array<std::byte, kCreateBufferSize> create{};
    std::array<std::byte, kMaskCaptureSize> dirtyBytes{};
    std::array<std::byte, kMaskCaptureSize> sentBytes{};
    WriterSnapshot writer{};
};

/** Reads only the bit-writer fields used by the probe. */
[[nodiscard]] bool inspect_writer(const void* writerAddress, WriterSnapshot& output) noexcept {
    if (writerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(writerAddress);
        std::memcpy(&output.flushedBits, bytes + 0x20, sizeof output.flushedBits);
        std::memcpy(&output.totalBits, bytes + 0x24, sizeof output.totalBits);
        std::memcpy(&output.accumulator, bytes + 0x28, sizeof output.accumulator);
        std::memcpy(&output.pendingBits, bytes + 0x30, sizeof output.pendingBits);
        std::memcpy(&output.cursor, bytes + 0x38, sizeof output.cursor);
        return output.pendingBits <= 64;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Reads the update context and its two mask objects without changing native state. */
[[nodiscard]] bool inspect_update(const void* contextAddress, UpdateSnapshot& output) noexcept {
    if (contextAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const context = static_cast<const std::byte*>(contextAddress);
        std::memcpy(&output.dirty, context + 0x08, sizeof output.dirty);
        std::memcpy(&output.sent, context + 0x10, sizeof output.sent);
        std::memcpy(&output.createBuffer, context + 0x20, sizeof output.createBuffer);
        std::memcpy(&output.componentBase, context + 0x30, sizeof output.componentBase);
        std::memcpy(&output.writerAddress, context + 0x48, sizeof output.writerAddress);
        std::memcpy(&output.diagnostics, context + 0x54, sizeof output.diagnostics);
        if (output.dirty == nullptr || output.sent == nullptr || output.createBuffer == nullptr
            || output.writerAddress == nullptr) {
            return false;
        }
        std::memcpy(output.create.data(), output.createBuffer, output.create.size());
        std::memcpy(output.dirtyBytes.data(), output.dirty, output.dirtyBytes.size());
        std::memcpy(output.sentBytes.data(), output.sent, output.sentBytes.size());
        return inspect_writer(output.writerAddress, output.writer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** FNV-1a hashes the stable input bytes into the bounded duplicate filter. */
[[nodiscard]] std::uint64_t update_key(const UpdateSnapshot& update) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto append = [&hash](std::span<const std::byte> bytes) noexcept {
        for (const std::byte value : bytes) {
            hash ^= std::to_integer<std::uint8_t>(value);
            hash *= 1099511628211ULL;
        }
    };
    append(update.create);
    append(update.dirtyBytes);
    append(update.sentBytes);
    return hash == 0 ? 1 : hash;
}

/** @return True only for the first bounded capture of this input state. */
[[nodiscard]] bool record_once(const UpdateSnapshot& update) noexcept {
    const std::uint64_t key = update_key(update);
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

/** Converts a bounded byte capture to uppercase hexadecimal. */
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

/** Copies bytes flushed by the update call when its cursor movement is bounded and readable. */
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

/** Preserves native encoding and captures a bounded set of kind-0 update payloads. */
__declspec(noinline) std::uint64_t __fastcall encode_body(void* codec,
                                                          const void* contextAddress) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectUpdateEncoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Encoder>(lease.original);
    UpdateSnapshot before{};
    const bool beforeReadable = inspect_update(contextAddress, before);
    std::uint64_t result{};
    __try {
        if (call != nullptr) {
            result = call(codec, contextAddress);
        }
        WriterSnapshot after{};
        const bool afterReadable = lease.accepting && inspect_writer(before.writerAddress, after);
        if (beforeReadable && afterReadable && record_once(before)) {
            std::uint32_t rsat{};
            std::memcpy(&rsat, before.create.data(), sizeof rsat);
            const unsigned trailing = std::to_integer<unsigned>(before.create[4]) & 1U;
            const std::int32_t bitDelta = after.totalBits - before.writer.totalBits;
            const bool scalarValid = bitDelta > 0 && bitDelta <= 64
                                     && before.writer.cursor == after.cursor
                                     && before.writer.flushedBits == after.flushedBits;
            std::uint64_t appended{};
            if (scalarValid) {
                appended = bitDelta == 64 ? after.accumulator
                                          : after.accumulator & ((1ULL << bitDelta) - 1ULL);
            }

            std::array<std::byte, kFlushedCaptureCapacity> flushed{};
            const std::size_t flushedSize = capture_flushed(before.writer, after, flushed);
            std::array<char, kCreateBufferSize * 2 + 1> createHex{};
            std::array<char, kMaskCaptureSize * 2 + 1> dirtyHex{};
            std::array<char, kMaskCaptureSize * 2 + 1> sentHex{};
            std::array<char, kFlushedCaptureCapacity * 2 + 1> flushedHex{};
            hex(before.create, createHex);
            hex(before.dirtyBytes, dirtyHex);
            hex(before.sentBytes, sentHex);
            hex(std::span<const std::byte>{flushed.data(), flushedSize}, flushedHex);

            std::array<char, core::log::kLineCapacity> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=sobject-update rsat=0x%08X flag=%u input=%s base=%lld "
                "diagnostics=%u dirty=%s sent=%s bits=%d "
                "before[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                "after[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                "flushed_bytes=%zu flushed_hex=%s scalar=%u append=0x%016llX",
                rsat,
                trailing,
                createHex.data(),
                static_cast<long long>(before.componentBase),
                static_cast<unsigned>(before.diagnostics),
                dirtyHex.data(),
                sentHex.data(),
                bitDelta,
                before.writer.totalBits,
                before.writer.flushedBits,
                static_cast<unsigned>(before.writer.pendingBits),
                static_cast<unsigned long long>(before.writer.accumulator),
                static_cast<const void*>(before.writer.cursor),
                after.totalBits,
                after.flushedBits,
                static_cast<unsigned>(after.pendingBits),
                static_cast<unsigned long long>(after.accumulator),
                static_cast<const void*>(after.cursor),
                flushedSize,
                flushedHex.data(),
                scalarValid ? 1U : 0U,
                static_cast<unsigned long long>(appended));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* encoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&encode_body);
}

void reset() noexcept {
    for (std::atomic_uint64_t& seen : g_seen) {
        seen.store(0, std::memory_order_relaxed);
    }
}

} // namespace sunrise::client::hooks::network::sobject_update_probe
