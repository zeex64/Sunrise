#pragma once

#include <cstdint>

namespace sunrise::client::hooks::network::view_message_probe {

/** @return The native message-40 lookup replacement body. */
[[nodiscard]] void* lookup_entry_point() noexcept;

/** Maps an embedded entity handler back to the session token observed at message-40 lookup. */
[[nodiscard]] bool token_for_entity_handler(const void* handler, std::uint64_t& token) noexcept;

/** Clears token/result observations during network-hook shutdown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::view_message_probe
