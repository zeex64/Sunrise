#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sunrise::state::build_data::custom_ornaments {

inline constexpr std::size_t kDefinitionCapacity = 256;
inline constexpr std::size_t kNameCapacity = 64;
inline constexpr std::size_t kResourceCapacity = 16;
inline constexpr std::size_t kResourceRoleCapacity = 32;
inline constexpr std::size_t kNativeIndexRowSize = 24;

struct Resource {
    std::uint32_t tag{};
    std::array<char, kResourceRoleCapacity> role{};
};

/** Runtime routing metadata authored entirely by a custom package descriptor. */
struct Definition {
    std::uint32_t definitionHash{};
    std::uint32_t targetItemHash{};
    std::uint32_t templateItemHash{};
    std::uint32_t descriptorTag{};
    std::uint32_t definitionTag{};
    std::uint32_t textureTag{};
    std::uint16_t definitionIndex{};
    std::uint16_t socketType{};
    std::uint8_t socketLane{};
    std::uint8_t flags{};
    std::array<char, kNameCapacity> name{};
    std::uint8_t resourceCount{};
    std::array<Resource, kResourceCapacity> resources{};
};

/** One package-loaded item definition exposed through the native registry overlay. */
struct NativeDefinition {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::array<std::byte, kNativeIndexRowSize> indexRow{};
    std::vector<std::byte> bytes{};
};

} // namespace sunrise::state::build_data::custom_ornaments
