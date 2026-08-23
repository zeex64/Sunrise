#pragma once

#include "definition.h"

namespace sunrise::state::activity::squads {

/** Copies the normalized fixed package-authored squad catalog. */
void snapshot_catalog(CatalogSnapshot& output) noexcept;

/**
 * Copies one exact complete static group without borrowing catalog storage.
 * @param scenarioTag Scenario package object owning the group.
 * @param region Exact native region in which the group is authored.
 * @param objectTag Placed roster-group object identity.
 * @param output Cleared first, then receives the complete package slot table.
 */
[[nodiscard]] bool find_group(std::uint32_t scenarioTag,
                              std::int32_t region,
                              std::uint32_t objectTag,
                              Group& output) noexcept;

/**
 * Finds one exact source in the fixed catalog.
 * @param scenarioTag Scenario package object owning the source.
 * @param region Exact native region in which the source is authored.
 * @param objectTag Placed roster-group object containing the source.
 * @param sourceSlot Exact type-1 slot inside objectTag.
 * @param output Cleared first, then receives the matching immutable row.
 * @return True when all four authored identity fields match one row.
 */
[[nodiscard]] bool find_candidate(std::uint32_t scenarioTag,
                                  std::int32_t region,
                                  std::uint32_t objectTag,
                                  std::uint16_t sourceSlot,
                                  Candidate& output) noexcept;

/**
 * Validates and publishes one placement against an exact native-current foreign route.
 * A successful request advances both its request id and positive 31-bit spawn generation. Every
 * new placement is refused while another request is active; an uncommitted request may be cleared,
 * while a committed request remains owned until proven retirement or reset. This first control
 * therefore emits one generation per exact world lifecycle. Counters never wrap; exhaustion
 * refuses another request.
 * @param input Coherent session/world and fixed-catalog selection copied by the operator UI.
 * @param output Cleared first, then receives the published request on success.
 * @return True when the candidate, route identities/generations, and count were valid.
 */
[[nodiscard]] bool request_placement(const PlacementRequestInput& input,
                                     PlacementRequest& output) noexcept;

/**
 * Copies the active request for roster encoding and committed keepalive replay.
 * @param output Cleared first, then receives the exact current request.
 * @return False after clear/reset or before the first successful request.
 */
[[nodiscard]] bool snapshot_request(PlacementRequest& output) noexcept;

/** Clears only an uncommitted, unstaged active request, retaining its identity for diagnostics. */
void clear() noexcept;

/** Copies request lifecycle counters and the current or last request. */
void snapshot_debug(DebugSnapshot& output) noexcept;

/** Marks a matching active request as prepared for one activity message-5 send. */
[[nodiscard]] bool note_prepared(std::uint64_t requestId, std::uint64_t activitySessionId) noexcept;

/** Marks a matching active request as committed by the transport. */
[[nodiscard]] bool note_committed(std::uint64_t requestId,
                                  std::uint64_t activitySessionId) noexcept;

/** Marks a matching active request's prepared send as discarded before commit. */
[[nodiscard]] bool note_discarded(std::uint64_t requestId,
                                  std::uint64_t activitySessionId) noexcept;

/** Marks a matching active request as refused by a roster readiness or validation gate. */
[[nodiscard]] bool note_refused(std::uint64_t requestId,
                                std::uint64_t activitySessionId,
                                RefusalReason reason) noexcept;

/**
 * Retires a matching request after exact owning-activity teardown or session rebinding proves its
 * identity stale. Temporary unavailable, ambiguous, or region-transition state must not call this;
 * a committed Auth is otherwise deliberately retained for later message-5 keepalives.
 * @return True when the exact active request was atomically retired.
 */
[[nodiscard]] bool
retire(std::uint64_t requestId, std::uint64_t activitySessionId, RefusalReason reason) noexcept;

/**
 * Clears operator/debug state without reusing request ids or spawn generations.
 * This is suitable for runtime teardown; monotonic identity is preserved for stale callbacks.
 */
void reset() noexcept;

} // namespace sunrise::state::activity::squads
