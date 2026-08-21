#pragma once

namespace sunrise::client::hooks::network::scheduler_entity_collector_probe {

/** @return Replicated-entity collector-dispatch gate replacement body. */
[[nodiscard]] void* gate_entry_point() noexcept;

/** @return Replicated-entity scheduler candidate collector replacement body. */
[[nodiscard]] void* collector_entry_point() noexcept;

/** Clears the bounded collector-shape registry during hook teardown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::scheduler_entity_collector_probe
