#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::network::view_signature {

/** Maximum signature bytes carried by native message 40. */
inline constexpr std::size_t kSignatureCapacity = 16;

/** One native view signature copied after the game refreshes it. */
struct CapturedSignature {
    std::array<std::byte, kSignatureCapacity> bytes{};
    std::uint8_t count{};
};

/** @return The view-signature refresh replacement body. */
[[nodiscard]] void* refresh_entry_point() noexcept;

/**
 * Copies the newest native signature containing one group-session id.
 * @param sessionId Group-session id embedded at byte four of the signature.
 * @param output Receives the captured bytes and count.
 * @return True when the native client has published a complete signature for that session.
 */
[[nodiscard]] bool find(std::uint64_t sessionId, CapturedSignature& output) noexcept;

/** Clears every captured signature during shutdown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::view_signature
