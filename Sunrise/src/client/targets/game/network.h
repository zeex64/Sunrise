#pragma once

#include <cstddef>
#include <span>

#include "../../patterns/registry.h"

namespace sunrise::client::targets::game::network {

/** Unowned main-image entry points and storage required by the early network guard. */
struct Targets {
    std::byte* transportKind{};
    std::byte* httpExecuteRequest{};
    std::byte* signOnReadinessFailure{};
    std::byte* signOnReadinessReady{};
    std::byte* httpEnqueueGet{};
    std::byte* contentConfigFetch{};
    std::byte* contentConfigTick{};
    std::byte* contentManifestSignatureGate{};
    std::byte* bubbleAuthorityDecoder{};
    std::byte* contentUntrackedGetter{};
    std::byte* viewSignatureRefresh{};
    std::byte* viewMessageLookup{};
    std::byte* viewSlotPump{};
    std::byte* schedulerSignatureEncoder{};
    std::byte* sobjectCreateEncoder{};
    std::byte* sobjectUpdateEncoder{};
    std::byte* sobjectNativeRegistration{};
    std::byte* entityCreateEncoder{};
    std::byte* entitySlotDecoder{};
    std::byte* viewMembershipSync{};
    std::byte* activityMembershipDecoder{};
    std::byte* activityMembershipQueue{};
    std::byte* viewCreator{};
    std::byte* viewAddressResolver{};
    std::byte* viewChannelValidator{};
    std::byte* viewChannelAccessor{};
    std::byte* activityHostDecoder{};
    std::byte* activityHostConnectionState{};
    std::byte* activityModeSelector{};
    std::byte* activityModeSetter{};
    std::byte* contentIdToken{};
};

/** Resolves and publishes the complete early network target group. */
[[nodiscard]] bool resolve(std::span<const patterns::ImageRange> image) noexcept;

/** Clears the published early network target group. */
void clear() noexcept;

/** @return Process-local early network target group. */
[[nodiscard]] const Targets& get() noexcept;

} // namespace sunrise::client::targets::game::network
