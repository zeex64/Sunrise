#pragma once

#include <cstddef>
#include <span>

namespace sunrise::client::hooks::network::sobject_update_probe {

/** @return Kind-0 sobject update-encoder replacement body. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/**
 * Re-encodes one successfully decoded kind-0 scratch record into private buffers. This is a
 * read-only diagnostic: it never changes the decoded record or publishes the generated bits.
 */
void probe_decoded_record(std::span<const std::byte> create,
                          std::span<const std::byte> update,
                          std::span<const std::byte> mask) noexcept;

/** Clears the bounded set of already-reported update inputs. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_update_probe
