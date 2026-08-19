#pragma once

namespace sunrise::client::hooks::network::sobject_rsat_probe {

/**
 * Queues the first experimental entity RSAT and polls its native residency predicate.
 * The create decoder supplies the two private entry points, keeping this work on a game-owned
 * networking call instead of invoking resource code from the server thread.
 */
void poll_first_entity(const void* createDecoder) noexcept;

/** @return True once the native create decoder would accept the first entity RSAT. */
[[nodiscard]] bool first_entity_ready() noexcept;

/** Clears resolved entry points and the bounded preload state. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_rsat_probe
