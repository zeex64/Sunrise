#include <Windows.h>

#include "../../../runtime/storage/internal.h"
#include "../../transactions/internal.h"
#include "../runtime.h"

namespace sunrise::state::activity::bubble_authority {

/** Picks the bubble token to repeat for this session's current native manager. */
bool select_grant(std::uint64_t sessionId, std::int32_t sliceSetIndex, Grant& grant) noexcept {
    grant = {};
    if (sessionId == kAbsentSessionId || sliceSetIndex < 0
        || sliceSetIndex > kMaximumGrantSliceSetIndex) {
        return false;
    }
    const auto bubble = static_cast<std::uint8_t>(sliceSetIndex >> kSliceSetToBubbleShift);
    bool selected = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && bubble < kFallbackBubble) {
        const std::uint16_t recorded = state.sessions[target].bubbleAuthority.grantTokens[bubble];
        grant.bubble = bubble;
        grant.token = recorded == 0 ? kInitialGrantToken : recorded;
        selected = true;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return selected;
}

/** Records the token last sent for the usable bubble and its paired domain slot. */
void record_grant(std::uint64_t sessionId, const Grant& grant) noexcept {
    if (sessionId == kAbsentSessionId || grant.bubble >= kAuthoritySlotCount || grant.token == 0) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].bubbleAuthority.grantTokens[grant.bubble] = grant.token;
        if (grant.bubble < kFallbackBubble) {
            state.sessions[target].bubbleAuthority.grantTokens[kFallbackBubble] = grant.token;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Drops every grant recorded for one session, so the next roster push grants again. */
void clear_grants(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].bubbleAuthority = {};
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

} // namespace sunrise::state::activity::bubble_authority
