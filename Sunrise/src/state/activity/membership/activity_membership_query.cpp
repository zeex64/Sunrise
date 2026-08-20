#include "activity_membership_query.h"

#include <Windows.h>

#include "../../runtime/storage/internal.h"
#include "../transactions/internal.h"

namespace sunrise::state::activity::membership {

/** Tests whether the client has applied the current membership revision. */
bool acknowledged(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool applied = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& membership = state.sessions[target].membership;
        applied = membership.revision != kAbsentRevision
                  && membership.acknowledgedRevision == membership.revision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return applied;
}

/** Reads the region the client last reported it was in. */
std::int32_t reported_region(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return kAbsentRegionIndex;
    }
    std::int32_t region = kAbsentRegionIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        region = state.sessions[target].membership.region.index;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return region;
}

/** Reads the newest session the client has reported a region on. */
std::uint64_t live_region_session(std::uint64_t fallback) noexcept {
    std::uint64_t newest = kAbsentSessionId;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const SessionRecord& record : runtime::storage::g_state.activity.sessions) {
        if (record.occupied && record.sessionId > newest
            && record.membership.region.index > kAbsentRegionIndex) {
            newest = record.sessionId;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return newest == kAbsentSessionId ? fallback : newest;
}

/** Reads the identity value message 12 publishes at member record `+16`. */
std::uint64_t join_identity(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return 0;
    }
    std::uint64_t identity = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && state.sessions[target].membership.hasIdentity) {
        identity = state.sessions[target].membership.identity.joinIdentity;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return identity;
}

/** Reads the oldest published identity belonging to the signed-in account. */
bool primary_identity(Identity& identity) noexcept {
    identity = {};
    std::uint64_t oldest = kAbsentSessionId;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    for (const SessionRecord& record : root.activity.sessions) {
        if (!record.occupied || !record.joined || !record.membership.hasIdentity
            || record.membership.identity.accountSoid != root.account.primarySoid
            || record.membership.identity.opaqueSoid == 0
            || (oldest != kAbsentSessionId && record.sessionId >= oldest)) {
            continue;
        }
        identity = record.membership.identity;
        oldest = record.sessionId;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return oldest != kAbsentSessionId;
}

/** Reads one coherent destination and region from the signed-in player's original session. */
bool primary_world(WorldSnapshot& output) noexcept {
    output = {};
    output.region = kAbsentRegionIndex;
    std::uint64_t oldest = kAbsentSessionId;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    for (const SessionRecord& record : root.activity.sessions) {
        if (!record.occupied || !record.joined || !record.membership.hasIdentity
            || record.membership.identity.accountSoid != root.account.primarySoid
            || record.membership.region.index <= kAbsentRegionIndex
            || (oldest != kAbsentSessionId && record.sessionId >= oldest)) {
            continue;
        }
        output.destination = record.destination;
        output.teleport = record.membership.teleport;
        output.sessionId = record.sessionId;
        output.regionHash = record.membership.region.hash;
        output.region = record.membership.region.index;
        oldest = record.sessionId;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return oldest != kAbsentSessionId;
}

} // namespace sunrise::state::activity::membership
