#pragma once

namespace sunrise::client::hooks::network::sobject_create_probe {

/** @return Kind-0 sobject create-encoder replacement body. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/** Clears the bounded set of already-reported creation inputs. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_create_probe
