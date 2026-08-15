#include <array>
#include <span>
#include <vector>

#include "../../../../state/build_data/custom_ornaments/custom_ornament_catalog.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {

/** Walks the located item index table, then publishes every domain that depends on it. */
bool build_item_rows(const reader::Source& source,
                     Storage& storage,
                     const tables::Array& table,
                     std::size_t& rowCount,
                     const char*& reason) noexcept {
    const bool needDefinitions = !state::build_data::item_definitions_ready();
    const bool needDetails = !state::build_data::configured_item_details_ready();
    const bool needRows = needDefinitions || needDetails;
    const bool needCustom = !state::build_data::custom_ornaments::ready();
    bool published = !needRows;
    const std::span<const std::byte> container{storage.child};
    reason = "rows";
    // The requested detail set is gathered during this one walk. An authored row is matched by
    // hash, because the index table that maps a hash to its index is what this loop is building.
    const bool haveAuthored = needRows && collect_authored_hashes(storage.authoredHashes);
    OrnamentExtractionRequest extraction{};
    const bool extractRequested = needRows && load_ornament_extraction_request(extraction);
    if (extractRequested) {
        extract_requested_ornament_graphs(extraction,
                                          source,
                                          storage.scratch,
                                          table,
                                          std::span<const std::byte>{storage.container},
                                          std::span<const std::byte>{storage.root},
                                          container);
    }
    std::size_t detailCount = 0;
    std::size_t unresolvedCount = 0;
    for (std::uint64_t index = 0; needRows && index < table.count && rowCount < storage.rows.size();
         ++index) {
        tables::IndexRow row{};
        if (!tables::index_row(container, table, index, row)) {
            break;
        }
        tables::items::Row item{};
        item.definitionHash = row.definitionHash;
        item.definitionIndex = static_cast<std::uint16_t>(index);
        if (!reader::read_tag(source, storage.scratch, row.targetTag, storage.definition)
            || !tables::items::read_definition(std::span<const std::byte>{storage.definition},
                                               item)) {
            // Keep the native index table dense even when one definition blob is unavailable.
            // Compacting successful rows shifts every later index and makes publication fail.
            storage.rows[rowCount++] = state::build_data::items::Definition{
                row.definitionHash,
                static_cast<std::uint16_t>(index),
                state::build_data::items::kUnresolvedBucketId};
            ++unresolvedCount;
            continue;
        }
        storage.rows[rowCount++] = state::build_data::items::Definition{
            item.definitionHash, item.definitionIndex, item.bucketId};
        if (haveAuthored && authored(storage.authoredHashes, item.definitionHash)) {
            (void)request(item.definitionIndex, storage.requested, detailCount);
            (void)append_initial_plugs(item, storage.requested, detailCount);
        }
    }
    report_item_row_shape(table.count, rowCount, unresolvedCount);
    std::size_t ornamentCount = 0;
    const bool customBuilt =
        !(needRows || needCustom)
        || build_custom_ornaments(
            source, storage, needDefinitions, rowCount, ornamentCount);
    if (!customBuilt) {
        reason = "custom_ornaments";
    }
    if (needRows) {
        compact_requested(storage.requested, detailCount);
        published = rowCount != 0 && customBuilt;
    } else {
        published = customBuilt;
    }
    if (published && needDefinitions) {
        published =
            state::build_data::publish_item_definitions(std::span(storage.rows).first(rowCount));
    }
    if (published && needCustom) {
        published = state::build_data::custom_ornaments::replace(
                        std::span(storage.customOrnaments).first(ornamentCount))
                    && state::build_data::custom_ornaments::replace_native(
                        std::span(storage.nativeDefinitions)
                            .first(storage.nativeDefinitionCount),
                        storage.nativeItemIndexTable);
    }
    if (!published) {
        reason = "publish";
    }
    // Configured rows and every plug they socket each need a detail record, and they are found
    // through the table this pass just published.
    if (published && needDetails) {
        reason = "details";
        const DetailSource detailSource{
            &source, &storage.scratch, container, table, &storage.definition};
        for (std::size_t slot = 0; published && slot < detailCount; ++slot) {
            published = build_detail(detailSource, storage.requested[slot], storage.details[slot]);
            if (!published) {
                report_detail_failure(slot, storage.requested[slot]);
            }
        }
        for (std::size_t slot = 0; published && slot < ornamentCount; ++slot) {
            const tables::items::Row& item = storage.customItemRows[slot];
            if (!haveAuthored || !authored(storage.authoredHashes, item.definitionHash)) {
                continue;
            }
            if (detailCount >= storage.details.size()) {
                published = false;
                break;
            }
            storage.details[detailCount++] = detail_of(item);
        }
        published = published
                    && state::build_data::publish_configured_item_details(
                        std::span(storage.details).first(detailCount));
        report_detail_count(detailCount);
    }
    // Ability buckets read the socket entry list table again and depend on the detail domain, so
    // they run last.
    if (published && !state::build_data::ability_buckets_ready()) {
        reason = "abilities";
        std::size_t abilityCount = 0;
        const bool built = build_character_abilities(source,
                                                     storage.scratch,
                                                     std::span<const std::byte>{storage.root},
                                                     storage.abilityTable,
                                                     storage.definition,
                                                     storage.abilityPool,
                                                     storage.abilityRows,
                                                     abilityCount);
        published = built
                    && state::build_data::publish_ability_buckets(
                        std::span(storage.abilityRows).first(abilityCount));
        if (built) {
            report_ability_count(abilityCount);
        }
    }
    return published && state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::custom_ornaments::ready();
}

} // namespace sunrise::client::content::items::packages
