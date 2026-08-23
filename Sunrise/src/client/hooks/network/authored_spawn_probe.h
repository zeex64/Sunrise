#pragma once

namespace sunrise::client::hooks::network::authored_spawn_probe {

/** @return Activity-authored object materializer replacement body. */
[[nodiscard]] void* materialize_entry_point() noexcept;

/** @return Activity-authored spawn state-machine replacement body. */
[[nodiscard]] void* state_entry_point() noexcept;

/** @return Activity-authored spawn-controller initializer replacement body. */
[[nodiscard]] void* initialize_entry_point() noexcept;

/** @return Activity-authored squad-descriptor queue builder replacement body. */
[[nodiscard]] void* queue_builder_entry_point() noexcept;

/** @return Activity-authored spawn-controller reset replacement body. */
[[nodiscard]] void* reset_entry_point() noexcept;

/** @return Generic authored-controller callback resolver replacement body. */
[[nodiscard]] void* callback_resolver_entry_point() noexcept;

/** @return Resource callback-link replacement body used to correlate the owning tag. */
[[nodiscard]] void* callback_link_entry_point() noexcept;

/** @return Decoded callback-table linker replacement body. */
[[nodiscard]] void* callback_table_link_entry_point() noexcept;

/** @return High-level client squad spawn-at-transform replacement body. */
[[nodiscard]] void* squad_transform_entry_point() noexcept;

/** @return High-level client squad spawn-at-object replacement body. */
[[nodiscard]] void* squad_object_entry_point() noexcept;

/** @return Native current-bubble authority predicate replacement body. */
[[nodiscard]] void* current_bubble_authority_entry_point() noexcept;

/** @return Native domain-authority predicate replacement body. */
[[nodiscard]] void* domain_authority_entry_point() noexcept;

/** @return Native granted/pending authority-bit tester replacement body. */
[[nodiscard]] void* authority_state_test_entry_point() noexcept;

/** @return Native per-lane bubble-authority apply replacement body. */
[[nodiscard]] void* bubble_authority_apply_entry_point() noexcept;

/** Samples both native authority predicates after a successful roster-authority decode. */
void sample_authority_after_roster() noexcept;

/**
 * Reports whether the most recent roster decode observed both native authority predicates ready.
 * The answer expires quickly so a previous activity cannot authorize a new authored squad.
 */
[[nodiscard]] bool roster_authority_ready() noexcept;

/** Tries the known EDZ authored spawn rule once after genuine local physics authority is live. */
void poll_direct_squad_spawn() noexcept;

/** Clears the bounded set of descriptor shapes already reported. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::authored_spawn_probe
