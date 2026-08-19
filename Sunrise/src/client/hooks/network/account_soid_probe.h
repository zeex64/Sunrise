#pragma once

namespace sunrise::client::hooks::network::account_soid_probe {

/** @return Passive replacement for the account-SOID prerequisite predicate. */
[[nodiscard]] void* validator_entry_point() noexcept;

/** Clears bounded observations retained across validator calls. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::account_soid_probe
