#pragma once

namespace sunrise::client::hooks::network::entity_slot_probe {

/** @return Native inbound entity-list decoder replacement body. */
[[nodiscard]] void* decoder_entry_point() noexcept;

/**
 * Snapshots one entity manager reached through an already-created native view.
 * @param manager Native per-view entity manager.
 * @param result Result value associated with the observation, or zero for a view lookup.
 */
void observe_manager(const void* manager, int result = 0) noexcept;

/** Clears the bounded set of already-reported entity managers. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::entity_slot_probe
