#pragma once

namespace sunrise::client::hooks::packages {

/**
 * Installs the scoped custom-package signature-result hook.
 * @return True when the native package registrar is hooked.
 */
[[nodiscard]] bool install() noexcept;

/** Removes the custom-package signature-result hook. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::packages
