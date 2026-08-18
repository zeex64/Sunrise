#pragma once

namespace sunrise::client::hooks::network::sobject_update_probe {

/** @return Kind-0 sobject update-encoder replacement body. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/** Clears the bounded set of already-reported update inputs. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_update_probe
