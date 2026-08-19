#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::hooks::network::sobject_update_probe {

/** Maximum exact native update retained for the guarded nearby-entity experiment. */
inline constexpr std::size_t kNearbyUpdateCapacity = 32;

/** One native-encoded transform captured from the accepted RSAT baseline. */
struct NearbyUpdateCapture {
    std::array<std::byte, kNearbyUpdateCapacity> wire{};
    std::uint32_t rsat{};
    std::uint16_t bitCount{};
    bool present{};
};

/** @return Kind-0 sobject update-encoder replacement body. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/**
 * Re-encodes one successfully decoded kind-0 scratch record into private buffers. This is a
 * read-only diagnostic: it never changes the decoded record. The exact nearby-player transform
 * is retained so the server can publish it only after this create's slot is observed occupied.
 */
void probe_decoded_record(std::span<const std::byte> create,
                          std::span<const std::byte> update,
                          std::span<const std::byte> mask) noexcept;

/** Moves out the exact nearby-player update captured for `rsat`; each capture is consumed once. */
[[nodiscard]] bool take_nearby_player_update(std::uint32_t rsat,
                                             NearbyUpdateCapture& output) noexcept;

/** Clears the bounded set of already-reported update inputs. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_update_probe
