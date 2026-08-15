#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

#include "../../../../middleware/content/custom_ornaments/format.h"
#include "../../../../state/build_data/custom_ornaments/custom_ornament_catalog.h"
#include "../../../../state/build_data/items/item_catalog.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace format = middleware::content::custom_ornaments;
namespace ornament_state = state::build_data::custom_ornaments;

struct DescriptorTags {
    std::array<std::uint32_t, ornament_state::kDefinitionCapacity> values{};
    std::size_t count{};
};

struct DefinitionTags {
    std::array<std::uint32_t, ornament_state::kDefinitionCapacity * 2> values{};
    std::size_t count{};
};

struct ItemTableTags {
    std::array<std::uint32_t, ornament_state::kDefinitionCapacity> values{};
    std::size_t count{};
};

[[nodiscard]] bool collect_descriptor(void* context, std::uint32_t tag) noexcept {
    auto& tags = *static_cast<DescriptorTags*>(context);
    if (!format::custom_tag(tag)) {
        return true;
    }
    if (tags.count >= tags.values.size()) {
        return false;
    }
    tags.values[tags.count++] = tag;
    return true;
}

[[nodiscard]] bool collect_definition(void* context, std::uint32_t tag) noexcept {
    auto& tags = *static_cast<DefinitionTags*>(context);
    if (!format::custom_tag(tag)) {
        return true;
    }
    if (tags.count >= tags.values.size()) {
        return false;
    }
    tags.values[tags.count++] = tag;
    return true;
}

[[nodiscard]] bool collect_item_table(void* context, std::uint32_t tag) noexcept {
    auto& tags = *static_cast<ItemTableTags*>(context);
    if (!format::custom_tag(tag)) {
        return true;
    }
    if (tags.count >= tags.values.size()) {
        return false;
    }
    tags.values[tags.count++] = tag;
    return true;
}

[[nodiscard]] bool find_index(std::span<const state::build_data::items::Definition> rows,
                              std::uint32_t hash,
                              std::uint16_t& index) noexcept {
    const auto match = std::find_if(rows.begin(), rows.end(), [hash](const auto& row) {
        return row.definitionHash == hash;
    });
    if (match == rows.end()) {
        return false;
    }
    index = match->definitionIndex;
    return true;
}

[[nodiscard]] bool cached_index(std::uint32_t hash, std::uint16_t& index) noexcept {
    state::build_data::items::Definition definition{};
    if (!state::build_data::items::find_hash(hash, definition)) {
        return false;
    }
    index = definition.definitionIndex;
    return true;
}

} // namespace

/** Discovers and validates every package-authored ornament descriptor. */
bool build_custom_ornaments(const reader::Source& source,
                            Storage& storage,
                            bool appendDefinitions,
                            std::size_t& rowCount,
                            std::size_t& ornamentCount) noexcept {
    ornamentCount = 0;
    storage.nativeDefinitionCount = 0;
    storage.nativeItemIndexTable.clear();
    DescriptorTags tags{};
    reader::ScanResult scan{};
    if (!reader::scan_class(
            source.directory, format::kDescriptorClass, &collect_descriptor, &tags, scan)) {
        return false;
    }
    auto end = tags.values.begin() + static_cast<std::ptrdiff_t>(tags.count);
    std::sort(tags.values.begin(), end);
    end = std::unique(tags.values.begin(), end);
    tags.count = static_cast<std::size_t>(end - tags.values.begin());

    for (std::size_t slot = 0; slot < tags.count; ++slot) {
        const std::uint32_t descriptorTag = tags.values[slot];
        std::uint32_t descriptorClass = 0;
        if (!reader::read_tag(
                source, storage.scratch, descriptorTag, storage.definition, descriptorClass)
            || descriptorClass != format::kDescriptorClass) {
            return false;
        }
        format::Descriptor descriptor{};
        if (!format::parse(std::span<const std::byte>{storage.definition}, descriptor)) {
            return false;
        }
        const std::uint32_t definitionTag =
            format::entry_tag(descriptorTag, descriptor.definitionEntry);
        const std::uint32_t textureTag =
            format::entry_tag(descriptorTag, descriptor.textureEntry);
        std::uint32_t definitionClass = 0;
        if (definitionTag == 0
            || !reader::read_tag(
                source, storage.scratch, definitionTag, storage.definition, definitionClass)
            || definitionClass != format::kItemDefinitionClass) {
            return false;
        }
        std::uint32_t embeddedHash = 0;
        if (!format::definition_hash(std::span<const std::byte>{storage.definition}, embeddedHash)
            || embeddedHash != descriptor.definitionHash) {
            return false;
        }

        std::uint16_t definitionIndex = 0;
        if (appendDefinitions) {
            const auto installedRows = std::span(storage.rows).first(rowCount);
            std::uint16_t unused = 0;
            const bool installed =
                find_index(installedRows, descriptor.definitionHash, definitionIndex);
            if (!find_index(installedRows, descriptor.targetItemHash, unused)
                || !find_index(installedRows, descriptor.templateItemHash, unused)
                || (!installed
                    && (rowCount >= storage.rows.size()
                        || rowCount > (std::numeric_limits<std::uint16_t>::max)()))) {
                return false;
            }
            if (!installed) {
                definitionIndex = static_cast<std::uint16_t>(rowCount);
            }
        } else {
            std::uint16_t unused = 0;
            if (!cached_index(descriptor.definitionHash, definitionIndex)
                || !cached_index(descriptor.targetItemHash, unused)
                || !cached_index(descriptor.templateItemHash, unused)) {
                return false;
            }
        }

        tables::items::Row item{};
        item.definitionHash = descriptor.definitionHash;
        item.definitionIndex = definitionIndex;
        if (!tables::items::read_definition(std::span<const std::byte>{storage.definition}, item)) {
            return false;
        }
        if (appendDefinitions
            && definitionIndex == static_cast<std::uint16_t>(rowCount)) {
            storage.rows[rowCount++] = state::build_data::items::Definition{
                item.definitionHash, item.definitionIndex, item.bucketId};
        }
        storage.customItemRows[ornamentCount] = item;
        auto& custom = storage.customOrnaments[ornamentCount];
        custom = {};
        custom.definitionHash = descriptor.definitionHash;
        custom.targetItemHash = descriptor.targetItemHash;
        custom.templateItemHash = descriptor.templateItemHash;
        custom.descriptorTag = descriptorTag;
        custom.definitionTag = definitionTag;
        custom.textureTag = textureTag;
        custom.definitionIndex = definitionIndex;
        custom.socketType = descriptor.socketType;
        custom.socketLane = descriptor.socketLane;
        custom.flags = descriptor.flags;
        std::copy(descriptor.name.begin(), descriptor.name.end(), custom.name.begin());
        custom.resourceCount = static_cast<std::uint8_t>(descriptor.resourceCount);
        for (std::size_t resource = 0; resource < descriptor.resourceCount; ++resource) {
            custom.resources[resource].tag =
                format::entry_tag(descriptorTag, descriptor.resources[resource].entry);
            if (custom.resources[resource].tag == 0) {
                return false;
            }
            std::copy(descriptor.resources[resource].role.begin(),
                      descriptor.resources[resource].role.end(),
                      custom.resources[resource].role.begin());
        }
        ++ornamentCount;
    }

    // The native registry overlay is also package-authored. Collect both each new plug definition
    // and each expanded target definition without embedding any ornament hash in the DLL.
    DefinitionTags definitionTags{};
    scan = {};
    if (!reader::scan_class(source.directory,
                            format::kItemDefinitionClass,
                            &collect_definition,
                            &definitionTags,
                            scan)) {
        return false;
    }
    const auto rows = std::span(storage.rows).first(rowCount);
    for (std::size_t slot = 0; slot < definitionTags.count; ++slot) {
        std::uint32_t classId = 0;
        if (!reader::read_tag(source,
                              storage.scratch,
                              definitionTags.values[slot],
                              storage.definition,
                              classId)
            || classId != format::kItemDefinitionClass) {
            return false;
        }
        std::uint32_t hash = 0;
        if (!format::definition_hash(std::span<const std::byte>{storage.definition}, hash)) {
            return false;
        }
        const bool owned = std::any_of(
            storage.customOrnaments.begin(),
            storage.customOrnaments.begin() + static_cast<std::ptrdiff_t>(ornamentCount),
            [hash](const auto& ornament) {
                return ornament.definitionHash == hash || ornament.targetItemHash == hash;
            });
        if (!owned) {
            continue;
        }
        std::uint16_t index = 0;
        if (!(appendDefinitions ? find_index(rows, hash, index) : cached_index(hash, index))) {
            return false;
        }
        bool duplicate = false;
        for (std::size_t prior = 0; prior < storage.nativeDefinitionCount; ++prior) {
            duplicate = duplicate || storage.nativeDefinitions[prior].definitionIndex == index;
        }
        if (duplicate) {
            continue;
        }
        if (storage.nativeDefinitionCount >= storage.nativeDefinitions.size()) {
            return false;
        }
        auto& native = storage.nativeDefinitions[storage.nativeDefinitionCount++];
        native.definitionHash = hash;
        native.definitionIndex = index;
        native.bytes = storage.definition;
    }

    // Preserve the package-authored 24-byte index rows as well as their definition blobs. The
    // native registry asks for this row before resolving a definition, so both halves must come
    // from the same custom investment table. No package id, hash, or index is embedded here.
    ItemTableTags itemTableTags{};
    scan = {};
    if (!reader::scan_class(source.directory,
                            format::kItemTableClass,
                            &collect_item_table,
                            &itemTableTags,
                            scan)) {
        return false;
    }
    for (std::size_t tableSlot = 0; tableSlot < itemTableTags.count; ++tableSlot) {
        std::uint32_t classId = 0;
        if (!reader::read_tag(source,
                              storage.scratch,
                              itemTableTags.values[tableSlot],
                              storage.child,
                              classId)
            || classId != format::kItemTableClass) {
            return false;
        }
        tables::Array array{};
        const std::span<const std::byte> table{storage.child};
        if (!tables::find_array_at(table, tables::kTableArrayDescriptor, array)
            || array.elementClass != tables::kItemIndexTableClass) {
            return false;
        }
        bool ownsEveryRow = true;
        for (std::size_t slot = 0; slot < storage.nativeDefinitionCount; ++slot) {
            const auto& native = storage.nativeDefinitions[slot];
            if (native.definitionIndex >= array.count) {
                ownsEveryRow = false;
                break;
            }
            const std::size_t offset = array.dataOffset
                                       + static_cast<std::size_t>(native.definitionIndex)
                                             * ornament_state::kNativeIndexRowSize;
            if (offset > table.size()
                || table.size() - offset < ornament_state::kNativeIndexRowSize) {
                return false;
            }
            std::uint32_t rowHash = 0;
            std::memcpy(&rowHash, table.data() + offset, sizeof rowHash);
            if (rowHash != native.definitionHash) {
                ownsEveryRow = false;
                break;
            }
        }
        if (!ownsEveryRow) {
            continue;
        }
        storage.nativeItemIndexTable.assign(table.begin(), table.end());
        for (std::size_t slot = 0; slot < storage.nativeDefinitionCount; ++slot) {
            auto& native = storage.nativeDefinitions[slot];
            const std::size_t offset = array.dataOffset
                                       + static_cast<std::size_t>(native.definitionIndex)
                                             * ornament_state::kNativeIndexRowSize;
            std::memcpy(native.indexRow.data(), table.data() + offset, native.indexRow.size());
        }
        break;
    }
    if (storage.nativeItemIndexTable.empty()) {
        return false;
    }
    for (std::size_t slot = 0; slot < storage.nativeDefinitionCount; ++slot) {
        std::uint32_t rowHash = 0;
        std::memcpy(&rowHash,
                    storage.nativeDefinitions[slot].indexRow.data(),
                    sizeof rowHash);
        if (rowHash != storage.nativeDefinitions[slot].definitionHash) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
