#pragma once

namespace sunrise::client::hooks::network::view_creation_probe {

/** @return The native membership-driven view creator replacement body. */
[[nodiscard]] void* creator_entry_point() noexcept;

/** Restores the bounded diagnostic log allowance during shutdown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::view_creation_probe
