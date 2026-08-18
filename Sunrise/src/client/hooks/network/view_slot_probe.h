#pragma once

namespace sunrise::client::hooks::network::view_slot_probe {

/** @return Replacement body for the native three-slot replication-manager pump. */
[[nodiscard]] void* pump_entry_point() noexcept;

/** Clears the distinct slot snapshots retained by the diagnostic. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::view_slot_probe
