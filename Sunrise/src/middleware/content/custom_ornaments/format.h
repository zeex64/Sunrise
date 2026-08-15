#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::content::custom_ornaments {

/** Package entry class used to discover ornament descriptors without knowing package ids. */
inline constexpr std::uint32_t kDescriptorClass = 0x53554E4FU;
/** Class of a raw texture payload consumed by a Sunrise renderer. */
inline constexpr std::uint32_t kTextureClass = 0x53554E54U;
/** Native class carried by serialized item-definition entries. */
inline constexpr std::uint32_t kItemDefinitionClass = 0x80807BEAU;
/** Native class carried by the serialized investment item index table. */
inline constexpr std::uint32_t kItemTableClass = 0x80807BE4U;
/** Custom package ids reserved for user-authored Sunrise content. */
inline constexpr std::uint16_t kPackageMinimum = 0x0AA0U;
inline constexpr std::uint16_t kPackageMaximum = 0x0CFFU;
/** Version-one descriptor identity and fixed byte width. */
inline constexpr std::uint32_t kMagic = 0x4F4E5553U;
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint16_t kRecordSize = 96;
inline constexpr std::size_t kNameCapacity = 64;
inline constexpr std::size_t kResourceCapacity = 16;
inline constexpr std::size_t kResourceRoleCapacity = 32;
inline constexpr std::size_t kResourceRecordSize = 36;
inline constexpr std::uint16_t kNoEntry = 0xFFFFU;
inline constexpr std::uint8_t kHasTexture = 1U << 0U;
inline constexpr std::uint8_t kKnownFlags = kHasTexture;
/** Serialized item definitions carry their own hash at this fixed native field. */
inline constexpr std::size_t kDefinitionHashOffset = 0xA0;

/** One versioned package-authored ornament declaration. */
struct Resource {
    std::uint16_t entry{kNoEntry};
    std::array<char, kResourceRoleCapacity> role{};
};

struct Descriptor {
    std::uint32_t definitionHash{};
    std::uint32_t targetItemHash{};
    std::uint32_t templateItemHash{};
    std::uint16_t definitionEntry{kNoEntry};
    std::uint16_t textureEntry{kNoEntry};
    std::uint16_t socketType{};
    std::uint8_t socketLane{};
    std::uint8_t flags{};
    std::array<char, kNameCapacity> name{};
    std::uint16_t resourceCount{};
    std::array<Resource, kResourceCapacity> resources{};
};

/** @return True when the tag belongs to Sunrise's custom package-id window. */
[[nodiscard]] bool custom_tag(std::uint32_t tag) noexcept;

/**
 * Resolves an entry index against the descriptor's own package.
 * @return A tag in the same custom package, or zero for an invalid entry.
 */
[[nodiscard]] std::uint32_t entry_tag(std::uint32_t descriptorTag,
                                      std::uint16_t entryIndex) noexcept;

/** Parses and strictly validates one version-one descriptor. */
[[nodiscard]] bool parse(std::span<const std::byte> bytes, Descriptor& output) noexcept;

/** Reads the definition hash embedded in one serialized item definition. */
[[nodiscard]] bool definition_hash(std::span<const std::byte> bytes,
                                   std::uint32_t& output) noexcept;

} // namespace sunrise::middleware::content::custom_ornaments
