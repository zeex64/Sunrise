#include "activity_roster_push.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../../../client/world/native_current_world.h"
#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/bubble_authority/runtime.h"
#include "../../../../../state/activity/squads/activity_squad_control.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../gameplay/group/group_host_sessions.h"
#include "activity_arrival.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace message = middleware::bap::activity_message::sensor_auth_update;
namespace current_world = client::world::native_current;
namespace layouts = state::build_data::scenarios;
namespace squads = state::activity::squads;

/** No bubble was granted with this body. */
constexpr std::int32_t kNoGrant = -1;
/** The destination name a refusal reports. The selection field is 40 bytes wide. */
constexpr std::size_t kDestinationCapacity = 40;
/** Empty phase one after a grant: prefix, authority block, latch, empty delta, and two terminators.
 */
constexpr std::size_t kAuthorityOnlyBitCount =
    message::kLatchBitWithoutGrant + message::kBubbleBlockBits + 1 + message::delta_bits(0) + 2;
constexpr std::size_t kAuthorityOnlyBodySize = (kAuthorityOnlyBitCount + 7) / 8;
/** Standard 32-bit FNV-1a basis and prime fold one foreign roster topology. */
constexpr std::uint32_t kFoldBasis = 2166136261U;
constexpr std::uint32_t kFoldPrime = 16777619U;
/** Per-entry state byte is biased into one signed byte by the encoder. */
constexpr std::uint8_t kStateSequenceWrap = 128;
/** Package slot type whose Auth schema is the authored squad state. */
constexpr std::uint8_t kSlotTypeSquad = 1;

/** Copies one exact static group into encoder-owned roster scratch. */
[[nodiscard]] bool materialize_squad_group(const squads::Candidate& candidate,
                                           const squads::Group& source,
                                           layouts::RosterGroup& output) noexcept {
    output = {};
    if (source.objectTag != candidate.objectTag || source.registryKey != candidate.registryKey
        || source.slotCount == 0 || source.slotCount > source.slotTypes.size()
        || source.slotCount > output.slotTypes.size()) {
        return false;
    }
    output.objectTag = source.objectTag;
    output.registryKey = source.registryKey;
    output.slotCount = source.slotCount;
    for (std::size_t slot = 0; slot < source.slotCount; ++slot) {
        output.slotTypes[slot] = source.slotTypes[slot];
        output.slotFlags[slot] = source.slotFlags[slot];
    }
    return candidate.source.sourceSlot < output.slotCount
           && output.slotTypes[candidate.source.sourceSlot] == kSlotTypeSquad
           && (output.slotFlags[candidate.source.sourceSlot] & layouts::kSlotAuthFlag) != 0;
}

/** @return True when native CURRENT still owns every identity captured by the request. */
[[nodiscard]] bool exact_current_route(const squads::PlacementRequest& request,
                                       current_world::Route& route) noexcept {
    route = {};
    if (!current_world::resolve_route(route)
        || route.current.world.sessionId != request.activitySessionId
        || route.current.groupSessionId != request.currentGroupSessionId
        || route.current.hostSessionId != request.currentHostSessionId
        || route.current.world.region != request.region
        || route.admissionGeneration != request.currentAdmissionGeneration
        || route.authorityToken != request.authorityToken) {
        return false;
    }

    const auto& destination = route.current.world.destination;
    const std::size_t length = (std::min)(static_cast<std::size_t>(destination.packageNameLength),
                                          destination.packageName.size());
    const std::string_view name(reinterpret_cast<const char*>(destination.packageName.data()),
                                length);
    layouts::Definition layout{};
    return !name.empty() && state::build_data::find_scenario_layout(name, layout)
           && layout.tag == request.scenarioTag;
}

/** Advances the foreign link's topology state exactly once for one new complete group. */
[[nodiscard]] std::uint8_t next_current_squad_state(Session& session,
                                                    std::uint32_t registryKey) noexcept {
    const std::uint32_t folded = (kFoldBasis ^ registryKey) * kFoldPrime;
    if (session.activityRosterGroups != folded) {
        session.activityRosterState =
            static_cast<std::uint8_t>((session.activityRosterState + 1) % kStateSequenceWrap);
        session.activityRosterGroups = folded;
    }
    return session.activityRosterState;
}

/** Emits one fixed-size diagnostic for every custom foreign squad attempt. */
void report_current_squad(const char* result,
                          const Session& session,
                          const squads::PlacementRequest& request,
                          const squads::Candidate& candidate,
                          const state::activity::bubble_authority::Grant& grant,
                          std::size_t objectCount,
                          std::size_t bytes) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=current-squad result=%s transport=0x%llX root=0x%llX "
                      "group=0x%llX host=0x%llX region=%d bubble=%u token=%u key=0x%X slot=%u "
                      "gen=%u groups=1 objects=%zu body=%zu state=%u admission=%llu",
                      result,
                      static_cast<unsigned long long>(session.activitySessionId),
                      static_cast<unsigned long long>(request.activitySessionId),
                      static_cast<unsigned long long>(request.currentGroupSessionId),
                      static_cast<unsigned long long>(request.currentHostSessionId),
                      request.region,
                      static_cast<unsigned>(grant.bubble),
                      static_cast<unsigned>(grant.token),
                      candidate.registryKey,
                      static_cast<unsigned>(candidate.source.sourceSlot),
                      request.spawnGeneration,
                      objectCount,
                      bytes,
                      static_cast<unsigned>(session.activityRosterState),
                      static_cast<unsigned long long>(request.currentAdmissionGeneration));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         bytes != 0 ? core::log::Level::info : core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

} // namespace

/** Appends one `sensor_auth_update` svc9 notification carrying the destination's roster. */
bool append_roster_notification(Session& session,
                                Scratch& scratch,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool burst) noexcept {
    if (written > response.size()) {
        return false;
    }
    const std::uint32_t initialRosterGroups = session.activityRosterGroups;
    const std::uint8_t initialRosterSends = session.activityRosterSends;
    const std::uint8_t initialRosterState = session.activityRosterState;
    message::Snapshot snapshot{};
    std::array<char, kDestinationCapacity> destination{};
    std::size_t destinationLength = 0;
    RosterOutcome outcome = RosterOutcome::noEpoch;
    if (session.activityPatchEpochSeen) {
        outcome = build_roster_snapshot(
            session, scratch, snapshot, destination, destinationLength, burst);
    }
    const std::string_view name(destination.data(), destinationLength);
    if (outcome != RosterOutcome::published) {
        report_roster_push(session, snapshot, name, 0, kNoGrant, outcome);
        return false;
    }

    // The grant is picked here and committed only once the frame reaches the caller. A stage-four
    // gameplay view advances its token, so the next repeated roster is a real change for the newly
    // constructed authority manager and remains inert after that manager synchronizes.
    state::activity::bubble_authority::Grant grant{};
    // The grant follows the player, not the destination. The client names the region it is in
    // and that moves as it walks between bubbles, so granting the arrival bubble again would leave
    // the bubble the player actually entered without authority.
    // The wire field is unsigned. A value past the signed range turns negative and the selector
    // rejects it, the same answer as its own upper bound.
    std::uint64_t grantGroupSessionId = 0;
    std::uint16_t grantAuthorityToken = 0;
    if (state::activity::bubble_authority::select_grant(
            session.activitySessionId, static_cast<std::int32_t>(snapshot.region), grant)) {
        grantGroupSessionId = server::gameplay::group::advertised_group_session(
            static_cast<std::int32_t>(snapshot.region));
        grantAuthorityToken = server::gameplay::group::authority_manager_token(grantGroupSessionId);
        if (grantAuthorityToken != 0) {
            grant.token = grantAuthorityToken;
        }
        snapshot.hasGrant = true;
        snapshot.grant.bubble = grant.bubble;
        snapshot.grant.token = grant.token;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    // The root stream is now permanently base-roster-only. Refuse any accidental squad-bearing
    // snapshot at the final boundary instead of allowing the foreign route to regress into root.
    bool encoded =
        snapshot.squadRequestId == 0 && !snapshot.squadAuth.present
        && message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize)
        && append_notification_frame(scratch,
                                     session.activitySessionId,
                                     message::kMessageType,
                                     std::span(scratch.responseBody).first(messageSize),
                                     key,
                                     nonce,
                                     response,
                                     written);
    if (encoded && snapshot.squadRequestId != 0
        && !state::activity::squads::note_prepared(snapshot.squadRequestId,
                                                   session.activitySessionId)) {
        // The UI cleared or replaced the request while its body was being built. Do not publish a
        // stale generation or a group whose root publication route is no longer owned.
        encoded = false;
    }
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        // Staged, not published. The grant and the counters are one-way and this body may still be
        // discarded, so they are held here and settled by `commit_staged_roster` or
        // `discard_staged_roster`.
        session.activityRosterStaged = {grant,
                                        initialRosterGroups,
                                        initialRosterSends,
                                        initialRosterState,
                                        snapshot.hasGrant,
                                        true,
                                        snapshot.squadRequestId};
    } else if (snapshot.squadRequestId != 0) {
        (void)state::activity::squads::note_discarded(snapshot.squadRequestId,
                                                      session.activitySessionId);
    }
    report_roster_push(session,
                       snapshot,
                       name,
                       encoded ? messageSize : 0,
                       encoded && snapshot.hasGrant ? snapshot.grant.bubble : kNoGrant,
                       encoded ? RosterOutcome::published : RosterOutcome::encodeFailed);
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    if (!encoded) {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
        session.activityRosterGroups = initialRosterGroups;
        session.activityRosterSends = initialRosterSends;
        session.activityRosterState = initialRosterState;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

/** Appends one complete authored group and Squad Auth to native CURRENT's foreign session. */
CurrentSquadOutcome
append_current_squad_notification(Session& session,
                                  Scratch& scratch,
                                  std::span<const std::byte, state::kAesKeySize> key,
                                  std::array<std::byte, state::kBapNonceSize>& nonce,
                                  std::span<std::byte> response,
                                  std::size_t& written) noexcept {
    squads::DebugSnapshot debug{};
    squads::snapshot_debug(debug);
    if (!debug.requestActive || debug.request.currentHostSessionId != session.activitySessionId) {
        return CurrentSquadOutcome::notOwned;
    }

    const squads::PlacementRequest request = debug.request;
    squads::Candidate candidate{};
    state::activity::bubble_authority::Grant grant{};
    current_world::Route route{};
    const bool transportReady =
        session.activityJoinedForeignSession && session.activityPatchEpochSeen
        && !session.activityRosterStaged.staged
        && session.activitySessionId != request.activitySessionId && written <= response.size();
    if (!transportReady || !exact_current_route(request, route)) {
        report_current_squad("held-route", session, request, candidate, grant, 0, 0);
        return CurrentSquadOutcome::held;
    }

    if (!squads::find_candidate(
            request.scenarioTag,
            request.region,
            request.candidateObjectTag,
            request.candidateSourceSlot,
            candidate)) {
        (void)squads::note_refused(request.requestId,
                                   request.activitySessionId,
                                   squads::RefusalReason::candidateUnavailable);
        report_current_squad("held-candidate", session, request, candidate, grant, 0, 0);
        return CurrentSquadOutcome::held;
    }
    if (scratch.rosterGroups.empty() || message::kGroupCapacity == 0) {
        (void)squads::note_refused(
            request.requestId, request.activitySessionId, squads::RefusalReason::rosterCapacity);
        report_current_squad("held-capacity", session, request, candidate, grant, 0, 0);
        return CurrentSquadOutcome::held;
    }

    layouts::RosterGroup& group = scratch.rosterGroups.front();
    squads::Group staticGroup{};
    if (!squads::find_group(
            request.scenarioTag, request.region, request.candidateObjectTag, staticGroup)
        || !materialize_squad_group(candidate, staticGroup, group)) {
        (void)squads::note_refused(request.requestId,
                                   request.activitySessionId,
                                   squads::RefusalReason::candidateUnavailable);
        report_current_squad("held-group", session, request, candidate, grant, 0, 0);
        return CurrentSquadOutcome::held;
    }
    if (!state::activity::bubble_authority::select_grant(
            session.activitySessionId, request.region, grant)
        || grant.bubble
               != static_cast<std::uint8_t>(static_cast<std::uint32_t>(request.region) >> 3U)) {
        report_current_squad("held-grant", session, request, candidate, grant, group.slotCount, 0);
        return CurrentSquadOutcome::held;
    }
    grant.token = request.authorityToken;

    const std::uint32_t initialRosterGroups = session.activityRosterGroups;
    const std::uint8_t initialRosterSends = session.activityRosterSends;
    const std::uint8_t initialRosterState = session.activityRosterState;
    const std::size_t initialWritten = written;
    auto initialNonce = nonce;

    message::Snapshot snapshot{};
    snapshot.patchEpoch = session.activityPatchEpoch;
    snapshot.roster.groups[0].key = group.registryKey;
    snapshot.roster.groups[0].slotTypes =
        std::span<const std::uint8_t>(group.slotTypes.data(), group.slotCount);
    snapshot.roster.groups[0].slotFlags =
        std::span<const std::uint8_t>(group.slotFlags.data(), group.slotCount);
    snapshot.roster.groupCount = 1;
    snapshot.squadAuth.registryKey = candidate.registryKey;
    snapshot.squadAuth.slotIndex = candidate.source.sourceSlot;
    snapshot.squadAuth.memberSlotCount = candidate.source.memberSlotCount;
    snapshot.squadAuth.requestedCounts[0] = request.requestedCount;
    snapshot.squadAuth.generation = request.spawnGeneration;
    snapshot.squadAuth.mode = 0;
    // The live-proved 91-bit Auth body leaves the optional source-name hash absent.
    snapshot.squadAuth.hasNameHash = false;
    snapshot.squadAuth.present = true;
    snapshot.squadRequestId = request.requestId;
    snapshot.grant.bubble = grant.bubble;
    snapshot.grant.token = grant.token;
    snapshot.stateSequence = next_current_squad_state(session, candidate.registryKey);
    snapshot.lifetime = message::kLifetimeStates.front();
    snapshot.hasGrant = true;

    std::size_t messageSize = 0;
    const bool bodyEncoded =
        message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize);
    current_world::Route verifiedRoute{};
    const bool routeStillReady = bodyEncoded && exact_current_route(request, verifiedRoute);
    bool appended = routeStillReady
                    && append_notification_frame(scratch,
                                                 session.activitySessionId,
                                                 message::kMessageType,
                                                 std::span(scratch.responseBody).first(messageSize),
                                                 key,
                                                 nonce,
                                                 response,
                                                 written);
    if (appended && !squads::note_prepared(request.requestId, request.activitySessionId)) {
        appended = false;
    }
    if (appended) {
        middleware::secure_channel::advance_nonce(nonce);
        session.activityRosterStaged = {};
        session.activityRosterStaged.grant = grant;
        session.activityRosterStaged.priorGroups = initialRosterGroups;
        session.activityRosterStaged.priorSends = initialRosterSends;
        session.activityRosterStaged.priorState = initialRosterState;
        session.activityRosterStaged.hasGrant = true;
        session.activityRosterStaged.staged = true;
        session.activityRosterStaged.squadRequestId = request.requestId;
        session.activityRosterStaged.squadActivitySessionId = request.activitySessionId;
    } else {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
        session.activityRosterGroups = initialRosterGroups;
        session.activityRosterSends = initialRosterSends;
        session.activityRosterState = initialRosterState;
        if (!bodyEncoded) {
            (void)squads::note_refused(
                request.requestId, request.activitySessionId, squads::RefusalReason::invalidAuth);
        } else if (routeStillReady) {
            (void)squads::note_discarded(request.requestId, request.activitySessionId);
        }
    }
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    report_current_squad(appended          ? "ok"
                         : routeStillReady ? "discarded"
                                           : "held-route",
                         session,
                         request,
                         candidate,
                         grant,
                         group.slotCount,
                         appended ? messageSize : 0);
    return appended ? CurrentSquadOutcome::appended : CurrentSquadOutcome::held;
}

/** Appends one grant with an empty roster delta to the owning activity-host session. */
bool append_authority_notification(Session& session,
                                   Scratch& scratch,
                                   std::span<const std::byte, state::kAesKeySize> key,
                                   std::array<std::byte, state::kBapNonceSize>& nonce,
                                   std::span<std::byte> response,
                                   std::size_t& written) noexcept {
    std::int32_t region = server::gameplay::group::kUnknownRegion;
    const std::uint64_t groupSessionId =
        server::gameplay::group::holding_group_session(session.activitySessionId);
    const std::uint16_t token = server::gameplay::group::authority_manager_token(groupSessionId);
    state::activity::bubble_authority::Grant grant{};
    const bool ready =
        session.activityJoinedForeignSession && session.activityPatchEpochSeen
        && written <= response.size() && groupSessionId != 0 && token != 0
        && server::gameplay::group::holding_region_index(session.activitySessionId, region)
        && state::activity::bubble_authority::select_grant(
            session.activitySessionId, region, grant);
    if (!ready) {
        std::array<char, core::log::kLineCapacity> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=activity stage=foreign-authority result=held soid=0x%llX epoch=%u "
                          "group=0x%llX region=%d token=%u",
                          static_cast<unsigned long long>(session.activitySessionId),
                          static_cast<unsigned>(session.activityPatchEpochSeen),
                          static_cast<unsigned long long>(groupSessionId),
                          region,
                          static_cast<unsigned>(token));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return false;
    }

    grant.token = token;
    message::Snapshot snapshot{};
    snapshot.patchEpoch = session.activityPatchEpoch;
    snapshot.grant.bubble = grant.bubble;
    snapshot.grant.token = grant.token;
    snapshot.lifetime = message::kLifetimeStates.front();
    snapshot.hasGrant = true;
    snapshot.phaseOneOnly = true;

    const std::size_t initialWritten = written;
    const auto initialNonce = nonce;
    std::size_t messageSize = 0;
    const bool encoded =
        message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize)
        && messageSize == kAuthorityOnlyBodySize
        && append_notification_frame(scratch,
                                     session.activitySessionId,
                                     message::kMessageType,
                                     std::span(scratch.responseBody).first(messageSize),
                                     key,
                                     nonce,
                                     response,
                                     written);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        session.activityRosterStaged = {grant,
                                        session.activityRosterGroups,
                                        session.activityRosterSends,
                                        session.activityRosterState,
                                        true,
                                        true};
    } else {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
    }
    SecureZeroMemory(scratch.responseBody.data(), messageSize);

    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=foreign-authority result=%s soid=0x%llX group=0x%llX "
                      "region=%d bubble=%u token=%u bytes=%zu",
                      encoded ? "ok" : "fail",
                      static_cast<unsigned long long>(session.activitySessionId),
                      static_cast<unsigned long long>(groupSessionId),
                      region,
                      static_cast<unsigned>(grant.bubble),
                      static_cast<unsigned>(grant.token),
                      encoded ? messageSize : 0);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         encoded ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return encoded;
}

/** Settles a staged roster body that reached the caller. */
void commit_staged_roster(Session& session) noexcept {
    if (!session.activityRosterStaged.staged) {
        return;
    }
    if (session.activityRosterStaged.hasGrant) {
        state::activity::bubble_authority::record_grant(session.activitySessionId,
                                                        session.activityRosterStaged.grant);
    }
    if (session.activityRosterStaged.squadRequestId != 0) {
        (void)state::activity::squads::note_committed(
            session.activityRosterStaged.squadRequestId,
            session.activityRosterStaged.squadActivitySessionId);
    }
    session.activityRosterStaged = {};
}

/** Puts back what a staged roster body advanced, now that the body has been discarded. */
void discard_staged_roster(Session& session) noexcept {
    if (!session.activityRosterStaged.staged) {
        return;
    }
    // The client never saw this body, so its state byte must not be spent. The next push has to
    // move the byte again or the client does not rebuild its roster objects.
    session.activityRosterGroups = session.activityRosterStaged.priorGroups;
    session.activityRosterSends = session.activityRosterStaged.priorSends;
    session.activityRosterState = session.activityRosterStaged.priorState;
    if (session.activityRosterStaged.squadRequestId != 0) {
        (void)state::activity::squads::note_discarded(
            session.activityRosterStaged.squadRequestId,
            session.activityRosterStaged.squadActivitySessionId);
    }
    session.activityRosterStaged = {};
}

} // namespace sunrise::server::bap::encrypted::push::activity
