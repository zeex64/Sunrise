#include "membership_update_probe.h"

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

namespace sunrise::client::hooks::network::membership_update_probe {
namespace {

/** FUN_14173E050's fixed membership-update image layout. */
constexpr std::size_t kPlayerCountOffset = 0x174A;
constexpr std::size_t kPlayerDeltaOffset = 0x4250;
constexpr std::size_t kPlayerDeltaStride = 0x1B8;
constexpr std::size_t kPlayerCapacity = 32;
constexpr std::size_t kIdentityPresentOffset = 8;
constexpr std::size_t kProfilePresentOffset = 0x21;

std::atomic_bool g_reportedObservation{};
std::atomic_bool g_reportedProfile{};

using Encoder = void(__fastcall*)(void*, void*, const void*);

struct ProfileCapture {
    std::int16_t count{};
    std::uint32_t index{};
    std::uint32_t mode{};
    std::uint8_t identityPresent{};
    std::uint8_t profilePresent{};
    std::array<std::byte, kPlayerDeltaStride> bytes{};
    bool readable{};
    bool profileFound{};
};

/** Converts the complete player delta to an uppercase byte-exact exemplar. */
void hex(std::span<const std::byte> input, std::span<char> output) noexcept {
    constexpr char kDigits[] = "0123456789ABCDEF";
    if (output.empty()) {
        return;
    }
    const std::size_t count =
        input.size() < (output.size() - 1) / 2 ? input.size() : (output.size() - 1) / 2;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(input[index]);
        output[index * 2] = kDigits[value >> 4];
        output[index * 2 + 1] = kDigits[value & 0x0F];
    }
    output[count * 2] = '\0';
}

/** Copies one profile-bearing delta before the native encoder can release its input. */
[[nodiscard]] bool inspect(const void* updateAddress, ProfileCapture& output) noexcept {
    if (updateAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const update = static_cast<const std::byte*>(updateAddress);
        std::memcpy(&output.count, update + kPlayerCountOffset, sizeof output.count);
        if (output.count < 0 || output.count > static_cast<std::int16_t>(kPlayerCapacity)) {
            return false;
        }
        output.readable = true;
        for (std::int16_t ordinal = 0; ordinal < output.count; ++ordinal) {
            const auto* const delta = update + kPlayerDeltaOffset
                                      + static_cast<std::size_t>(ordinal) * kPlayerDeltaStride;
            std::uint32_t mode = 0;
            std::uint8_t profilePresent = 0;
            std::memcpy(&mode, delta + 4, sizeof mode);
            std::memcpy(&profilePresent, delta + kProfilePresentOffset, sizeof profilePresent);
            if (mode != 1 || profilePresent == 0) {
                continue;
            }
            std::memcpy(&output.index, delta, sizeof output.index);
            output.mode = mode;
            std::memcpy(&output.identityPresent,
                        delta + kIdentityPresentOffset,
                        sizeof output.identityPresent);
            output.profilePresent = profilePresent;
            std::memcpy(output.bytes.data(), delta, output.bytes.size());
            output.profileFound = true;
            break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Reports the first encoder call and the first native profile-bearing player delta. */
void report(const ProfileCapture& capture) noexcept {
    if (!g_reportedObservation.exchange(true, std::memory_order_relaxed)) {
        std::array<char, 160> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=gameplay stage=membership-native-profile result=observe readable=%u count=%d",
            capture.readable ? 1U : 0U,
            static_cast<int>(capture.count));
        if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (!capture.profileFound || g_reportedProfile.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    std::array<char, kPlayerDeltaStride * 2 + 1> deltaHex{};
    hex(capture.bytes, deltaHex);
    std::array<char, 1152> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=membership-native-profile result=capture count=%d index=%u mode=%u "
        "identity=%u profile=%u delta=%s",
        static_cast<int>(capture.count),
        capture.index,
        capture.mode,
        static_cast<unsigned>(capture.identityPresent),
        static_cast<unsigned>(capture.profilePresent),
        deltaHex.data());
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Preserves message-30 encoding while observing game-authored local membership profiles. */
__declspec(noinline) void __fastcall
encode_body(void* writer, void* context, const void* update) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::membershipUpdateEncoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Encoder>(lease.original);
    ProfileCapture capture{};
    const bool readable = inspect(update, capture);
    __try {
        if (call != nullptr) {
            call(writer, context, update);
        }
        if (lease.accepting) {
            if (!readable) {
                capture = {};
            }
            report(capture);
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
    g_reportedObservation.store(false, std::memory_order_relaxed);
    g_reportedProfile.store(false, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::network::membership_update_probe
