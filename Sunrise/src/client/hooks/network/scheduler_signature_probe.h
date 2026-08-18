#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::network::scheduler_signature_probe {

/** Native schema-0x80806AEA value and the exact number of bits its encoder appended. */
struct Capture {
    static constexpr std::size_t kValueSize = 16;

    std::array<std::byte, kValueSize> value{};
    std::uint16_t bitCount{};
    bool present{};
};

/** @return Native generic-schema encoder replacement body. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/** @return The newest successful scheduler-signature schema encoding. */
[[nodiscard]] bool latest(Capture& output) noexcept;

/** Clears the retained native scheduler-signature encoding. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::scheduler_signature_probe
