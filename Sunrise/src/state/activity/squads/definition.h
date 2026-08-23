#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::activity::squads {

/** Exact whole groups retained by the conservative EDZ authored-source catalog. */
inline constexpr std::size_t kGroupCatalogCapacity = 33;
/** Exact independently addressable type-1 sources retained by the catalog. */
inline constexpr std::size_t kSourceCatalogCapacity = 95;
/** Compatibility name for fixed UI row storage. */
inline constexpr std::size_t kCatalogCapacity = kSourceCatalogCapacity;
/** Passive, non-addressable package-location evidence retained beside selectable sources. */
inline constexpr std::size_t kLocationEvidenceCapacity = 1;
/** Widest complete retained group; 64 also covers the largest examined EDZ group. */
inline constexpr std::size_t kGroupSlotCapacity = 64;
/** Fixed human-readable source label copied into diagnostics without heap storage. */
inline constexpr std::size_t kDisplayNameCapacity = 48;
/** Every retained source resolves to exactly one source-specific rule and anchor. */
inline constexpr std::size_t kAuthoredLocationCapacity = 1;
/** Fixed human-readable location label copied into diagnostics without heap storage. */
inline constexpr std::size_t kLocationNameCapacity = 32;
/** Package descriptor-selected Auth schema carried by every retained type-1 slot. */
inline constexpr std::uint32_t kSquadAuthSchema = 0x80807EC9U;
/** Zero is not a valid operator request id. */
inline constexpr std::uint64_t kAbsentRequestId = 0;
/** Activity message 5 carries a positive 31-bit spawn generation. */
inline constexpr std::uint32_t kMaximumSpawnGeneration = 0x7FFFFFFFU;
/** A cleared request has no activity-session owner or native-current route witness. */
inline constexpr std::uint64_t kAbsentSessionId = 0;

/** One package-authored world anchor used by a squad's authored spawn rule. */
struct Anchor final {
    /** Package map-table tag that owns the anchor row, not the placed entity's class tag. */
    std::uint32_t objectTag{};
    std::uint16_t ordinal{};
    float x{};
    float y{};
    float z{};
};

/** Describes whether choosing a source also chooses this exact authored location. */
enum class LocationAddressability : std::uint8_t {
    /** The source has one proved spawn rule, so choosing the source uniquely resolves this row. */
    uniqueForSource,
    /** The source has several rules; the stock game chooses among them after Auth is applied. */
    packageChosenAlternative,
};

/** One proved package-authored rule-to-anchor location reachable from a squad source. */
struct AuthoredLocation final {
    /** Fixed archive-derived label, not a game-owned string. */
    std::array<char, kLocationNameCapacity> displayName{};
    std::uint8_t displayNameLength{};
    /** Type-66 authored spawn-rule configuration resolved by the stock client. */
    std::uint32_t spawnRuleTag{};
    /** Exact type-66 slot containing spawnRuleTag in the complete group. */
    std::uint16_t spawnRuleSlot{};
    Anchor anchor{};
    /** Whether the message-5 ClientRef can distinguish this location from its siblings. */
    LocationAddressability addressability{LocationAddressability::packageChosenAlternative};
};

/** Conservative semantic class shown by the debug panel; it never changes wire behavior. */
enum class SourceClassification : std::uint8_t {
    combat,
    vendorConflict,
    scripted,
    unknown,
};

/** Evidence strength that the authored source creates fightable enemy actors. */
enum class EnemyConfidence : std::uint8_t {
    low,
    medium,
    high,
};

/** One complete package-authored roster group, stored once for all of its type-1 sources. */
struct Group final {
    /** Scenario package object that owns the group. */
    std::uint32_t scenarioTag{};
    /** Exact native region in which the group is authored. */
    std::int32_t region{};
    /** Placed roster-group object registered by activity message 5. */
    std::uint32_t objectTag{};
    /** ClientRef registry key used by every source in this group. */
    std::uint32_t registryKey{};
    /** Whole-group type table in package slot order. */
    std::array<std::uint8_t, kGroupSlotCapacity> slotTypes{};
    /** Whole-group sense/auth flags in package slot order. */
    std::array<std::uint8_t, kGroupSlotCapacity> slotFlags{};
    /** Number of initialized entries in both slot tables. */
    std::uint8_t slotCount{};
};

/** One independently addressable type-1 source in a normalized Group. */
struct Source final {
    /** Fixed package/archive label, not a game-owned string. */
    std::array<char, kDisplayNameCapacity> displayName{};
    std::uint8_t displayNameLength{};
    /** Index of the one complete group in CatalogSnapshot::groups. */
    std::uint8_t groupIndex{};
    /** Exact type-1 source slot inside the complete authored group. */
    std::uint16_t sourceSlot{};
    /** Package-authored source name hash, useful to diagnostics and tools. */
    std::uint32_t sourceNameHash{};
    /** Type-1 squad component configuration resolved by the stock client. */
    std::uint32_t squadConfigTag{};
    /** Exact source-specific rule and anchor; message 5 selects it through this source. */
    AuthoredLocation authoredLocation{};
    SourceClassification classification{SourceClassification::unknown};
    EnemyConfidence enemyConfidence{EnemyConfidence::low};
    /** Descriptor-proved schema identity. The schema id itself is package-selected, not sent. */
    std::uint32_t authSchema{kSquadAuthSchema};
    /** True only when logical Auth member width and count bounds are independently proved. */
    bool authResolved{};
    /** Number of logical member counters in the selected squad Auth schema. */
    std::uint8_t memberSlotCount{};
    /** Authored count the operator control initially presents. */
    std::int32_t defaultRequestedCount{};
    /** Highest count accepted by the fixed operator control. */
    std::int32_t maximumRequestedCount{};
};

/** Passive package location that is not independently selectable through a type-1 ClientRef. */
struct LocationEvidence final {
    std::uint8_t groupIndex{};
    std::uint16_t sourceSlot{};
    AuthoredLocation location{};
};

/** Materialized source plus its group identity, used by request validation and the sender. */
struct Candidate final {
    Source source{};
    std::uint32_t scenarioTag{};
    std::int32_t region{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
};

/** Fixed normalized copy of the immutable operator catalog. */
struct CatalogSnapshot final {
    std::array<Group, kGroupCatalogCapacity> groups{};
    std::array<Source, kSourceCatalogCapacity> sources{};
    std::array<LocationEvidence, kLocationEvidenceCapacity> locationEvidence{};
    std::size_t groupCount{};
    std::size_t sourceCount{};
    std::size_t locationEvidenceCount{};
};

/** Exact native-current foreign route and catalog selection copied from the UI. */
struct PlacementRequestInput final {
    /** Root/primary activity identity that owns this request's lifecycle. */
    std::uint64_t activitySessionId{};
    /** Gameplay group whose native manager owns the area currently on screen. */
    std::uint64_t currentGroupSessionId{};
    /** Foreign ActivityClient session that transports native CURRENT's message 5. */
    std::uint64_t currentHostSessionId{};
    /** Durable accepted-join generation captured for currentGroupSessionId. */
    std::uint64_t currentAdmissionGeneration{};
    std::uint32_t scenarioTag{};
    std::int32_t region{};
    std::uint32_t candidateObjectTag{};
    std::uint16_t candidateSourceSlot{};
    std::int32_t requestedCount{};
    /** Nonzero replication-view authority token captured for currentGroupSessionId. */
    std::uint16_t authorityToken{};
};

/** Validated immutable placement request consumed by the roster snapshot path. */
struct PlacementRequest final {
    std::uint64_t requestId{};
    std::uint64_t activitySessionId{};
    std::uint64_t currentGroupSessionId{};
    std::uint64_t currentHostSessionId{};
    std::uint64_t currentAdmissionGeneration{};
    std::uint32_t scenarioTag{};
    std::int32_t region{};
    std::uint32_t candidateObjectTag{};
    std::uint16_t candidateSourceSlot{};
    std::int32_t requestedCount{};
    /** Positive monotonically advancing 31-bit value written into Squad Auth. */
    std::uint32_t spawnGeneration{};
    std::uint16_t authorityToken{};
};

/** Most recent observable step for the current or last operator request. */
enum class Lifecycle : std::uint8_t {
    idle,
    requested,
    prepared,
    committed,
    discarded,
    refused,
    cleared,
};

/** Stable non-text reason recorded when a roster path refuses a valid operator request. */
enum class RefusalReason : std::uint8_t {
    none,
    worldNotReady,
    activitySessionChanged,
    currentGroupChanged,
    currentHostChanged,
    scenarioChanged,
    regionChanged,
    candidateUnavailable,
    rosterCapacity,
    invalidAuth,
};

/** Copy-only diagnostics for the operator panel and structured logging. */
struct DebugSnapshot final {
    PlacementRequest request{};
    Lifecycle lifecycle{Lifecycle::idle};
    RefusalReason refusalReason{RefusalReason::none};
    std::uint64_t lifecycleRevision{};
    std::uint64_t requestedTick{};
    std::uint64_t updatedTick{};
    std::uint32_t preparedCount{};
    std::uint32_t committedCount{};
    std::uint32_t discardedCount{};
    std::uint32_t refusedCount{};
    /** True while snapshot_request may return the request for later message-5 keepalives. */
    bool requestActive{};
    /** True after this identity committed once, until explicit clear, retire, or reset. */
    bool committedLatched{};
};

} // namespace sunrise::state::activity::squads
