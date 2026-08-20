#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::network::scheduler_output_probe {

/** @return Shared event/mask/fixed zero-bit finalizer replacement body. */
[[nodiscard]] void* zero_finalizer_entry_point() noexcept;

/** @return Entity-lane one-bit finalizer replacement body. */
[[nodiscard]] void* entity_finalizer_entry_point() noexcept;

/**
 * Commits the same-thread lane sample that immediately preceded one native scheduler signature.
 * @param bitCount Native schema-body width, excluding the scheduler's one-bit update gate.
 * @param value Fixed 16-byte scheduler signature value encoded by the game.
 * @param signatureChanged True when the native signature differs from the prior captured value.
 */
void commit_signature(std::uint16_t bitCount,
                      const std::array<std::byte, 16>& value,
                      bool signatureChanged) noexcept;

/** Clears every bounded shape and invalidates thread-local samples during hook teardown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::scheduler_output_probe
