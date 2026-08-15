#include "format.h"

#include <algorithm>
#include <cstring>

#include "../packages/reader/layout.h"

namespace sunrise::middleware::content::custom_ornaments {
namespace {

namespace layout = packages::reader::layout;

/** Byte offsets of the stable version-one descriptor fields. */
struct Offsets {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t version = 4;
    static constexpr std::size_t size = 6;
    static constexpr std::size_t definitionHash = 8;
    static constexpr std::size_t targetItemHash = 12;
    static constexpr std::size_t definitionEntry = 16;
    static constexpr std::size_t textureEntry = 18;
    static constexpr std::size_t socketType = 20;
    static constexpr std::size_t socketLane = 22;
    static constexpr std::size_t flags = 23;
    static constexpr std::size_t reserved = 24;
    static constexpr std::size_t name = 26;
    static constexpr std::size_t templateItemHash = 90;
    static constexpr std::size_t resourceCount = 94;
    static constexpr std::size_t resources = 96;
};

template <typename Value>
[[nodiscard]] bool read(std::span<const std::byte> bytes,
                        std::size_t offset,
                        Value& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return true;
}

/** @return Package id encoded in one valid tag, or all bits set. */
[[nodiscard]] std::uint16_t package_of(std::uint32_t tag) noexcept {
    if (tag < layout::kTagBase) {
        return kNoEntry;
    }
    return static_cast<std::uint16_t>((tag - layout::kTagBase) >> layout::kTagEntryBits);
}

} // namespace

/** @return True when the tag belongs to Sunrise's custom package-id window. */
bool custom_tag(std::uint32_t tag) noexcept {
    const std::uint16_t packageId = package_of(tag);
    return packageId >= kPackageMinimum && packageId <= kPackageMaximum;
}

/** Resolves an entry index against the descriptor's own package. */
std::uint32_t entry_tag(std::uint32_t descriptorTag, std::uint16_t entryIndex) noexcept {
    if (!custom_tag(descriptorTag) || entryIndex == kNoEntry
        || entryIndex > layout::kTagEntryMask) {
        return 0;
    }
    const std::uint32_t packageId = package_of(descriptorTag);
    return layout::kTagBase + (packageId << layout::kTagEntryBits) + entryIndex;
}

/** Parses and strictly validates one version-one descriptor. */
bool parse(std::span<const std::byte> bytes, Descriptor& output) noexcept {
    output = {};
    if (bytes.size() < kRecordSize) {
        return false;
    }
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t size = 0;
    std::uint16_t reserved = 0;
    Descriptor candidate{};
    if (!read(bytes, Offsets::magic, magic) || !read(bytes, Offsets::version, version)
        || !read(bytes, Offsets::size, size)
        || !read(bytes, Offsets::definitionHash, candidate.definitionHash)
        || !read(bytes, Offsets::targetItemHash, candidate.targetItemHash)
        || !read(bytes, Offsets::definitionEntry, candidate.definitionEntry)
        || !read(bytes, Offsets::textureEntry, candidate.textureEntry)
        || !read(bytes, Offsets::socketType, candidate.socketType)
        || !read(bytes, Offsets::socketLane, candidate.socketLane)
        || !read(bytes, Offsets::flags, candidate.flags)
        || !read(bytes, Offsets::reserved, reserved)
        || !read(bytes, Offsets::templateItemHash, candidate.templateItemHash)
        || !read(bytes, Offsets::resourceCount, candidate.resourceCount)) {
        return false;
    }
    const std::size_t expectedSize =
        kRecordSize + static_cast<std::size_t>(candidate.resourceCount) * kResourceRecordSize;
    if (candidate.resourceCount > candidate.resources.size() || bytes.size() != expectedSize) {
        return false;
    }
    std::memcpy(candidate.name.data(), bytes.data() + Offsets::name, candidate.name.size());
    const bool terminated =
        std::find(candidate.name.begin(), candidate.name.end(), '\0') != candidate.name.end();
    const bool textureConsistent =
        ((candidate.flags & kHasTexture) != 0) == (candidate.textureEntry != kNoEntry);
    if (magic != kMagic || version != kVersion || size != kRecordSize || reserved != 0
        || candidate.definitionHash == 0 || candidate.targetItemHash == 0
        || candidate.templateItemHash == 0
        || candidate.definitionEntry == kNoEntry || candidate.definitionEntry > layout::kTagEntryMask
        || (candidate.textureEntry != kNoEntry && candidate.textureEntry > layout::kTagEntryMask)
        || candidate.socketLane >= 12
        || (candidate.flags & ~kKnownFlags) != 0 || !textureConsistent || !terminated
        || candidate.name.front() == '\0') {
        return false;
    }
    for (std::size_t index = 0; index < candidate.resourceCount; ++index) {
        const std::size_t offset = Offsets::resources + index * kResourceRecordSize;
        std::uint16_t resourceReserved = 0;
        Resource& resource = candidate.resources[index];
        if (!read(bytes, offset, resource.entry)
            || !read(bytes, offset + sizeof resource.entry, resourceReserved)
            || resource.entry == kNoEntry || resource.entry > layout::kTagEntryMask
            || resourceReserved != 0) {
            return false;
        }
        std::memcpy(resource.role.data(), bytes.data() + offset + 4, resource.role.size());
        if (resource.role.front() == '\0'
            || std::find(resource.role.begin(), resource.role.end(), '\0') == resource.role.end()) {
            return false;
        }
    }
    output = candidate;
    return true;
}

/** Reads the definition hash embedded in one serialized item definition. */
bool definition_hash(std::span<const std::byte> bytes, std::uint32_t& output) noexcept {
    output = 0;
    return read(bytes, kDefinitionHashOffset, output) && output != 0;
}

} // namespace sunrise::middleware::content::custom_ornaments
