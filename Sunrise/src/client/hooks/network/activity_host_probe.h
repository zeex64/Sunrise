#pragma once

namespace sunrise::client::hooks::network::activity_host_probe {

/** @return Registry parameter 3's native decoder replacement body. */
[[nodiscard]] void* decoder_entry_point() noexcept;

/** @return Native activity-host connection-state publisher replacement body. */
[[nodiscard]] void* connection_state_entry_point() noexcept;

/** Clears observations during network-hook shutdown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::activity_host_probe
