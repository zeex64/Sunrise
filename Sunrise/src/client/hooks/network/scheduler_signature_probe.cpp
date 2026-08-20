#include "scheduler_signature_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"
#include "scheduler_output_probe.h"

namespace sunrise::client::hooks::network::scheduler_signature_probe {
namespace {

/** Schema used for the scheduler's 16-byte local/remote signature value. */
constexpr std::uint32_t kSchedulerSignatureSchema = 0x80806AEAU;
constexpr std::uint16_t kMaximumEncodedBits = 511;

SRWLOCK g_captureLock{SRWLOCK_INIT};
Capture g_capture{};

using Encoder = std::uint8_t(__fastcall*)(std::uint32_t, const void*, void*, std::uint32_t);

struct WriterSnapshot {
    std::int32_t totalBits{};
};

/** Reads only the native writer field needed to measure one schema call. */
[[nodiscard]] bool inspect_writer(const void* writerAddress, WriterSnapshot& output) noexcept {
    if (writerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(writerAddress);
        std::memcpy(&output.totalBits, bytes + 0x24, sizeof output.totalBits);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Copies the fixed native signature value without dereferencing it after the base call. */
[[nodiscard]] bool inspect_value(const void* valueAddress,
                                 std::array<std::byte, Capture::kValueSize>& output) noexcept {
    if (valueAddress == nullptr) {
        return false;
    }
    __try {
        std::memcpy(output.data(), valueAddress, output.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Preserves generic schema encoding and measures only schema 0x80806AEA in output mode. */
__declspec(noinline) std::uint8_t __fastcall encode(std::uint32_t schema,
                                                    const void* valueAddress,
                                                    void* writerAddress,
                                                    std::uint32_t mode) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::schedulerSignatureEncoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Encoder>(lease.original);

    WriterSnapshot before{};
    WriterSnapshot after{};
    std::array<std::byte, Capture::kValueSize> value{};
    const bool target = schema == kSchedulerSignatureSchema && mode == 1;
    const bool beforeReadable =
        target && inspect_writer(writerAddress, before) && inspect_value(valueAddress, value);
    std::uint8_t result = 0;
    __try {
        if (call != nullptr) {
            result = call(schema, valueAddress, writerAddress, mode);
        }
        const bool afterReadable =
            lease.accepting && beforeReadable && inspect_writer(writerAddress, after);
        const std::int32_t delta = after.totalBits - before.totalBits;
        if (result != 0 && afterReadable && delta > 0 && delta <= kMaximumEncodedBits) {
            Capture capture{};
            capture.value = value;
            capture.bitCount = static_cast<std::uint16_t>(delta);
            capture.present = true;

            AcquireSRWLockExclusive(&g_captureLock);
            const bool changed = g_capture.value != capture.value
                                 || g_capture.bitCount != capture.bitCount || !g_capture.present;
            g_capture = capture;
            ReleaseSRWLockExclusive(&g_captureLock);

            scheduler_output_probe::commit_signature(capture.bitCount, capture.value, changed);

            if (changed) {
                std::uint64_t first = 0;
                std::uint64_t second = 0;
                std::memcpy(&first, value.data(), sizeof first);
                std::memcpy(&second, value.data() + sizeof first, sizeof second);
                std::array<char, 192> line{};
                const int written = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=gameplay stage=scheduler-native-signature schema=0x%08X bits=%d "
                    "value=%016llX%016llX",
                    schema,
                    delta,
                    static_cast<unsigned long long>(first),
                    static_cast<unsigned long long>(second));
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

void* encoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&encode);
}

bool latest(Capture& output) noexcept {
    AcquireSRWLockShared(&g_captureLock);
    output = g_capture;
    ReleaseSRWLockShared(&g_captureLock);
    return output.present;
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_captureLock);
    g_capture = {};
    ReleaseSRWLockExclusive(&g_captureLock);
}

} // namespace sunrise::client::hooks::network::scheduler_signature_probe
