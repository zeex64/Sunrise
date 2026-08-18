#pragma once

namespace sunrise::client::hooks::network::entity_slot_probe {

/** @return Native inbound entity-list decoder replacement body. */
[[nodiscard]] void* decoder_entry_point() noexcept;

/** Clears the bounded set of already-reported entity managers. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::entity_slot_probe
