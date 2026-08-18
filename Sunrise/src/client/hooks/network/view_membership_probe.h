#pragma once

namespace sunrise::client::hooks::network::view_membership_probe {

/** @return The native membership-to-view synchronization replacement body. */
[[nodiscard]] void* sync_entry_point() noexcept;

/** @return The type-12 root decoder replacement body. */
[[nodiscard]] void* wire_entry_point() noexcept;

/** @return The decoded activity-membership queue replacement body. */
[[nodiscard]] void* decoded_entry_point() noexcept;

/** Clears the bounded set of membership snapshots recorded during this process. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::view_membership_probe
