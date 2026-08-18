#include "view_creation_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../targets/game.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::view_creation_probe {
namespace {

/** Enough entries to cover repeated public-region transitions without flooding the retail log. */
constexpr std::uint32_t kLogLimit = 32;
/** Native channel establishment must reach this state before a view may be allocated. */
constexpr std::int32_t kEstablishedChannelState = 5;
/** Channel state field in the object returned by the native accessor. */
constexpr std::size_t kChannelStateOffset = 0x1D18;
/** Lifecycle value checked by the native channel validator, relative to the accessor result. */
constexpr std::size_t kChannelLifecycleOffset = 0x3040;
/** Family bitmap consulted by the native address resolver, relative to the accessor result. */
constexpr std::size_t kChannelFamilyMaskOffset = 0x306A;
/** Address compared by the native resolver, relative to the accessor result. */
constexpr std::size_t kChannelAddressOffset = 0x309C;
/** Native network addresses compare as one fixed 0x56-byte value. */
constexpr std::size_t kNetworkAddressSize = 0x56;

std::atomic_uint32_t g_logCount{};
std::atomic_bool g_addressLogged{};

using Creator = bool(__fastcall*)(void*,
                                  std::int32_t,
                                  std::uint8_t,
                                  const void*,
                                  const void*,
                                  std::uint8_t);
using AddressResolver =
    std::int32_t(__fastcall*)(void*, std::uint8_t, const void*);
using ChannelValidator = bool(__fastcall*)(void*, std::uint8_t, std::int32_t);
using ChannelAccessor = void*(__fastcall*)(void*, std::uint8_t, std::int32_t);

struct Preconditions {
    struct ChannelSlot {
        std::int32_t lifecycle{-1};
        std::int32_t state{-1};
        std::uint16_t familyMask{};
        bool addressMatch{};
        std::array<std::uint8_t, kNetworkAddressSize> address{};
    };

    std::uint64_t session{};
    std::int32_t family{-1};
    std::int32_t channel{-1};
    std::int32_t channelLifecycle{-1};
    std::int32_t channelState{-1};
    bool channelValid{};
    std::array<std::uint8_t, kNetworkAddressSize> requestedAddress{};
    ChannelSlot slot0{};
    ChannelSlot slot1{};
};

/** Reads the native fields that distinguish an established channel from a pending duplicate. */
void inspect_slot(ChannelAccessor access,
                  void* channelManager,
                  std::uint8_t nativeFamily,
                  std::int32_t index,
                  const void* address,
                  Preconditions::ChannelSlot& output) noexcept {
    const auto* const channel =
        static_cast<const std::byte*>(access(channelManager, nativeFamily, index));
    if (channel == nullptr) {
        return;
    }
    output.lifecycle =
        *reinterpret_cast<const std::int32_t*>(channel + kChannelLifecycleOffset);
    output.state = *reinterpret_cast<const std::int32_t*>(channel + kChannelStateOffset);
    output.familyMask =
        *reinterpret_cast<const std::uint16_t*>(channel + kChannelFamilyMaskOffset);
    output.addressMatch = output.lifecycle != 0 && address != nullptr
                          && std::memcmp(channel + kChannelAddressOffset,
                                         address,
                                         kNetworkAddressSize)
                                 == 0;
    std::memcpy(output.address.data(), channel + kChannelAddressOffset, output.address.size());
}

/** Logs one complete native address once so its semantic subfields can be reconstructed. */
void log_address(const char* source,
                 const std::array<std::uint8_t, kNetworkAddressSize>& address) noexcept {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::array<char, 256> line{};
    const int prefix = std::snprintf(
        line.data(), line.size(), "ev=gameplay stage=view-address source=%s bytes=", source);
    if (prefix <= 0 || static_cast<std::size_t>(prefix) + address.size() * 2 >= line.size()) {
        return;
    }
    std::size_t cursor = static_cast<std::size_t>(prefix);
    for (const std::uint8_t value : address) {
        line[cursor++] = kHex[value >> 4U];
        line[cursor++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     {line.data(), cursor});
}

/** Replays only the creator's read-only channel queries so its rejection is observable. */
void inspect(void* context, const void* address, Preconditions& output) noexcept {
    const targets::game::network::Targets& resolved = targets::game::network::get();
    const auto resolve = reinterpret_cast<AddressResolver>(resolved.viewAddressResolver);
    const auto validate = reinterpret_cast<ChannelValidator>(resolved.viewChannelValidator);
    const auto access = reinterpret_cast<ChannelAccessor>(resolved.viewChannelAccessor);
    if (context == nullptr || address == nullptr || resolve == nullptr || validate == nullptr
        || access == nullptr) {
        return;
    }

    __try {
        const auto* const bytes = static_cast<const std::byte*>(context);
        const auto* const familyState =
            *reinterpret_cast<const std::byte* const*>(bytes + sizeof(void*));
        void* const channelManager =
            *reinterpret_cast<void* const*>(bytes + 2 * sizeof(void*));
        if (familyState == nullptr || channelManager == nullptr) {
            return;
        }
        output.family = *reinterpret_cast<const std::int32_t*>(familyState + 8);
        output.session = *reinterpret_cast<const std::uint64_t*>(familyState + 0x20);
        std::memcpy(output.requestedAddress.data(), address, output.requestedAddress.size());
        const auto nativeFamily = static_cast<std::uint8_t>(output.family + 6);
        inspect_slot(access, channelManager, nativeFamily, 0, address, output.slot0);
        inspect_slot(access, channelManager, nativeFamily, 1, address, output.slot1);
        output.channel = resolve(channelManager, nativeFamily, address);
        if (output.channel < 0) {
            return;
        }
        output.channelValid = validate(channelManager, nativeFamily, output.channel);
        const auto* const channel =
            static_cast<const std::byte*>(access(channelManager, nativeFamily, output.channel));
        if (channel != nullptr) {
            output.channelLifecycle =
                *reinterpret_cast<const std::int32_t*>(channel + kChannelLifecycleOffset);
            output.channelState =
                *reinterpret_cast<const std::int32_t*>(channel + kChannelStateOffset);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.family = -1;
        output.channel = -1;
        output.channelState = -1;
    }
}

/** Preserves creation behavior and records the exact prerequisite that rejected it. */
__declspec(noinline) bool __fastcall creator_body(void* context,
                                                   std::int32_t peer,
                                                   std::uint8_t role,
                                                   const void* address,
                                                   const void* identity,
                                                   std::uint8_t retain) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::viewCreation, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Creator>(lease.original);
    Preconditions preconditions{};
    bool result = false;
    __try {
        if (lease.accepting) {
            inspect(context, address, preconditions);
        }
        if (call != nullptr) {
            result = call(context, peer, role, address, identity, retain);
        }
        if (lease.accepting && !g_addressLogged.exchange(true, std::memory_order_relaxed)) {
            log_address("requested", preconditions.requestedAddress);
            log_address("slot0", preconditions.slot0.address);
            log_address("slot1", preconditions.slot1.address);
        }
        if (lease.accepting
            && g_logCount.fetch_add(1, std::memory_order_relaxed) < kLogLimit) {
            std::array<char, 384> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=view-create result=%s session=0x%llX family=%d peer=%d "
                "role=%u retain=%u channel=%d valid=%u lifecycle=%d state=%d established=%u "
                "s0[life=%d state=%d family=0x%04X addr=%u] "
                "s1[life=%d state=%d family=0x%04X addr=%u]",
                result ? "ok" : "fail",
                static_cast<unsigned long long>(preconditions.session),
                preconditions.family,
                peer,
                static_cast<unsigned>(role),
                static_cast<unsigned>(retain),
                preconditions.channel,
                preconditions.channelValid ? 1U : 0U,
                preconditions.channelLifecycle,
                preconditions.channelState,
                preconditions.channelState == kEstablishedChannelState ? 1U : 0U,
                preconditions.slot0.lifecycle,
                preconditions.slot0.state,
                static_cast<unsigned>(preconditions.slot0.familyMask),
                preconditions.slot0.addressMatch ? 1U : 0U,
                preconditions.slot1.lifecycle,
                preconditions.slot1.state,
                static_cast<unsigned>(preconditions.slot1.familyMask),
                preconditions.slot1.addressMatch ? 1U : 0U);
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

void* creator_entry_point() noexcept {
    return reinterpret_cast<void*>(&creator_body);
}

void reset() noexcept {
    g_logCount.store(0, std::memory_order_relaxed);
    g_addressLogged.store(false, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::network::view_creation_probe
