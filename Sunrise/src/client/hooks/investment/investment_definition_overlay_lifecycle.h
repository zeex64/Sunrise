#pragma once

namespace sunrise::client::hooks::investment {

/** Installs the package-authored item-definition registry overlay when definitions exist. */
[[nodiscard]] bool install() noexcept;

/** Removes the native item-definition lookup overlay. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::investment
