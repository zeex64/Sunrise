#include "activity_membership_push.h"

#include <Windows.h>

#include "../../../../../middleware/bap/activity_message/replicate_membership.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../gameplay/endpoint/gameplay_endpoint.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "../../../../gameplay/group/group_host.h"
#include "../../../../gameplay/group/group_host_sessions.h"
#include "activity_arrival.h"
#include "activity_notification_frame.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace message = middleware::bap::activity_message::replicate_membership;

/** The one published member always occupies slot zero of both top-level masks. */
constexpr std::uint8_t kLocalMemberSlot = 0;

/** Copies one State identity into its wire form. */
void copy_identity(
    const state::activity::membership::Identity& source,
    middleware::bap::activity_message::client_identity::ClientIdentity& target) noexcept {
    target.memberKey = source.memberKey;
    target.field1 = source.smallOpaque;
    target.field2 = source.signedOpaque;
    target.field3 = source.joinIdentity;
    target.accountSoid = source.accountSoid;
    target.field5 = source.opaqueSoid;
    target.field6 = source.secondaryOpaque;
}

/** Adds the synthetic gameplay host as the second activity-member record. */
void reflect_host(message::MembershipSnapshot& wire, std::uint64_t hostKey) noexcept {
    const state::gameplay::Endpoint host = server::gameplay::endpoint::advertised();
    if (hostKey == 0 || hostKey == wire.identity.memberKey || host.address == 0 || host.port == 0) {
        return;
    }
    wire.reflectedHost.memberKey = hostKey;
    wire.reflectedHost.field1 = -1;
    wire.reflectedHost.field2 = -1;
    wire.reflectedHost.field3 = hostKey;
    middleware::gameplay::descriptor::write_net_addr(
        host.address, host.port, wire.reflectedHostAddress);
    wire.hasReflectedHost = true;
}

/**
 * Maps a lock-consistent State snapshot into the fixed Middleware schema.
 * @param sessionId Joined activity session, used to resolve the advertised region.
 * @param mutation Prepared membership operation, whose region this body publishes.
 * @param rootMembership Whether this is the active root membership rather than a foreign copy.
 * @param includeCitizenAdvertisement Whether the root record carries a new citizen descriptor.
 * @return Whole current membership encoder input.
 */
[[nodiscard]] message::MembershipSnapshot
make_wire_snapshot(std::uint64_t sessionId,
                   const state::activity::membership::PendingMutation& mutation,
                   bool rootMembership,
                   bool includeCitizenAdvertisement) noexcept {
    const state::activity::membership::Snapshot& snapshot = mutation.snapshot;
    message::MembershipSnapshot wire{};
    copy_identity(snapshot.identity, wire.identity);
    wire.spawn.state = snapshot.spawn.state;
    wire.spawn.opaqueByte = snapshot.spawn.opaqueByte;
    wire.spawn.opaqueValue = snapshot.spawn.opaqueValue;
    wire.teleport.state = snapshot.teleport.state;
    wire.teleport.token = snapshot.teleport.token;
    wire.teleport.sliceSetIndex = snapshot.teleport.sliceSetIndex;
    wire.teleport.sliceSetHash = snapshot.teleport.sliceSetHash;
    wire.revision = snapshot.revision;
    wire.epoch = snapshot.epoch;
    wire.transitionToken = snapshot.transitionToken;
    // A foreign activity still receives its own two-member copy for normal membership handling.
    // View creation itself is driven from the active root membership below, not this foreign copy.
    if (!rootMembership) {
        state::activity::membership::Identity local{};
        const std::uint64_t hostKey = server::gameplay::group::holding_group_session(sessionId);
        if (hostKey != 0 && state::activity::membership::primary_identity(local)) {
            copy_identity(local, wire.identity);
            reflect_host(wire, hostKey);
        }
        SecureZeroMemory(&local, sizeof local);
    }
    // The region this body is about to commit, not the one State still holds. Staging runs before
    // the commit, so the region just left would leave the pending record empty for good.
    if (rootMembership) {
        const EffectiveRegion region = planned_region(mutation, sessionId);
        if (includeCitizenAdvertisement) {
            server::gameplay::build_advertisement(region.index,
                                                  region.reported
                                                      ? server::gameplay::RegionSource::reported
                                                      : server::gameplay::RegionSource::arrival,
                                                  kLocalMemberSlot,
                                                  wire.citizen);
        }
        // FUN_141702580 synchronizes the single active root membership. Mirror the gameplay group
        // named by this region into every root refresh so a descriptor need not be replayed merely
        // to keep its non-local slot current.
        const std::uint64_t hostKey =
            server::gameplay::group::advertised_group_session(region.index);
        if (server::gameplay::group::session_admitted(hostKey)) {
            reflect_host(wire, hostKey);
        }
    }
    return wire;
}

} // namespace

/** Appends one current membership svc9 notification and advances its local nonce. */
bool append_membership_notification(Scratch& scratch,
                                    const activity_message::ActivityPlan& activity,
                                    bool rootMembership,
                                    bool includeCitizenAdvertisement,
                                    std::span<const std::byte, state::kAesKeySize> key,
                                    std::array<std::byte, state::kBapNonceSize>& nonce,
                                    std::span<std::byte> response,
                                    std::size_t& written) noexcept {
    if (written > response.size() || !activity.membershipMutation.hasSnapshot) {
        return false;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    const message::MembershipSnapshot snapshot = make_wire_snapshot(activity.sessionId,
                                                                    activity.membershipMutation,
                                                                    rootMembership,
                                                                    includeCitizenAdvertisement);
    const bool encoded =
        message::encode_replicate_membership(snapshot, scratch.responseBody, messageSize)
        && append_notification_frame(scratch,
                                     activity.sessionId,
                                     message::kMessageType,
                                     std::span(scratch.responseBody).first(messageSize),
                                     key,
                                     nonce,
                                     response,
                                     written);
    SecureZeroMemory(scratch.responseBody.data(), message::encoded_size(snapshot));
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
    } else {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

/** Records the region named by a transaction-staged citizen descriptor. */
void stage_advertised_region(Session& session, std::int32_t region) noexcept {
    if (region < 0) {
        return;
    }
    session.activityAdvertisedRegionStaged = region;
    session.activityAdvertisedRegionStagedPresent = true;
}

/** Publishes the transaction-staged advertised region after its frame reaches the caller. */
void commit_staged_advertised_region(Session& session) noexcept {
    if (!session.activityAdvertisedRegionStagedPresent) {
        return;
    }
    session.activityAdvertisedRegion = session.activityAdvertisedRegionStaged;
    session.activityAdvertisedRegionStaged = 0;
    session.activityAdvertisedRegionStagedPresent = false;
}

/** Drops the transaction-staged advertised region with its discarded frame. */
void discard_staged_advertised_region(Session& session) noexcept {
    session.activityAdvertisedRegionStaged = 0;
    session.activityAdvertisedRegionStagedPresent = false;
}

} // namespace sunrise::server::bap::encrypted::push::activity
