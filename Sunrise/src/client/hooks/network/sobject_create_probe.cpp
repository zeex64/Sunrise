#include "sobject_create_probe.h"

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

namespace sunrise::client::hooks::network::sobject_create_probe {
namespace {

constexpr std::size_t kCreateBufferSize = 0x10;
constexpr std::size_t kSeenCapacity = 64;
constexpr std::size_t kFlushedCaptureCapacity = 32;

std::array<std::atomic_uint64_t, kSeenCapacity> g_seen{};

using Encoder = void(__fastcall*)(void*, const void*, void*);

/** Native bit-writer fields needed to isolate the bits appended by one codec call. */
struct WriterSnapshot {
    const std::byte* begin{};
    const std::byte* end{};
    std::int32_t flushedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    const std::byte* cursor{};
};

/** Reads the create input and bit-writer without changing either native object. */
[[nodiscard]] bool inspect(const void* createBuffer,
                           const void* writerAddress,
                           std::array<std::byte, kCreateBufferSize>& create,
                           WriterSnapshot& writer) noexcept {
    if (createBuffer == nullptr || writerAddress == nullptr) {
        return false;
    }
    __try {
        std::memcpy(create.data(), createBuffer, create.size());
        const auto* const bytes = static_cast<const std::byte*>(writerAddress);
        std::memcpy(&writer.begin, bytes, sizeof writer.begin);
        std::memcpy(&writer.end, bytes + 0x08, sizeof writer.end);
        std::memcpy(&writer.flushedBits, bytes + 0x20, sizeof writer.flushedBits);
        std::memcpy(&writer.totalBits, bytes + 0x24, sizeof writer.totalBits);
        std::memcpy(&writer.accumulator, bytes + 0x28, sizeof writer.accumulator);
        std::memcpy(&writer.pendingBits, bytes + 0x30, sizeof writer.pendingBits);
        std::memcpy(&writer.cursor, bytes + 0x38, sizeof writer.cursor);
        return writer.pendingBits <= 64;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** FNV-1a keeps the fixed atomic set independent of any particular create-buffer layout. */
[[nodiscard]] std::uint64_t
create_key(const std::array<std::byte, kCreateBufferSize>& create) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte value : create) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

/** @return True only for the first captured instance of this exact create buffer. */
[[nodiscard]] bool record_once(const std::array<std::byte, kCreateBufferSize>& create) noexcept {
    const std::uint64_t key = create_key(create);
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

/** Copies bytes flushed by the codec call, if its cursor movement is bounded and readable. */
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

/** Preserves native encoding and captures one exact kind-0 creation payload. */
__declspec(noinline) void __fastcall
encode_body(void* codec, const void* createBuffer, void* writerAddress) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectCreateEncoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Encoder>(lease.original);
    std::array<std::byte, kCreateBufferSize> create{};
    WriterSnapshot before{};
    WriterSnapshot after{};
    const bool beforeReadable = inspect(createBuffer, writerAddress, create, before);
    __try {
        if (call != nullptr) {
            call(codec, createBuffer, writerAddress);
        }
        std::array<std::byte, kCreateBufferSize> ignored{};
        const bool afterReadable =
            lease.accepting && inspect(createBuffer, writerAddress, ignored, after);
        if (beforeReadable && afterReadable && record_once(create)) {
            std::uint32_t rsat{};
            std::memcpy(&rsat, create.data(), sizeof rsat);
            const unsigned trailing = std::to_integer<unsigned>(create[4]) & 1U;
            const std::int32_t bitDelta = after.totalBits - before.totalBits;
            const bool scalarValid = bitDelta > 0 && bitDelta <= 64 && before.cursor == after.cursor
                                     && before.flushedBits == after.flushedBits;
            std::uint64_t appended{};
            if (scalarValid) {
                appended = bitDelta == 64 ? after.accumulator
                                          : after.accumulator & ((1ULL << bitDelta) - 1ULL);
            }

            std::array<std::byte, kFlushedCaptureCapacity> flushed{};
            const std::size_t flushedSize = capture_flushed(before, after, flushed);
            std::array<char, kCreateBufferSize * 2 + 1> createHex{};
            std::array<char, kFlushedCaptureCapacity * 2 + 1> flushedHex{};
            hex(create, createHex);
            hex(std::span<const std::byte>{flushed.data(), flushedSize}, flushedHex);

            std::array<char, 896> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=sobject-create rsat=0x%08X flag=%u input=%s bits=%d "
                "before[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                "after[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                "flushed_bytes=%zu flushed_hex=%s scalar=%u append=0x%016llX",
                rsat,
                trailing,
                createHex.data(),
                bitDelta,
                before.totalBits,
                before.flushedBits,
                static_cast<unsigned>(before.pendingBits),
                static_cast<unsigned long long>(before.accumulator),
                before.cursor,
                after.totalBits,
                after.flushedBits,
                static_cast<unsigned>(after.pendingBits),
                static_cast<unsigned long long>(after.accumulator),
                after.cursor,
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

} // namespace sunrise::client::hooks::network::sobject_create_probe
