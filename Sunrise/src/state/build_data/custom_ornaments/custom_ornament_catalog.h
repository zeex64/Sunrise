#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::custom_ornaments {

void clear() noexcept;
[[nodiscard]] bool ready() noexcept;
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;
[[nodiscard]] bool replace_native(std::span<const NativeDefinition> definitions,
                                  std::span<const std::byte> itemIndexTable) noexcept;
[[nodiscard]] const void* find_native(std::uint16_t definitionIndex) noexcept;
[[nodiscard]] const void* find_native_index_row(std::uint16_t definitionIndex) noexcept;
[[nodiscard]] const void* native_item_index_table() noexcept;
[[nodiscard]] std::size_t native_count() noexcept;
[[nodiscard]] bool find_hash(std::uint32_t definitionHash, Definition& output) noexcept;
[[nodiscard]] bool find_index(std::uint16_t definitionIndex, Definition& output) noexcept;
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::custom_ornaments
