#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../../../internal.h"
#include "../../activity_message/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Appends one current membership svc9 notification and advances its local nonce.
 * @param scratch Lock-owned transform buffers.
 * @param activity Session and prepared State snapshot picked by the svc8 route.
 * @param rootMembership Whether this is the active root membership rather than a foreign copy.
 * @param includeCitizenAdvertisement Whether the root record carries a new citizen descriptor.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @return True when the snapshot and notification encode atomically.
 */
[[nodiscard]] bool
append_membership_notification(Scratch& scratch,
                               const activity_message::ActivityPlan& activity,
                               bool rootMembership,
                               bool includeCitizenAdvertisement,
                               std::span<const std::byte, state::kAesKeySize> key,
                               std::array<std::byte, state::kBapNonceSize>& nonce,
                               std::span<std::byte> response,
                               std::size_t& written) noexcept;

/** Returns whether one gameplay group's citizen descriptor reached the client. */
[[nodiscard]] bool group_published(const Session& session, std::uint64_t groupSession) noexcept;

/** Returns whether one gameplay group's citizen descriptor was safely retired. */
[[nodiscard]] bool group_settled(const Session& session, std::uint64_t groupSession) noexcept;

/** Records the region and group named by a transaction-staged citizen descriptor. */
void stage_published_region(Session& session,
                            std::int32_t region,
                            std::uint64_t groupSession) noexcept;

/** Publishes the transaction-staged descriptor region after its frame reaches the caller. */
void commit_staged_published_region(Session& session) noexcept;

/** Drops the transaction-staged descriptor region with its discarded frame. */
void discard_staged_published_region(Session& session) noexcept;

/** Records a region and group retired by a transaction-staged membership body. */
void stage_settled_region(Session& session,
                          std::int32_t region,
                          std::uint64_t groupSession) noexcept;

/** Publishes the transaction-staged settled region after its frame reaches the caller. */
void commit_staged_settled_region(Session& session) noexcept;

/** Drops the transaction-staged settled region with its discarded frame. */
void discard_staged_settled_region(Session& session) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
