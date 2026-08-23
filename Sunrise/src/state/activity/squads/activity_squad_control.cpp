#include "activity_squad_control.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sunrise::state::activity::squads {
namespace {

/** Installed EDZ free-roam scenario owning every retained group. */
constexpr std::uint32_t kEdzScenario = 0x80B2F00AU;

/** Copies a string literal into one fixed diagnostic label. */
template <std::size_t Size>
[[nodiscard]] consteval std::array<char, kDisplayNameCapacity>
fixed_name(const char (&value)[Size]) noexcept {
    static_assert(Size > 0 && Size - 1 <= kDisplayNameCapacity);
    std::array<char, kDisplayNameCapacity> result{};
    for (std::size_t index = 0; index + 1 < Size; ++index) {
        result[index] = value[index];
    }
    return result;
}

/** Copies a string literal into one fixed authored-location label. */
template <std::size_t Size>
[[nodiscard]] consteval std::array<char, kLocationNameCapacity>
fixed_location_name(const char (&value)[Size]) noexcept {
    static_assert(Size > 0 && Size - 1 <= kLocationNameCapacity);
    std::array<char, kLocationNameCapacity> result{};
    for (std::size_t index = 0; index + 1 < Size; ++index) {
        result[index] = value[index];
    }
    return result;
}

/** Exact sense/auth presence for the retained package slot descriptors. */
[[nodiscard]] consteval std::uint8_t slot_flags(std::uint8_t slotType) noexcept {
    switch (slotType) {
    case 1:
    case 3:
    case 30:
    case 70:
        return 3;
    case 42:
        return 2;
    case 44:
    case 45:
    case 46:
    case 60:
    case 61:
    case 66:
        return 0;
    default:
        return 0xFFU;
    }
}

/** Builds one complete group and derives every exact flag from its package slot type. */
template <std::size_t SlotCount>
[[nodiscard]] consteval Group make_group(
    std::int32_t region,
    std::uint32_t objectTag,
    std::uint32_t registryKey,
    const std::array<std::uint8_t, SlotCount>& slotTypes) noexcept {
    static_assert(SlotCount > 0 && SlotCount <= kGroupSlotCapacity);
    Group result{};
    result.scenarioTag = kEdzScenario;
    result.region = region;
    result.objectTag = objectTag;
    result.registryKey = registryKey;
    result.slotCount = static_cast<std::uint8_t>(SlotCount);
    for (std::size_t slot = 0; slot < SlotCount; ++slot) {
        result.slotTypes[slot] = slotTypes[slot];
        result.slotFlags[slot] = slot_flags(slotTypes[slot]);
    }
    return result;
}

/**
 * Type-1 configs whose six parallel class-80808358 arrays prove two logical member counters.
 * Every other retained source proves one. This width alone does not resolve requested-count
 * vector defaults, so it never makes a source sendable.
 */
constexpr std::array<std::uint64_t, kGroupCatalogCapacity> kTwoMemberSlotMasks{{
    0,
    (1ULL << 0U) | (1ULL << 4U),
    0,
    1ULL << 2U,
    0,
    1ULL << 0U,
    0,
    0,
    0,
    (1ULL << 1U) | (1ULL << 4U),
    1ULL << 1U,
    (1ULL << 0U) | (1ULL << 1U),
    0,
    0,
    1ULL << 2U,
    1ULL << 2U,
    0,
    1ULL << 2U,
    0,
    0,
    0,
    1ULL << 0U,
    0,
    0,
    1ULL << 2U,
    0,
    1ULL << 1U,
    1ULL << 1U,
    1ULL << 1U,
    0,
    (1ULL << 1U) | (1ULL << 2U),
    1ULL << 0U,
    (1ULL << 0U) | (1ULL << 3U),
}};

/** @return Package-proved logical member width for one retained source. */
[[nodiscard]] consteval std::uint8_t proved_member_slot_count(std::uint8_t groupIndex,
                                                             std::uint16_t sourceSlot) noexcept {
    return groupIndex < kTwoMemberSlotMasks.size() && sourceSlot < 64
                   && (kTwoMemberSlotMasks[groupIndex] & (1ULL << sourceSlot)) != 0
               ? 2
               : 1;
}

/** Builds one independently addressable source without borrowing archive or package strings. */
template <std::size_t NameSize>
[[nodiscard]] consteval Source make_source(
    const char (&name)[NameSize],
    std::uint8_t groupIndex,
    std::uint16_t sourceSlot,
    std::uint32_t sourceNameHash,
    std::uint32_t squadConfigTag,
    std::uint32_t spawnRuleTag,
    std::uint16_t spawnRuleSlot,
    std::uint32_t anchorObjectTag,
    std::uint16_t anchorOrdinal,
    float x,
    float y,
    float z,
    SourceClassification classification,
    EnemyConfidence enemyConfidence,
    bool authResolved,
    std::uint8_t memberSlotCount,
    std::int32_t defaultRequestedCount,
    std::int32_t maximumRequestedCount) noexcept {
    static_assert(NameSize > 1 && NameSize - 1 <= kDisplayNameCapacity);
    return Source{
        .displayName = fixed_name(name),
        .displayNameLength = static_cast<std::uint8_t>(NameSize - 1),
        .groupIndex = groupIndex,
        .sourceSlot = sourceSlot,
        .sourceNameHash = sourceNameHash,
        .squadConfigTag = squadConfigTag,
        .authoredLocation =
            {
                .displayName = fixed_location_name("Exact source anchor"),
                .displayNameLength = 19,
                .spawnRuleTag = spawnRuleTag,
                .spawnRuleSlot = spawnRuleSlot,
                .anchor =
                    {
                        .objectTag = anchorObjectTag,
                        .ordinal = anchorOrdinal,
                        .x = x,
                        .y = y,
                        .z = z,
                    },
                .addressability = LocationAddressability::uniqueForSource,
            },
        .classification = classification,
        .enemyConfidence = enemyConfidence,
        .authSchema = kSquadAuthSchema,
        .authResolved = authResolved,
        .memberSlotCount = authResolved ? memberSlotCount
                                        : proved_member_slot_count(groupIndex, sourceSlot),
        .defaultRequestedCount = defaultRequestedCount,
        .maximumRequestedCount = maximumRequestedCount,
    };
}

/** Complete normalized group table. Group indices are stable Source foreign keys. */
constexpr std::array<Group, kGroupCatalogCapacity> kGroups{{
    make_group(24, 0x80BE2228U, 0x9272D7E9U, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 61, 66, 66, 45, 45, 45, 45, 45, 60, 44, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE2257U, 0x242A94C8U, std::array<std::uint8_t, 28>{{1, 1, 1, 1, 1, 3, 70, 30, 61, 66, 66, 66, 66, 66, 45, 45, 45, 45, 45, 45, 45, 44, 44, 44, 44, 66, 60, 46}}),
    make_group(24, 0x80BE2285U, 0xBBBD21F4U, std::array<std::uint8_t, 23>{{1, 1, 1, 3, 70, 30, 66, 60, 66, 61, 45, 45, 45, 45, 45, 45, 45, 44, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE22ACU, 0x0258B41AU, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 66, 60, 66, 61, 45, 45, 45, 45, 45, 44, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE22D1U, 0x482C8699U, std::array<std::uint8_t, 16>{{1, 1, 3, 70, 30, 66, 61, 45, 45, 45, 60, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE22EFU, 0x9BACA995U, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 66, 66, 66, 45, 45, 45, 45, 45, 60, 44, 44, 44, 66, 61, 46}}),
    make_group(24, 0x80BE2314U, 0x4B815DCCU, std::array<std::uint8_t, 18>{{3, 1, 1, 70, 30, 45, 45, 45, 45, 66, 44, 66, 44, 66, 60, 44, 61, 46}}),
    make_group(24, 0x80BE233CU, 0x9F701030U, std::array<std::uint8_t, 22>{{3, 1, 1, 1, 70, 30, 45, 45, 45, 45, 45, 66, 44, 44, 66, 66, 44, 66, 60, 61, 45, 46}}),
    make_group(24, 0x80BE2360U, 0x46434F1EU, std::array<std::uint8_t, 18>{{1, 1, 3, 70, 30, 66, 45, 45, 45, 66, 44, 44, 44, 44, 60, 61, 66, 46}}),
    make_group(24, 0x80BE238EU, 0xDD834A2EU, std::array<std::uint8_t, 31>{{3, 1, 1, 1, 1, 70, 30, 45, 45, 45, 45, 45, 45, 66, 66, 60, 66, 61, 66, 45, 66, 44, 44, 44, 44, 44, 44, 44, 45, 45, 46}}),
    make_group(24, 0x80BE23B5U, 0x142E4FE6U, std::array<std::uint8_t, 26>{{3, 1, 1, 1, 70, 30, 45, 45, 45, 45, 45, 66, 66, 60, 66, 61, 66, 44, 44, 44, 44, 44, 44, 45, 45, 46}}),
    make_group(24, 0x80BE23DDU, 0x8648BDA1U, std::array<std::uint8_t, 26>{{1, 1, 1, 1, 3, 70, 30, 60, 66, 61, 66, 66, 45, 45, 45, 45, 45, 45, 45, 44, 44, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE23FFU, 0xE0DD0E60U, std::array<std::uint8_t, 14>{{1, 3, 70, 30, 45, 45, 66, 44, 44, 44, 60, 66, 61, 46}}),
    make_group(24, 0x80BE2420U, 0x62632AECU, std::array<std::uint8_t, 15>{{1, 1, 3, 70, 30, 61, 45, 45, 66, 44, 44, 60, 66, 66, 46}}),
    make_group(24, 0x80BE2445U, 0x967195ADU, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 61, 66, 66, 45, 45, 45, 45, 45, 60, 44, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE2460U, 0x54DAF3C1U, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 61, 66, 66, 45, 45, 45, 45, 45, 60, 44, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE24D2U, 0x325DD7D0U, std::array<std::uint8_t, 27>{{1, 1, 1, 1, 3, 70, 30, 66, 66, 66, 66, 45, 45, 45, 45, 45, 45, 45, 60, 44, 44, 44, 44, 44, 66, 61, 46}}),
    make_group(24, 0x80BE24FFU, 0x8C943A1CU, std::array<std::uint8_t, 23>{{1, 1, 1, 3, 70, 30, 61, 66, 66, 45, 45, 45, 45, 45, 60, 44, 44, 44, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE2509U, 0xAB5DBA27U, std::array<std::uint8_t, 16>{{1, 1, 3, 70, 30, 66, 61, 45, 45, 45, 60, 44, 44, 66, 66, 46}}),
    make_group(24, 0x80BE2516U, 0x09AF5B37U, std::array<std::uint8_t, 20>{{1, 1, 1, 3, 70, 30, 66, 66, 66, 45, 45, 45, 45, 60, 44, 44, 44, 66, 61, 46}}),
    make_group(24, 0x80BE256EU, 0x6F9D9F93U, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 66, 61, 45, 45, 45, 45, 45, 66, 44, 44, 44, 60, 66, 66, 46}}),
    make_group(24, 0x80BE25BAU, 0x4BFB369BU, std::array<std::uint8_t, 17>{{1, 1, 3, 70, 30, 66, 45, 45, 45, 66, 44, 44, 44, 60, 61, 66, 46}}),
    make_group(408, 0x80BE950DU, 0xC984DDDEU, std::array<std::uint8_t, 6>{{1, 70, 42, 66, 60, 61}}),
    make_group(408, 0x80BE951FU, 0x96C5D88CU, std::array<std::uint8_t, 23>{{1, 1, 1, 3, 70, 30, 66, 61, 45, 45, 45, 45, 45, 45, 45, 66, 44, 44, 44, 60, 66, 66, 46}}),
    make_group(408, 0x80BE9541U, 0xC6476E49U, std::array<std::uint8_t, 31>{{1, 1, 1, 3, 70, 30, 66, 60, 66, 61, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 44, 44, 44, 44, 44, 44, 66, 66, 46}}),
    make_group(408, 0x80BE9573U, 0xC9918F05U, std::array<std::uint8_t, 30>{{1, 1, 1, 3, 70, 30, 66, 60, 66, 61, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 44, 44, 44, 44, 44, 66, 66, 46}}),
    make_group(408, 0x80BE95B6U, 0x7295E953U, std::array<std::uint8_t, 27>{{1, 1, 1, 3, 3, 70, 30, 66, 66, 45, 45, 45, 45, 45, 60, 44, 44, 44, 44, 44, 44, 66, 45, 45, 66, 61, 46}}),
    make_group(408, 0x80BE95C7U, 0x37BB975BU, std::array<std::uint8_t, 28>{{1, 1, 1, 3, 3, 70, 30, 66, 66, 45, 45, 45, 45, 45, 45, 60, 44, 44, 44, 44, 44, 44, 66, 45, 45, 66, 61, 46}}),
    make_group(408, 0x80BE95DAU, 0x482F6FDCU, std::array<std::uint8_t, 26>{{1, 1, 1, 3, 70, 30, 66, 44, 66, 61, 45, 45, 45, 45, 45, 45, 45, 45, 60, 44, 44, 44, 44, 66, 66, 46}}),
    make_group(408, 0x80BE95F2U, 0x8B0BB60AU, std::array<std::uint8_t, 26>{{1, 1, 1, 3, 3, 70, 30, 66, 66, 45, 45, 45, 45, 45, 60, 44, 44, 44, 44, 44, 66, 45, 45, 61, 66, 46}}),
    make_group(408, 0x80BE9608U, 0x976C8791U, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 66, 61, 45, 45, 45, 45, 45, 66, 44, 44, 44, 60, 66, 66, 46}}),
    make_group(408, 0x80BE961BU, 0x4648E00DU, std::array<std::uint8_t, 21>{{1, 1, 1, 3, 70, 30, 66, 61, 45, 45, 45, 45, 45, 66, 44, 44, 44, 60, 66, 66, 46}}),
    make_group(408, 0x80BE9638U, 0x83B37CFDU, std::array<std::uint8_t, 25>{{1, 1, 1, 1, 3, 70, 30, 66, 66, 66, 66, 45, 45, 45, 45, 45, 45, 44, 44, 44, 44, 60, 66, 61, 46}}),
}};

/** Exact source-specific rows. Only two currently have complete requested-count vectors. */
constexpr std::array<Source, kSourceCatalogCapacity> kSources{{
    make_source("pf_beach_simple._squad[0]", 0, 0, 0x65B407E5U, 0x80BE3545U, 0x80BE3535U, 8, 0x80BE20FFU, 0, 84.139610291F, -123.132530212F, 11.239041328F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_beach_simple._squad[1]", 0, 1, 0x66B40916U, 0x80BE3548U, 0x80BE353CU, 19, 0x80BE20FFU, 1, 55.713275909F, -125.566970825F, 11.514183044F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_beach_simple._squad[2]", 0, 2, 0x67B40A83U, 0x80BE354BU, 0x80BE3532U, 7, 0x80BE20FFU, 2, 88.946319580F, -98.529594421F, 16.352521896F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[0]", 1, 0, 0x503FF4EEU, 0x80BE356FU, 0x80BE355CU, 11, 0x80BE2127U, 4, 256.707275391F, -7.919555187F, 29.416696548F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[1]", 1, 1, 0x4F3FF37DU, 0x80BE3573U, 0x80BE3569U, 25, 0x80BE2127U, 0, 254.524536133F, -36.626487732F, 39.500007629F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[2]", 1, 2, 0x523FF784U, 0x80BE3576U, 0x80BE3559U, 10, 0x80BE2127U, 2, 253.781738281F, -19.780658722F, 34.499984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[4]", 1, 3, 0x543FFAA2U, 0x80BE357CU, 0x80BE3556U, 9, 0x80BE2127U, 1, 242.197402954F, -15.211795807F, 29.500007629F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[3]", 1, 4, 0x513FF61BU, 0x80BE357FU, 0x80BE355FU, 12, 0x80BE2127U, 3, 247.593414307F, -24.840919495F, 29.500007629F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_alt_simple._squad[0]", 2, 0, 0xCB8F20FEU, 0x80BE35A0U, 0x80BE3591U, 20, 0x80BE215BU, 2, 256.707275391F, -7.919555187F, 29.416696548F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_alt_simple._squad[1]", 2, 1, 0xCA8F1F4DU, 0x80BE35A3U, 0x80BE3588U, 6, 0x80BE215BU, 0, 243.435577393F, -16.947637558F, 29.787744522F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_alt_simple._squad[2]", 2, 2, 0xCD8F2394U, 0x80BE35A6U, 0x80BE358BU, 8, 0x80BE215BU, 1, 253.781738281F, -19.780658722F, 34.499984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_coast_fallen_dropship._squad[2]", 3, 0, 0x2A7D4A86U, 0x80BE35C0U, 0x80BE35B7U, 18, 0x80BE2182U, 2, 278.374603271F, -159.938339233F, 10.941139221F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_coast_fallen_dropship._squad[1]", 3, 1, 0x2B7D4C33U, 0x80BE35C3U, 0x80BE35B1U, 6, 0x80BE2182U, 0, 228.709625244F, -140.913208008F, 12.035655022F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_coast_fallen_dropship._squad[0]", 3, 2, 0x2C7D4E5CU, 0x80BE35C9U, 0x80BE35B4U, 8, 0x80BE2182U, 1, 257.691986084F, -145.272872925F, 10.683089256F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_culvert_simple._squad[0]", 4, 0, 0xC0D6E259U, 0x80BE35E0U, 0x80BE35D5U, 13, 0x80BE21AAU, 1, 301.239929199F, -51.066757202F, 10.903111458F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_culvert_simple._squad[1]", 4, 1, 0xC1D6E3CAU, 0x80BE35E3U, 0x80BE35D2U, 5, 0x80BE21AAU, 0, 278.623199463F, -54.857032776F, 10.960733414F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_culvert_alt_simple._squad[0]", 5, 0, 0xA1485BE9U, 0x80BE3601U, 0x80BE35F8U, 8, 0x80BE21CCU, 2, 281.351867676F, -54.114368439F, 10.903111458F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_culvert_alt_simple._squad[1]", 5, 1, 0xA2485D1AU, 0x80BE3605U, 0x80BE35F3U, 7, 0x80BE21CCU, 1, 261.688690186F, -53.998435974F, 13.329497337F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_culvert_alt_simple._squad[2]", 5, 2, 0xA3485E87U, 0x80BE3608U, 0x80BE35F0U, 6, 0x80BE21CCU, 0, 298.064025879F, -52.106288910F, 10.968665123F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_dwelling_simple._squad[0]", 6, 1, 0xA10AA462U, 0x80BE3627U, 0x80BE361EU, 11, 0x80BE21F5U, 0, 313.937896729F, 30.057899475F, 29.458631516F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_dwelling_simple._squad[1]", 6, 2, 0xA00AA2F1U, 0x80BE362EU, 0x80BE3621U, 13, 0x80BE21F5U, 1, 314.767608643F, 41.189907074F, 29.585941315F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_dwelling_alt_simple._squad[0]", 7, 1, 0x3D6798CAU, 0x80BE3673U, 0x80BE3657U, 15, 0x80BE2218U, 1, 313.937896729F, 30.057899475F, 29.458631516F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_dwelling_alt_simple._squad[2]", 7, 2, 0x3F679BE0U, 0x80BE367AU, 0x80BE3654U, 14, 0x80BE2218U, 0, 312.825347900F, 38.653247833F, 29.585897446F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_dwelling_alt_simple._squad[1]", 7, 3, 0x3C679759U, 0x80BE367DU, 0x80BE365AU, 17, 0x80BE2218U, 2, 314.767608643F, 41.189907074F, 29.585941315F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_facade_simple._squad[0]", 8, 0, 0x82D393C4U, 0x80BE3692U, 0x80BE3686U, 5, 0x80BE2240U, 1, 277.571807861F, 56.952995300F, 35.000007629F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_facade_simple._squad[1]", 8, 1, 0x81D3925BU, 0x80BE3695U, 0x80BE368CU, 16, 0x80BE2240U, 0, 276.847259521F, 54.416221619F, 29.627967834F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_hilltop_fallen_dropship._squad[0]", 9, 1, 0xC34FCE70U, 0x80BE36D4U, 0x80BE36A4U, 14, 0x80BE2262U, 3, 181.689895630F, -85.364555359F, 37.155971527F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_hilltop_fallen_dropship._squad[1]", 9, 2, 0xC24FCCC7U, 0x80BE36D7U, 0x80BE36BFU, 16, 0x80BE2262U, 0, 227.509887695F, -70.766227722F, 31.767143250F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_hilltop_fallen_dropship._squad[2]", 9, 3, 0xC14FCB5AU, 0x80BE36DAU, 0x80BE36C9U, 18, 0x80BE2262U, 1, 206.093826294F, -53.954776764F, 31.106586456F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_hilltop_fallen_dropship._squad[3]", 9, 4, 0xC04FC929U, 0x80BE36DDU, 0x80BE36CCU, 20, 0x80BE2262U, 2, 217.304992676F, -49.585735321F, 31.924711227F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_hilltop_alt_fallen_dropship._squad[0]", 10, 1, 0x75BF0064U, 0x80BE3740U, 0x80BE372AU, 12, 0x80BE2290U, 2, 181.689895630F, -85.364555359F, 37.155971527F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_hilltop_alt_fallen_dropship._squad[1]", 10, 2, 0x74BEFEFBU, 0x80BE3746U, 0x80BE372DU, 14, 0x80BE2290U, 0, 227.509887695F, -70.766227722F, 31.767143250F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_hilltop_alt_fallen_dropship._squad[2]", 10, 3, 0x73BEFD4EU, 0x80BE3756U, 0x80BE3730U, 16, 0x80BE2290U, 1, 184.847625732F, -50.069072723F, 29.175807953F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_marsh_simple._squad[0]", 11, 0, 0xC6B7D4EDU, 0x80BE3784U, 0x80BE3778U, 11, 0x80BE22B7U, 3, 150.873031616F, -3.611662149F, 18.646820068F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_marsh_simple._squad[1]", 11, 1, 0xC7B7D61EU, 0x80BE3787U, 0x80BE377EU, 24, 0x80BE22B7U, 0, 143.547805786F, 18.352153778F, 18.255113602F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_marsh_simple._squad[2]", 11, 2, 0xC8B7D78BU, 0x80BE378BU, 0x80BE376EU, 8, 0x80BE22B7U, 1, 175.041183472F, -2.590023279F, 20.040565491F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_marsh_simple._squad[3]", 11, 3, 0xC9B7D934U, 0x80BE378FU, 0x80BE3775U, 10, 0x80BE22B7U, 2, 126.244026184F, -18.457698822F, 18.862485886F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_parking_simple._squad[0]", 12, 0, 0xD4D95CC6U, 0x80BE37AEU, 0x80BE37A8U, 11, 0x80BE22E5U, 0, 149.806930542F, -109.127372742F, 18.394191742F, SourceClassification::combat, EnemyConfidence::medium, true, 1, 1, 1),
    make_source("pf2_parking_alt_simple._squad[0]", 13, 0, 0x5CF28EB6U, 0x80BE38C8U, 0x80BE37CFU, 13, 0x80BE2301U, 0, 149.806930542F, -109.127372742F, 18.394191742F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_parking_alt_simple._squad[1]", 13, 1, 0x5BF28D05U, 0x80BE38CBU, 0x80BE37C9U, 12, 0x80BE2301U, 1, 153.103759766F, -108.852416992F, 18.173522949F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_path_simple._squad[0]", 14, 0, 0xAED5C6ADU, 0x80BE38E9U, 0x80BE38D7U, 8, 0x80BE2322U, 2, 57.198711395F, 23.855913162F, 37.888080597F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_path_simple._squad[1]", 14, 1, 0xAFD5C8DEU, 0x80BE38ECU, 0x80BE38E3U, 19, 0x80BE2322U, 0, 60.233119965F, -1.443636179F, 34.847450256F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_path_simple._squad[2]", 14, 2, 0xB0D5CA4BU, 0x80BE391FU, 0x80BE38D4U, 7, 0x80BE2322U, 1, 83.417175293F, 28.081417084F, 35.380588531F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_path_alt_simple._squad[0]", 15, 0, 0x08A05A25U, 0x80BE395BU, 0x80BE393AU, 8, 0x80BE234AU, 2, 57.198711395F, 23.855913162F, 37.888080597F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_path_alt_simple._squad[1]", 15, 1, 0x09A05C56U, 0x80BE3961U, 0x80BE3940U, 19, 0x80BE234AU, 0, 60.233119965F, -1.443636179F, 34.847450256F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_path_alt_simple._squad[2]", 15, 2, 0x0AA05DC3U, 0x80BE3964U, 0x80BE3937U, 7, 0x80BE234AU, 1, 83.417175293F, 28.081417084F, 35.380588531F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_residence_simple._squad[0]", 16, 0, 0x480C714EU, 0x80BE3998U, 0x80BE3973U, 8, 0x80BE2371U, 3, 225.931777954F, 46.820728302F, 39.499996185F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_residence_simple._squad[2]", 16, 1, 0x4A0C7464U, 0x80BE399BU, 0x80BE3992U, 24, 0x80BE2371U, 0, 219.722503662F, 42.863162994F, 39.489383698F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_residence_simple._squad[3]", 16, 2, 0x490C72FBU, 0x80BE399EU, 0x80BE3970U, 7, 0x80BE2371U, 1, 246.842102051F, 27.979475021F, 29.982400894F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_residence_simple._squad[1]", 16, 3, 0x470C6FDDU, 0x80BE39A1U, 0x80BE3976U, 9, 0x80BE2371U, 2, 237.054595947F, 39.103336334F, 34.658901215F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_residence_alt_simple._squad[0]", 17, 0, 0xC1462E9EU, 0x80BE39B9U, 0x80BE39ADU, 8, 0x80BE239FU, 2, 225.931777954F, 46.820728302F, 39.499996185F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_residence_alt_simple._squad[2]", 17, 1, 0xC34631B4U, 0x80BE39BCU, 0x80BE39B3U, 21, 0x80BE239FU, 0, 240.152313232F, 29.655950546F, 33.536705017F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_residence_alt_simple._squad[1]", 17, 2, 0xC0462D6DU, 0x80BE39BFU, 0x80BE39AAU, 7, 0x80BE239FU, 1, 219.678482056F, 42.781970978F, 39.903759003F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_shore_simple._squad[0]", 18, 0, 0xAA13F62BU, 0x80BE39D7U, 0x80BE39CEU, 13, 0x80BE23C6U, 0, 344.622039795F, -136.664672852F, 11.754228592F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_shore_simple._squad[1]", 18, 1, 0xAB13F854U, 0x80BE39DAU, 0x80BE39CBU, 5, 0x80BE23C6U, 1, 350.546997070F, -108.194808960F, 11.276945114F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_shore_alt_simple._squad[0]", 19, 0, 0x97D5828FU, 0x80BE39FDU, 0x80BE39F0U, 8, 0x80BE23E8U, 0, 334.914520264F, -155.695114136F, 11.754228592F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_shore_alt_simple._squad[1]", 19, 1, 0x98D58438U, 0x80BE3A00U, 0x80BE39EDU, 7, 0x80BE23E8U, 2, 348.432830811F, -138.053054810F, 12.037323952F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_shore_alt_simple._squad[2]", 19, 2, 0x95D57FF1U, 0x80BE3A03U, 0x80BE39EAU, 6, 0x80BE23E8U, 1, 320.809265137F, -159.164672852F, 11.508558273F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_underground_simple._squad[0]", 20, 0, 0xABFE93A3U, 0x80BE3A22U, 0x80BE3A19U, 19, 0x80BE2410U, 2, 217.953414917F, -60.506698608F, 13.000454903F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_underground_simple._squad[1]", 20, 1, 0xACFE95CCU, 0x80BE3A25U, 0x80BE3A16U, 18, 0x80BE2410U, 0, 231.479217529F, -62.980476379F, 13.000436783F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_underground_simple._squad[2]", 20, 2, 0xA9FE9085U, 0x80BE3A28U, 0x80BE3A0CU, 6, 0x80BE2410U, 1, 210.653839111F, -47.137546539F, 13.864979744F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_underground_alt_simple._squad[0]", 21, 0, 0xAB603AA7U, 0x80BE3A82U, 0x80BE3A75U, 5, 0x80BE2438U, 1, 217.953414917F, -60.506698608F, 13.000454903F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_underground_alt_simple._squad[1]", 21, 1, 0xAC603CD0U, 0x80BE3A85U, 0x80BE3A7BU, 15, 0x80BE2438U, 0, 210.717895508F, -46.402400970F, 14.000000000F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("sq_vendor_fallen_conflict", 22, 0, 0x3C066BE8U, 0x80BE9309U, 0x80BE9303U, 3, 0x80BE6452U, 6, 541.294921875F, 92.516426086F, 92.361770630F, SourceClassification::vendorConflict, EnemyConfidence::high, true, 1, 1, 1),
    make_source("pf2_apartments_simple._squad[0]", 23, 0, 0x1EDC4BB1U, 0x80BE9326U, 0x80BE931FU, 21, 0x80BE6465U, 2, 466.745147705F, 81.362083435F, 79.261909485F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_apartments_simple._squad[1]", 23, 1, 0x1FDC4D22U, 0x80BE932AU, 0x80BE931CU, 20, 0x80BE6465U, 0, 480.304534912F, 87.623214722F, 79.346359253F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_apartments_simple._squad[2]", 23, 2, 0x20DC4F4FU, 0x80BE932DU, 0x80BE9312U, 6, 0x80BE6465U, 1, 471.417266846F, 84.856269836F, 79.262001038F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[0]", 24, 0, 0x503FF4EEU, 0x80BE934CU, 0x80BE933DU, 28, 0x80BE648DU, 2, 598.501098633F, 95.179023743F, 80.549980164F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[1]", 24, 1, 0x4F3FF37DU, 0x80BE934FU, 0x80BE9336U, 6, 0x80BE648DU, 0, 599.612121582F, 101.337860107F, 74.999984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_simple._squad[2]", 24, 2, 0x523FF784U, 0x80BE9352U, 0x80BE933AU, 8, 0x80BE648DU, 1, 584.985168457F, 103.618064880F, 74.991455078F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_alt_simple._squad[0]", 25, 0, 0xCB8F20FEU, 0x80BE9371U, 0x80BE9362U, 27, 0x80BE64B5U, 2, 600.596008301F, 99.545898438F, 75.152412415F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_alt_simple._squad[1]", 25, 1, 0xCA8F1F4DU, 0x80BE9374U, 0x80BE935BU, 6, 0x80BE64B5U, 0, 583.886596680F, 103.079063416F, 74.999984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_building_alt_simple._squad[2]", 25, 2, 0xCD8F2394U, 0x80BE9378U, 0x80BE935EU, 8, 0x80BE64B5U, 1, 600.721313477F, 93.662086487F, 80.779785156F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_business_simple._squad[0]", 26, 0, 0xE75B2C28U, 0x80BE9395U, 0x80BE9386U, 8, 0x80BE64DCU, 2, 484.690246582F, 129.175292969F, 74.266166687F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_business_simple._squad[1]", 26, 1, 0xE65B2ABFU, 0x80BE9399U, 0x80BE938DU, 24, 0x80BE64DCU, 0, 474.906707764F, 135.355178833F, 74.749984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_business_simple._squad[2]", 26, 2, 0xE55B2912U, 0x80BE939EU, 0x80BE9382U, 7, 0x80BE64DCU, 1, 482.678527832F, 115.169998169F, 74.749984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_business_alt_simple._squad[0]", 27, 0, 0x9B12DE24U, 0x80BE93BAU, 0x80BE93AAU, 8, 0x80BE6507U, 2, 484.690246582F, 129.175292969F, 74.266166687F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_business_alt_simple._squad[1]", 27, 1, 0x9A12DCBBU, 0x80BE93BDU, 0x80BE93B2U, 25, 0x80BE6507U, 0, 474.906707764F, 135.355178833F, 74.749984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_business_alt_simple._squad[2]", 27, 2, 0x9912DB0EU, 0x80BE93C1U, 0x80BE93A7U, 7, 0x80BE6507U, 1, 482.678527832F, 115.169998169F, 74.749984741F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_collapse_simple._squad[0]", 28, 0, 0xCB9630C9U, 0x80BE93D7U, 0x80BE93D1U, 23, 0x80BE6531U, 2, 532.583068848F, 192.377716064F, 84.569297791F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_collapse_simple._squad[1]", 28, 1, 0xCC96327AU, 0x80BE93DBU, 0x80BE93CAU, 6, 0x80BE6531U, 0, 522.455749512F, 186.110382080F, 78.170005798F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_collapse_simple._squad[2]", 28, 2, 0xCD9633E7U, 0x80BE93DEU, 0x80BE93CEU, 8, 0x80BE6531U, 1, 530.640441895F, 177.039505005F, 74.875801086F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_courtyard_simple._squad[0]", 29, 0, 0xBC879045U, 0x80BE93FFU, 0x80BE93E4U, 8, 0x80BE6550U, 2, 496.635223389F, 81.422576904F, 74.684616089F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_courtyard_simple._squad[1]", 29, 1, 0xBD8791F6U, 0x80BE9403U, 0x80BE93E1U, 7, 0x80BE6550U, 1, 474.461242676F, 69.347396851F, 73.521141052F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_courtyard_simple._squad[2]", 29, 2, 0xBE879363U, 0x80BE9407U, 0x80BE93ECU, 24, 0x80BE6550U, 0, 490.705322266F, 90.380134583F, 74.472473145F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_hallway_simple._squad[0]", 30, 0, 0xC8EAAF5CU, 0x80BE9420U, 0x80BE941AU, 19, 0x80BE657BU, 2, 550.159118652F, 149.256423950F, 72.892257690F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_hallway_simple._squad[1]", 30, 1, 0xC7EAAD33U, 0x80BE9424U, 0x80BE9417U, 18, 0x80BE657BU, 0, 566.233154297F, 133.426864624F, 76.565200806F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_hallway_simple._squad[2]", 30, 2, 0xC6EAAB86U, 0x80BE9427U, 0x80BE9410U, 6, 0x80BE657BU, 1, 558.452392578F, 146.797927856F, 73.010002136F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_hallway_alt_simple._squad[0]", 31, 0, 0x595456F0U, 0x80BE9452U, 0x80BE944CU, 19, 0x80BE65A3U, 2, 550.159118652F, 149.256423950F, 72.892257690F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_hallway_alt_simple._squad[1]", 31, 1, 0x58545547U, 0x80BE9457U, 0x80BE9449U, 18, 0x80BE65A3U, 0, 566.233154297F, 133.426864624F, 76.565200806F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf2_hallway_alt_simple._squad[2]", 31, 2, 0x575453DAU, 0x80BE945AU, 0x80BE9443U, 6, 0x80BE65A3U, 1, 558.452392578F, 146.797927856F, 73.010002136F, SourceClassification::combat, EnemyConfidence::medium, false, 0, 0, 0),
    make_source("pf_landslide_fallen_dropship._squad[0]", 32, 0, 0xCB372000U, 0x80BE9479U, 0x80BE9468U, 8, 0x80BE65CAU, 3, 565.858886719F, 52.216976166F, 72.531723022F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_landslide_fallen_dropship._squad[1]", 32, 1, 0xCA371E97U, 0x80BE947CU, 0x80BE9472U, 22, 0x80BE65CAU, 0, 562.199340820F, 62.634025574F, 72.405914307F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_landslide_fallen_dropship._squad[2]", 32, 2, 0xC9371D6AU, 0x80BE947FU, 0x80BE9465U, 7, 0x80BE65CAU, 1, 549.277832031F, 42.513328552F, 72.031784058F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
    make_source("pf_landslide_fallen_dropship._squad[3]", 32, 3, 0xC8371BF9U, 0x80BE9482U, 0x80BE946BU, 9, 0x80BE65CAU, 2, 561.050476074F, 44.353672028F, 72.982803345F, SourceClassification::scripted, EnemyConfidence::high, false, 0, 0, 0),
}};

/** Generic group rule retained only as passive, non-addressable package evidence. */
constexpr std::array<LocationEvidence, kLocationEvidenceCapacity> kLocationEvidence{{
    {
        .groupIndex = 12,
        .sourceSlot = 0,
        .location =
            {
                .displayName = fixed_location_name("Generic group alternative"),
                .displayNameLength = 25,
                .spawnRuleTag = 0x80BE37A5U,
                .spawnRuleSlot = 6,
                .anchor =
                    {
                        .objectTag = 0x80BE20F8U,
                        .ordinal = 2,
                        .x = 140.052902222F,
                        .y = -61.294075012F,
                        .z = 39.026412964F,
                    },
                .addressability = LocationAddressability::packageChosenAlternative,
            },
    },
}};

/** @return True when every static foreign key, group layout, and Auth gate is coherent. */
[[nodiscard]] consteval bool catalog_valid() noexcept {
    for (const Group& group : kGroups) {
        if (group.scenarioTag != kEdzScenario || group.registryKey == 0 || group.slotCount == 0
            || group.slotCount > group.slotTypes.size()) {
            return false;
        }
        for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
            if (group.slotTypes[slot] == 0 || group.slotFlags[slot] == 0xFFU) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < kSources.size(); ++index) {
        const Source& source = kSources[index];
        if (source.groupIndex >= kGroups.size()) {
            return false;
        }
        const Group& group = kGroups[source.groupIndex];
        if (source.sourceSlot >= group.slotCount || group.slotTypes[source.sourceSlot] != 1
            || (group.slotFlags[source.sourceSlot] & 2U) == 0
            || source.authSchema != kSquadAuthSchema
            || source.authoredLocation.spawnRuleTag == 0
            || source.authoredLocation.spawnRuleSlot >= group.slotCount
            || group.slotTypes[source.authoredLocation.spawnRuleSlot] != 66
            || source.authoredLocation.addressability
                   != LocationAddressability::uniqueForSource) {
            return false;
        }
        if (source.authResolved) {
            if (source.memberSlotCount == 0 || source.memberSlotCount > 15
                || source.defaultRequestedCount < 1
                || source.maximumRequestedCount < source.defaultRequestedCount) {
                return false;
            }
        } else if (source.memberSlotCount == 0 || source.memberSlotCount > 15
                   || source.defaultRequestedCount != 0 || source.maximumRequestedCount != 0) {
            return false;
        }
        for (std::size_t other = index + 1; other < kSources.size(); ++other) {
            if (source.groupIndex == kSources[other].groupIndex
                && source.sourceSlot == kSources[other].sourceSlot) {
                return false;
            }
        }
    }
    for (const LocationEvidence& evidence : kLocationEvidence) {
        if (evidence.groupIndex >= kGroups.size()
            || evidence.sourceSlot >= kGroups[evidence.groupIndex].slotCount
            || evidence.location.addressability
                   != LocationAddressability::packageChosenAlternative) {
            return false;
        }
    }
    return true;
}

static_assert(catalog_valid());

/** Private fixed state guarded independently from the root account/activity State. */
struct ControlState final {
    DebugSnapshot debug{};
    /** Last issued value; zero means the first successful request receives one. */
    std::uint64_t lastRequestId{};
    /** Last issued value; zero means the first successful request receives one. */
    std::uint32_t lastSpawnGeneration{};
};

SRWLOCK g_lock{SRWLOCK_INIT};
ControlState g_state{};

/** Borrowed immutable catalog match used only while holding no mutable State lock. */
struct CatalogMatch final {
    const Group* group{};
    const Source* source{};
};

/** @return Exact normalized group/source pair for the complete authored identity. */
[[nodiscard]] CatalogMatch find_catalog_candidate(std::uint32_t scenarioTag,
                                                  std::int32_t region,
                                                  std::uint32_t objectTag,
                                                  std::uint16_t sourceSlot) noexcept {
    for (const Source& source : kSources) {
        if (source.groupIndex >= kGroups.size()) {
            continue;
        }
        const Group& group = kGroups[source.groupIndex];
        if (group.scenarioTag == scenarioTag && group.region == region
            && group.objectTag == objectTag && source.sourceSlot == sourceSlot) {
            return {&group, &source};
        }
    }
    return {};
}

/** @return True only when the source's logical Auth body and bounds are proved. */
[[nodiscard]] bool auth_ready(const Source& source) noexcept {
    return source.authResolved && source.authSchema == kSquadAuthSchema
           && source.memberSlotCount > 0 && source.memberSlotCount <= 15
           && source.defaultRequestedCount >= 1
           && source.maximumRequestedCount >= source.defaultRequestedCount;
}

/** Copies a normalized pair into the request/sender-facing composite. */
void materialize(const CatalogMatch& match, Candidate& output) noexcept {
    output = {};
    if (match.group == nullptr || match.source == nullptr) {
        return;
    }
    output.source = *match.source;
    output.scenarioTag = match.group->scenarioTag;
    output.region = match.group->region;
    output.objectTag = match.group->objectTag;
    output.registryKey = match.group->registryKey;
}

/** Advances a bounded diagnostic counter without allowing it to wrap. */
void increment(std::uint32_t& value) noexcept {
    if (value != (std::numeric_limits<std::uint32_t>::max)()) {
        ++value;
    }
}

/** Advances the lifecycle revision without allowing a stale value to recur. */
void advance_revision(DebugSnapshot& debug) noexcept {
    if (debug.lifecycleRevision != (std::numeric_limits<std::uint64_t>::max)()) {
        ++debug.lifecycleRevision;
    }
    debug.updatedTick = GetTickCount64();
}

/** @return True when a late transport callback still names the exact active request. */
[[nodiscard]] bool matches_active(const DebugSnapshot& debug,
                                  std::uint64_t requestId,
                                  std::uint64_t activitySessionId) noexcept {
    return debug.requestActive && requestId != kAbsentRequestId
           && activitySessionId != kAbsentSessionId && debug.request.requestId == requestId
           && debug.request.activitySessionId == activitySessionId;
}

/** Writes one guarded lifecycle transition and optionally increments its counter. */
[[nodiscard]] bool note_lifecycle(std::uint64_t requestId,
                                  std::uint64_t activitySessionId,
                                  Lifecycle lifecycle,
                                  RefusalReason reason) noexcept {
    bool accepted = false;
    AcquireSRWLockExclusive(&g_lock);
    DebugSnapshot& debug = g_state.debug;
    if (matches_active(debug, requestId, activitySessionId)) {
        debug.lifecycle = lifecycle;
        debug.refusalReason = reason;
        switch (lifecycle) {
        case Lifecycle::prepared:
            increment(debug.preparedCount);
            break;
        case Lifecycle::committed:
            increment(debug.committedCount);
            debug.committedLatched = true;
            break;
        case Lifecycle::discarded:
            increment(debug.discardedCount);
            break;
        case Lifecycle::refused:
            increment(debug.refusedCount);
            break;
        default:
            break;
        }
        advance_revision(debug);
        accepted = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return accepted;
}

} // namespace

/** Copies the normalized fixed package-authored squad catalog. */
void snapshot_catalog(CatalogSnapshot& output) noexcept {
    output = {};
    output.groups = kGroups;
    output.sources = kSources;
    output.locationEvidence = kLocationEvidence;
    output.groupCount = kGroups.size();
    output.sourceCount = kSources.size();
    output.locationEvidenceCount = kLocationEvidence.size();
}

/** Copies one exact complete static group. */
bool find_group(std::uint32_t scenarioTag,
                std::int32_t region,
                std::uint32_t objectTag,
                Group& output) noexcept {
    output = {};
    for (const Group& group : kGroups) {
        if (group.scenarioTag == scenarioTag && group.region == region
            && group.objectTag == objectTag) {
            output = group;
            return true;
        }
    }
    return false;
}

/** Finds one exact source in the fixed catalog. */
bool find_candidate(std::uint32_t scenarioTag,
                    std::int32_t region,
                    std::uint32_t objectTag,
                    std::uint16_t sourceSlot,
                    Candidate& output) noexcept {
    const CatalogMatch match =
        find_catalog_candidate(scenarioTag, region, objectTag, sourceSlot);
    materialize(match, output);
    if (match.source == nullptr) {
        return false;
    }
    return true;
}

/** Validates and publishes one exact placement request. */
bool request_placement(const PlacementRequestInput& input, PlacementRequest& output) noexcept {
    output = {};
    const CatalogMatch match = find_catalog_candidate(input.scenarioTag,
                                                       input.region,
                                                       input.candidateObjectTag,
                                                       input.candidateSourceSlot);
    if (match.source == nullptr || !auth_ready(*match.source)
        || input.activitySessionId == kAbsentSessionId
        || input.currentGroupSessionId == kAbsentSessionId
        || input.currentHostSessionId == kAbsentSessionId
        || input.currentHostSessionId == input.activitySessionId
        || input.currentAdmissionGeneration == 0 || input.authorityToken == 0
        || input.requestedCount < 1
        || input.requestedCount > match.source->maximumRequestedCount) {
        return false;
    }

    bool published = false;
    AcquireSRWLockExclusive(&g_lock);
    const DebugSnapshot& previous = g_state.debug;
    if (!previous.requestActive
        && g_state.lastRequestId != (std::numeric_limits<std::uint64_t>::max)()
        && g_state.lastSpawnGeneration < kMaximumSpawnGeneration) {
        PlacementRequest request{};
        request.requestId = ++g_state.lastRequestId;
        request.activitySessionId = input.activitySessionId;
        request.currentGroupSessionId = input.currentGroupSessionId;
        request.currentHostSessionId = input.currentHostSessionId;
        request.currentAdmissionGeneration = input.currentAdmissionGeneration;
        request.scenarioTag = input.scenarioTag;
        request.region = input.region;
        request.candidateObjectTag = input.candidateObjectTag;
        request.candidateSourceSlot = input.candidateSourceSlot;
        request.requestedCount = input.requestedCount;
        request.spawnGeneration = ++g_state.lastSpawnGeneration;
        request.authorityToken = input.authorityToken;

        DebugSnapshot& debug = g_state.debug;
        debug = {};
        debug.request = request;
        debug.lifecycle = Lifecycle::requested;
        debug.lifecycleRevision = 1;
        debug.requestedTick = GetTickCount64();
        debug.updatedTick = debug.requestedTick;
        debug.requestActive = true;
        output = request;
        published = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return published;
}

/** Copies the active placement request. */
bool snapshot_request(PlacementRequest& output) noexcept {
    output = {};
    bool present = false;
    AcquireSRWLockShared(&g_lock);
    if (g_state.debug.requestActive) {
        output = g_state.debug.request;
        present = true;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Clears an uncommitted, unstaged active request and records the operator action. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    DebugSnapshot& debug = g_state.debug;
    if (debug.requestActive && !debug.committedLatched && debug.lifecycle != Lifecycle::prepared) {
        debug.requestActive = false;
        debug.lifecycle = Lifecycle::cleared;
        debug.refusalReason = RefusalReason::none;
        debug.committedLatched = false;
        advance_revision(debug);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies request lifecycle counters and the current or last request. */
void snapshot_debug(DebugSnapshot& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    output = g_state.debug;
    ReleaseSRWLockShared(&g_lock);
}

/** Marks a matching request as prepared. */
bool note_prepared(std::uint64_t requestId, std::uint64_t activitySessionId) noexcept {
    return note_lifecycle(requestId, activitySessionId, Lifecycle::prepared, RefusalReason::none);
}

/** Marks a matching request as committed. */
bool note_committed(std::uint64_t requestId, std::uint64_t activitySessionId) noexcept {
    return note_lifecycle(requestId, activitySessionId, Lifecycle::committed, RefusalReason::none);
}

/** Marks a matching request's prepared send as discarded. */
bool note_discarded(std::uint64_t requestId, std::uint64_t activitySessionId) noexcept {
    return note_lifecycle(requestId, activitySessionId, Lifecycle::discarded, RefusalReason::none);
}

/** Marks a matching request as refused. */
bool note_refused(std::uint64_t requestId,
                  std::uint64_t activitySessionId,
                  RefusalReason reason) noexcept {
    if (reason == RefusalReason::none) {
        reason = RefusalReason::invalidAuth;
    }
    return note_lifecycle(requestId, activitySessionId, Lifecycle::refused, reason);
}

/** Atomically retires a matching request after its exact owning activity goes stale. */
bool retire(std::uint64_t requestId,
            std::uint64_t activitySessionId,
            RefusalReason reason) noexcept {
    if (reason == RefusalReason::none) {
        reason = RefusalReason::invalidAuth;
    }
    bool retired = false;
    AcquireSRWLockExclusive(&g_lock);
    DebugSnapshot& debug = g_state.debug;
    if (matches_active(debug, requestId, activitySessionId)) {
        debug.requestActive = false;
        debug.lifecycle = Lifecycle::refused;
        debug.refusalReason = reason;
        debug.committedLatched = false;
        increment(debug.refusedCount);
        advance_revision(debug);
        retired = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return retired;
}

/** Clears runtime state while preserving monotonic identities. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t lastRequestId = g_state.lastRequestId;
    const std::uint32_t lastSpawnGeneration = g_state.lastSpawnGeneration;
    g_state = {};
    g_state.lastRequestId = lastRequestId;
    g_state.lastSpawnGeneration = lastSpawnGeneration;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::state::activity::squads
