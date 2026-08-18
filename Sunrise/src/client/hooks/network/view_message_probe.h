#pragma once

namespace sunrise::client::hooks::network::view_message_probe {

/** @return The native message-40 lookup replacement body. */
[[nodiscard]] void* lookup_entry_point() noexcept;

/** Clears token/result observations during network-hook shutdown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::view_message_probe
