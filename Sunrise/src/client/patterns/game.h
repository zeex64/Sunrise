#pragma once

#include <cstddef>
#include <span>

#include "registry.h"

namespace sunrise::client::patterns::game {

/** Stable indices for game-executable signatures. */
enum class Id : std::size_t {
    transportKind,
    httpExecuteRequest,
    signOnReadinessFailure,
    signOnReadinessReady,
    contentConfigFetch,
    contentConfigTick,
    contentManifestGate,
    bubbleAuthorityDecoder,
    contentUntrackedGetter,
    viewSignatureRefresh,
    viewMessageLookup,
    viewReadinessScan,
    viewSlotPump,
    schedulerSignatureEncoder,
    sobjectCreateEncoder,
    sobjectUpdateEncoder,
    sobjectNativeRegistration,
    sobjectBinder,
    entityCreateEncoder,
    entitySlotDecoder,
    schedulerEventDecoder,
    schedulerMaskDecoder,
    schedulerEntityPreludeDecoder,
    schedulerFixedDecoder,
    viewMembershipSync,
    activityMembershipDecoder,
    activityMembershipQueue,
    membershipUpdateEncoder,
    accountSoidValidator,
    accountSoidPublisher,
    accountSoidSource,
    accountConnectionSource,
    viewCreator,
    viewAddressResolver,
    viewChannelValidator,
    viewChannelAccessor,
    activityHostDecoder,
    activityHostConnectionState,
    activityRouteRecord,
    activityRouteLocal,
    activityRouteAuthored,
    activityModeSelector,
    activityModeSetter,
    activityTypeResolver,
    sobjectApplyJob,
    sobjectKind0Constructor,
    contentIdTokenLoad,
    queuezObjectResolver,
    queuezFamily5Subscribe,
    getItemStatValue,
    lightValueToScalar,
    retailLogEnqueue,
    retailLogSetCategoryVerbosity,
    count,
};

/** Returns the read-only game-executable signature table. */
[[nodiscard]] std::span<const patterns::Pattern> definitions() noexcept;

} // namespace sunrise::client::patterns::game
