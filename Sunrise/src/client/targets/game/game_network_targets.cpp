#include <array>

#include "../../patterns/game.h"
#include "network.h"
#include "network/network_content_derivation.h"
#include "relative.h"
#include "resolution/internal.h"

namespace sunrise::client::targets::game::network {
namespace {

Targets g_targets;

/** @param id Network signature identity. @return Group-local match index. */
[[nodiscard]] constexpr std::size_t index(patterns::game::Id id) noexcept {
    return static_cast<std::size_t>(id);
}

/** The token load's rip-relative displacement begins inside its lea. */
constexpr std::size_t kContentIdTokenDisplacementOffset = 14;
/** The token load's lea ends after its signed rel32. */
constexpr std::size_t kContentIdTokenInstructionEndOffset = 18;

} // namespace

/**
 * Derives a network target table without publishing it.
 * @param image Game executable ranges.
 * @param matches Unique network matches.
 * @param output Receives the derived table.
 * @return True when every address validates.
 */
bool derive(std::span<const patterns::ImageRange> image,
            std::span<const patterns::Match> matches,
            Targets& output) noexcept {
    output = {};
    if (matches.size() != resolution::kNetworkMatchCount) {
        return false;
    }

    Targets resolved;
    resolved.transportKind = matches[index(patterns::game::Id::transportKind)].address;
    resolved.httpExecuteRequest = matches[index(patterns::game::Id::httpExecuteRequest)].address;
    resolved.signOnReadinessFailure =
        matches[index(patterns::game::Id::signOnReadinessFailure)].address;
    resolved.signOnReadinessReady =
        matches[index(patterns::game::Id::signOnReadinessReady)].address;
    resolved.contentConfigFetch = matches[index(patterns::game::Id::contentConfigFetch)].address;
    resolved.contentConfigTick = matches[index(patterns::game::Id::contentConfigTick)].address;
    std::byte* const contentManifestGate =
        matches[index(patterns::game::Id::contentManifestGate)].address;
    resolved.bubbleAuthorityDecoder =
        matches[index(patterns::game::Id::bubbleAuthorityDecoder)].address;
    resolved.contentUntrackedGetter =
        matches[index(patterns::game::Id::contentUntrackedGetter)].address;
    resolved.viewSignatureRefresh =
        matches[index(patterns::game::Id::viewSignatureRefresh)].address;
    resolved.viewMessageLookup = matches[index(patterns::game::Id::viewMessageLookup)].address;
    resolved.viewSlotPump = matches[index(patterns::game::Id::viewSlotPump)].address;
    resolved.schedulerSignatureEncoder =
        matches[index(patterns::game::Id::schedulerSignatureEncoder)].address;
    resolved.sobjectCreateEncoder =
        matches[index(patterns::game::Id::sobjectCreateEncoder)].address;
    resolved.sobjectUpdateEncoder =
        matches[index(patterns::game::Id::sobjectUpdateEncoder)].address;
    resolved.sobjectNativeRegistration =
        matches[index(patterns::game::Id::sobjectNativeRegistration)].address;
    resolved.entityCreateEncoder = matches[index(patterns::game::Id::entityCreateEncoder)].address;
    resolved.entitySlotDecoder = matches[index(patterns::game::Id::entitySlotDecoder)].address;
    resolved.viewMembershipSync = matches[index(patterns::game::Id::viewMembershipSync)].address;
    resolved.activityMembershipDecoder =
        matches[index(patterns::game::Id::activityMembershipDecoder)].address;
    resolved.activityMembershipQueue =
        matches[index(patterns::game::Id::activityMembershipQueue)].address;
    resolved.viewCreator = matches[index(patterns::game::Id::viewCreator)].address;
    resolved.viewAddressResolver = matches[index(patterns::game::Id::viewAddressResolver)].address;
    resolved.viewChannelValidator =
        matches[index(patterns::game::Id::viewChannelValidator)].address;
    resolved.viewChannelAccessor = matches[index(patterns::game::Id::viewChannelAccessor)].address;
    resolved.activityHostDecoder = matches[index(patterns::game::Id::activityHostDecoder)].address;
    resolved.activityHostConnectionState =
        matches[index(patterns::game::Id::activityHostConnectionState)].address;
    resolved.activityModeSelector =
        matches[index(patterns::game::Id::activityModeSelector)].address;
    resolved.activityModeSetter = matches[index(patterns::game::Id::activityModeSetter)].address;
    resolved.activityTypeResolver =
        matches[index(patterns::game::Id::activityTypeResolver)].address;
    std::byte* const contentIdTokenLoad =
        matches[index(patterns::game::Id::contentIdTokenLoad)].address;
    if (!relative::resolve(contentIdTokenLoad,
                           kContentIdTokenDisplacementOffset,
                           kContentIdTokenInstructionEndOffset,
                           resolved.contentIdToken)
        || !content_derivation::derive(
            image, resolved.contentConfigFetch, contentManifestGate, resolved)) {
        return false;
    }
    output = resolved;
    return true;
}

/** @param targets Fully validated network table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
}

/** Resolves the early network targets and publishes only a complete group. */
bool resolve(std::span<const patterns::ImageRange> image) noexcept {
    g_targets = {};
    const auto definitions = patterns::game::definitions().first(resolution::kNetworkMatchCount);
    std::array<patterns::Match, resolution::kNetworkMatchCount> matches{};
    if (!patterns::resolve_all(image, definitions, matches)) {
        return false;
    }
    for (const patterns::Match match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }

    Targets resolved;
    if (!derive(image, matches, resolved)) {
        return false;
    }
    publish(resolved);
    return true;
}

/** Clears every published early network target. */
void clear() noexcept {
    g_targets = {};
}

/** @return Current immutable early network target group. */
const Targets& get() noexcept {
    return g_targets;
}

} // namespace sunrise::client::targets::game::network
