#pragma once

namespace sunrise::client::hooks::network::membership_update_probe {

/** @return Replacement body for message 30's native membership-update encoder. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/** Clears the bounded native-profile observations. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::membership_update_probe
