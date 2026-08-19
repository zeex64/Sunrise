#pragma once

#include <cstdint>

namespace sunrise::client::hooks::assert_handler::managed_session_pump_probe {

/** Lock-free progress for the native managed-session pump inside `network_update`. */
struct ProgressSnapshot final {
    std::uint64_t enteredSerial{};
    std::uint64_t returnedSerial{};
    std::uint32_t activeCalls{};
    std::uint32_t enteredThread{};
};

/** Attaches the diagnostic pump observer. A target miss is reported but is not fatal. */
[[nodiscard]] bool install() noexcept;

/** Detaches the diagnostic pump observer. */
void uninstall() noexcept;

/** @return Lock-free progress suitable for the watchdog assert path. */
[[nodiscard]] ProgressSnapshot progress_snapshot() noexcept;

} // namespace sunrise::client::hooks::assert_handler::managed_session_pump_probe
