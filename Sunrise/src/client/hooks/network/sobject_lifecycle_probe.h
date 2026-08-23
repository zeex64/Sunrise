#pragma once

namespace sunrise::client::hooks::network::sobject_lifecycle_probe {

/** @return Current-native-object lookup replacement used only for nested correlation. */
[[nodiscard]] void* current_object_lookup_entry_point() noexcept;

/** @return Kind-0 inbound update-apply replacement body. */
[[nodiscard]] void* inbound_update_apply_entry_point() noexcept;

/** @return Kind-0 native lifecycle-state replacement body. */
[[nodiscard]] void* lifecycle_state_entry_point() noexcept;

/** @return Kind-0 component lifecycle-state replacement body. */
[[nodiscard]] void* component_state_entry_point() noexcept;

/** @return Current-object to replicated-entity resolver replacement used for correlation. */
[[nodiscard]] void* current_entity_resolver_entry_point() noexcept;

/** @return Current-object to definition-tag resolver replacement used for correlation. */
[[nodiscard]] void* current_definition_resolver_entry_point() noexcept;

/** @return Native object-transform replacement used to test post-factory propagation. */
[[nodiscard]] void* object_transform_entry_point() noexcept;

/** Applies one queued synthetic-object transform on the camera/player thread. */
void poll_target_transform_propagation() noexcept;

/** Clears bounded report counters after all network hooks are detached. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_lifecycle_probe
