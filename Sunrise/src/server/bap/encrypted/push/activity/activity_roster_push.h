#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../internal.h"
#include "../../activity_message/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

/** Result of offering a foreign native-current session its authored squad body. */
enum class CurrentSquadOutcome : std::uint8_t {
    /** No active request belongs to this transport; its ordinary authority body may be sent. */
    notOwned,
    /** A request belongs here but its exact route is transiently unavailable; emit no type 5. */
    held,
    /** The full authored group, Auth state, and exact grant were staged. */
    appended,
};

/**
 * Appends one `sensor_auth_update` svc9 notification carrying the destination's roster.
 * Nothing is staged before the client's own patch epoch has arrived, because a body carrying a
 * wrong epoch still lands phase 1, skips phase 2, and reports nothing.
 * @param session Connection-owned nonce, activity binding, epoch, and roster counters.
 * @param scratch Lock-owned transform and roster buffers.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @param burst True for a send on the loading cadence, which must not move the state byte.
 * @return True when the roster is built and the type-5 frame encodes atomically.
 */
[[nodiscard]] bool append_roster_notification(Session& session,
                                              Scratch& scratch,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              bool burst) noexcept;

/**
 * Appends one bubble-authority-only type-5 notification for an advertised activity host.
 * The body carries an empty roster delta, so it routes the grant to that host's native manager
 * without recursively publishing the root roster on the foreign connection.
 * @param session Foreign activity-host link and its patch epoch.
 * @param scratch Lock-owned transform buffers.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @return True when a ready host grant was encoded atomically.
 */
[[nodiscard]] bool append_authority_notification(Session& session,
                                                 Scratch& scratch,
                                                 std::span<const std::byte, state::kAesKeySize> key,
                                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                                 std::span<std::byte> response,
                                                 std::size_t& written) noexcept;

/**
 * Appends a squad-only type-5 notification to the exact foreign native-CURRENT ActivityClient.
 * It carries that link's patch epoch, one complete package-authored group, type-1 Auth, and the
 * exact current-group grant. It deliberately carries no root groups, player participation,
 * region, or spawn override. Once this session owns an active request, a held result must suppress
 * the empty authority-only type 5 so a committed group is never withdrawn transiently.
 */
[[nodiscard]] CurrentSquadOutcome
append_current_squad_notification(Session& session,
                                  Scratch& scratch,
                                  std::span<const std::byte, state::kAesKeySize> key,
                                  std::array<std::byte, state::kBapNonceSize>& nonce,
                                  std::span<std::byte> response,
                                  std::size_t& written) noexcept;

/**
 * Settles a staged roster body that reached the caller.
 * The current bubble/domain token is recorded only where the frame is known published.
 * @param session Connection-owned staged roster, cleared here.
 */
void commit_staged_roster(Session& session) noexcept;

/**
 * Puts back what a staged roster body advanced, now that the body has been discarded.
 * The next push must offer the grant again and move the per-entry state byte again.
 * @param session Connection-owned staged roster, cleared here.
 */
void discard_staged_roster(Session& session) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
