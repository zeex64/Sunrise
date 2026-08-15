#include "custom_ornament_catalog.h"

#include <array>
#include <cstring>
#include <vector>

#include "../table.h"

namespace sunrise::state::build_data::custom_ornaments {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
bool g_ready{};
std::vector<NativeDefinition> g_nativeDefinitions;
std::vector<std::byte> g_nativeItemIndexTable;

[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const Definition& candidate = definitions[index];
        if (candidate.definitionHash == 0 || candidate.targetItemHash == 0
            || candidate.definitionTag == 0 || candidate.descriptorTag == 0
            || candidate.name.front() == '\0' || candidate.resourceCount > candidate.resources.size()) {
            return false;
        }
        for (std::size_t resource = 0; resource < candidate.resourceCount; ++resource) {
            if (candidate.resources[resource].tag == 0
                || candidate.resources[resource].role.front() == '\0') {
                return false;
            }
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (definitions[prior].definitionHash == candidate.definitionHash
                || definitions[prior].definitionIndex == candidate.definitionIndex) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
    g_ready = false;
    g_nativeDefinitions.clear();
    g_nativeItemIndexTable.clear();
}

bool ready() noexcept {
    const Lock::Shared guard(g_lock);
    return g_ready;
}

bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    const bool replaced = g_definitions.replace(definitions);
    g_ready = replaced;
    return replaced;
}

bool replace_native(std::span<const NativeDefinition> definitions,
                    std::span<const std::byte> itemIndexTable) noexcept {
    if (itemIndexTable.size() <= 8) {
        return false;
    }
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        std::uint32_t rowHash = 0;
        std::memcpy(&rowHash, definitions[index].indexRow.data(), sizeof rowHash);
        if (definitions[index].definitionHash == 0
            || rowHash != definitions[index].definitionHash
            || definitions[index].bytes.empty()) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (definitions[prior].definitionIndex == definitions[index].definitionIndex) {
                return false;
            }
        }
    }
    const Lock::Exclusive guard(g_lock);
    g_nativeDefinitions.assign(definitions.begin(), definitions.end());
    g_nativeItemIndexTable.assign(itemIndexTable.begin(), itemIndexTable.end());
    return true;
}

const void* find_native_index_row(std::uint16_t definitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    for (const NativeDefinition& definition : g_nativeDefinitions) {
        if (definition.definitionIndex == definitionIndex) {
            return definition.indexRow.data();
        }
    }
    return nullptr;
}

const void* native_item_index_table() noexcept {
    const Lock::Shared guard(g_lock);
    return g_nativeItemIndexTable.size() > 8 ? g_nativeItemIndexTable.data() + 8 : nullptr;
}

const void* find_native(std::uint16_t definitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    for (const NativeDefinition& definition : g_nativeDefinitions) {
        if (definition.definitionIndex == definitionIndex) {
            return definition.bytes.data();
        }
    }
    return nullptr;
}

std::size_t native_count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_nativeDefinitions.size();
}

bool find_hash(std::uint32_t definitionHash, Definition& output) noexcept {
    output = {};
    const Lock::Shared guard(g_lock);
    for (const Definition& definition : g_definitions.rows()) {
        if (definition.definitionHash == definitionHash) {
            output = definition;
            return true;
        }
    }
    return false;
}

bool find_index(std::uint16_t definitionIndex, Definition& output) noexcept {
    output = {};
    const Lock::Shared guard(g_lock);
    for (const Definition& definition : g_definitions.rows()) {
        if (definition.definitionIndex == definitionIndex) {
            output = definition;
            return true;
        }
    }
    return false;
}

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::custom_ornaments
