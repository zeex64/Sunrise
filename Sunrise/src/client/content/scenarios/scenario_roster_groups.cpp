#include <array>
#include <span>

#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/** How many hops the chain from a handle to a descriptor blob may take. */
constexpr std::size_t kChainDepthLimit = 8;
/** Small authored combat groups selected for the first EDZ message-5 placements. */
constexpr std::uint32_t kTrostlandSquadObject = 0x80BE950D;
constexpr std::uint32_t kOutskirtsSquadObject = 0x80BE23FF;

/** @return True when this object is one of the exact slice-local EDZ squad groups below. */
[[nodiscard]] bool is_edz_squad_object(std::uint32_t objectTag) noexcept {
    return objectTag == kTrostlandSquadObject || objectTag == kOutskirtsSquadObject;
}

/**
 * Fills the exact schema-presence flags authored by the selected EDZ squad object.
 * The group's handles use the package's split descriptor form, rather than the combined placed
 * descriptor parsed by the general roster extractor, so its six measured slots are checked here.
 * @param group Exact object whose slot types have already been read.
 * @return True when every declared type belongs to the measured package layout.
 */
[[nodiscard]] bool fill_edz_squad_flags(std::uint32_t objectTag,
                                        layouts::RosterGroup& group) noexcept {
    constexpr std::array<std::uint8_t, 6> kTrostlandTypes = {1, 70, 42, 66, 60, 61};
    constexpr std::array<std::uint8_t, 6> kTrostlandFlags = {
        layouts::kSlotSenseFlag | layouts::kSlotAuthFlag,
        layouts::kSlotSenseFlag | layouts::kSlotAuthFlag,
        layouts::kSlotAuthFlag,
        0,
        0,
        0,
    };
    constexpr std::array<std::uint8_t, 14> kOutskirtsTypes = {
        1, 3, 70, 30, 45, 45, 66, 44, 44, 44, 60, 66, 61, 46};
    constexpr std::array<std::uint8_t, 14> kOutskirtsFlags = {
        layouts::kSlotSenseFlag | layouts::kSlotAuthFlag,
        layouts::kSlotSenseFlag | layouts::kSlotAuthFlag,
        layouts::kSlotSenseFlag | layouts::kSlotAuthFlag,
        layouts::kSlotSenseFlag | layouts::kSlotAuthFlag,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    };
    const std::span<const std::uint8_t> types =
        objectTag == kTrostlandSquadObject   ? std::span<const std::uint8_t>(kTrostlandTypes)
        : objectTag == kOutskirtsSquadObject ? std::span<const std::uint8_t>(kOutskirtsTypes)
                                             : std::span<const std::uint8_t>();
    const std::span<const std::uint8_t> flags =
        objectTag == kTrostlandSquadObject   ? std::span<const std::uint8_t>(kTrostlandFlags)
        : objectTag == kOutskirtsSquadObject ? std::span<const std::uint8_t>(kOutskirtsFlags)
                                             : std::span<const std::uint8_t>();
    if (types.empty() || group.slotCount != types.size() || flags.size() != types.size()) {
        return false;
    }
    for (std::size_t slot = 0; slot < types.size(); ++slot) {
        if (group.slotTypes[slot] != types[slot]) {
            return false;
        }
        group.slotFlags[slot] = flags[slot];
    }
    return true;
}

/**
 * Records one descriptor's schemas against its slot type.
 * @param context Roster storage.
 * @param descriptor Descriptor read from a placed-object blob.
 * @return Always true, because a descriptor of an unknown type is ordinary.
 */
bool record_flags(void* context, const tables::SlotDescriptor& descriptor) noexcept {
    auto& storage = *static_cast<RosterStorage*>(context);
    if (descriptor.slotType >= kSlotTypeSpan) {
        return true;
    }
    std::uint8_t flags = 0;
    if (descriptor.authSchema != tables::kAbsentSchema) {
        flags |= layouts::kSlotAuthFlag;
    }
    if (descriptor.senseSchema != tables::kAbsentSchema) {
        flags |= layouts::kSlotSenseFlag;
    }
    storage.slotFlags[descriptor.slotType] = flags;
    storage.slotFlagsKnown[descriptor.slotType] = 1;
    return true;
}

/**
 * Follows one placed handle to its descriptor blob and records what it declares.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param handle Tag from a placed object's per-bubble sub-block.
 * @param registryKey Registry key the descriptors must name.
 */
void follow_handle(const reader::Source& source,
                   reader::Scratch& scratch,
                   RosterStorage& storage,
                   std::uint32_t handle,
                   std::uint32_t registryKey) noexcept {
    std::uint32_t tag = handle;
    for (std::size_t depth = 0; depth < kChainDepthLimit; ++depth) {
        std::uint32_t classId = 0;
        ++storage.reads;
        if (!reader::read_tag(source, scratch, tag, storage.chain, classId)) {
            return;
        }
        if (classId == tables::kPlacedObjectClass) {
            (void)tables::visit_slot_descriptors(
                storage.chain, tag, registryKey, &record_flags, &storage);
            return;
        }
        std::uint32_t next = 0;
        if (!tables::next_descriptor_tag(storage.chain, classId, next)) {
            return;
        }
        tag = next;
    }
}

/** @param group Candidate group. @return True when every slot type is known. */
[[nodiscard]] bool flags_complete(const RosterStorage& storage,
                                  const layouts::RosterGroup& group) noexcept {
    for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
        if (storage.slotFlagsKnown[group.slotTypes[slot]] == 0) {
            return false;
        }
    }
    return true;
}

/**
 * Reads the slot flags one group object declares, stopping once every type is known.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param objectBlob Whole placed-object bytes.
 * @param group Candidate group whose slot types are already filled.
 */
void resolve_flags(const reader::Source& source,
                   reader::Scratch& scratch,
                   RosterStorage& storage,
                   std::span<const std::byte> objectBlob,
                   const layouts::RosterGroup& group) noexcept {
    tables::Array bubbles{};
    if (flags_complete(storage, group) || !tables::object_bubbles(objectBlob, bubbles)) {
        return;
    }
    for (std::uint64_t index = 0; index < bubbles.count; ++index) {
        tables::ObjectBubble bubble{};
        if (!tables::object_bubble_at(objectBlob, bubbles, index, bubble)) {
            return;
        }
        for (std::uint64_t slot = 0; slot < bubble.handleCount; ++slot) {
            std::uint32_t handle = 0;
            if (!tables::object_placed_handle_at(objectBlob, bubble, slot, handle)) {
                return;
            }
            follow_handle(source, scratch, storage, handle, group.registryKey);
            if (flags_complete(storage, group)) {
                return;
            }
        }
    }
}

/**
 * Fills one candidate group's slot types from the object's own slot array.
 * @param objectBlob Whole placed-object bytes.
 * @param group Receives the key and slot types.
 * @return True when the object declares a usable slot array.
 */
[[nodiscard]] bool fill_slots(std::span<const std::byte> objectBlob,
                              layouts::RosterGroup& group) noexcept {
    tables::Array slots{};
    if (!tables::object_slots(objectBlob, slots) || slots.count == 0
        || slots.count > layouts::kRosterSlotCapacity) {
        return false;
    }
    for (std::uint64_t index = 0; index < slots.count; ++index) {
        tables::Slot slot{};
        if (!tables::object_slot_at(objectBlob, slots, index, slot) || slot.type == 0
            || slot.type > layouts::kMaximumSlotType) {
            return false;
        }
        group.slotTypes[index] = static_cast<std::uint8_t>(slot.type);
    }
    group.slotCount = static_cast<std::uint16_t>(slots.count);
    return true;
}

/** @param storage Working storage. @param tag Object tag. @return Its memo slot, or capacity. */
[[nodiscard]] std::size_t memo_slot(const RosterStorage& storage, std::uint32_t tag) noexcept {
    std::size_t probe = tag % kObjectMemoCapacity;
    for (std::size_t step = 0; step < kObjectMemoCapacity; ++step) {
        if (storage.memo[probe].tag == 0 || storage.memo[probe].tag == tag) {
            return probe;
        }
        probe = (probe + 1) % kObjectMemoCapacity;
    }
    return kObjectMemoCapacity;
}

} // namespace

/**
 * Finds the roster group of one placed object, reading it only the first time it is seen.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param objectTag Tag from an object registry.
 * @param group Receives the roster group index, or the not-a-group sentinel.
 * @return True when the object was read or was already known.
 */
bool resolve_object(const reader::Source& source,
                    reader::Scratch& scratch,
                    RosterStorage& storage,
                    std::uint32_t objectTag,
                    std::uint16_t& group) noexcept {
    group = kNotARosterGroup;
    const std::size_t slot = memo_slot(storage, objectTag);
    if (slot == kObjectMemoCapacity) {
        return false;
    }
    if (storage.memo[slot].tag == objectTag) {
        group = storage.memo[slot].group;
        return true;
    }
    storage.memo[slot].tag = objectTag;
    storage.memo[slot].group = kNotARosterGroup;
    ++storage.reads;
    if (!reader::read_tag(source, scratch, objectTag, storage.object)
        || (!tables::carries_roster_slot(storage.object) && !is_edz_squad_object(objectTag))) {
        return true;
    }

    layouts::RosterGroup candidate{};
    if (!tables::object_key(storage.object, candidate.registryKey) || candidate.registryKey == 0
        || !fill_slots(storage.object, candidate)) {
        return true;
    }
    candidate.objectTag = objectTag;
    if (is_edz_squad_object(objectTag)) {
        if (!fill_edz_squad_flags(objectTag, candidate)) {
            ++storage.unresolvedGroups;
            return true;
        }
    } else {
        resolve_flags(source, scratch, storage, storage.object, candidate);
        if (!flags_complete(storage, candidate)) {
            // A slot whose flags are unknown would be encoded with the wrong reset bits, and phase
            // 2 has no resync point, so the whole group is dropped instead.
            ++storage.unresolvedGroups;
            return true;
        }
        for (std::size_t index = 0; index < candidate.slotCount; ++index) {
            candidate.slotFlags[index] = storage.slotFlags[candidate.slotTypes[index]];
        }
    }
    // One key may carry different layouts in different activities, so only exact layouts reuse.
    for (std::size_t index = 0; index < storage.groupCount; ++index) {
        if (same_group_layout(storage.groups[index], candidate)) {
            storage.memo[slot].group = static_cast<std::uint16_t>(index);
            group = storage.memo[slot].group;
            return true;
        }
    }
    if (storage.groupCount == layouts::kRosterGroupCapacity) {
        return false;
    }
    storage.groups[storage.groupCount] = candidate;
    storage.memo[slot].group = static_cast<std::uint16_t>(storage.groupCount);
    group = storage.memo[slot].group;
    ++storage.groupCount;
    return true;
}

} // namespace sunrise::client::content::scenarios
