#include "investment_definition_overlay_lifecycle.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/custom_ornaments/custom_ornament_catalog.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"

namespace sunrise::client::hooks::investment {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Accessor for the native investment item registry. The adjacent accessor and function prologue
 * keep this otherwise-small RIP-relative load unique to the supported client build.
 */
constexpr std::string_view kRegistryAccessorText =
    "48 8B 05 ? ? ? ? C3 00 00 00 40 01 00 00 00 40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 10";
constexpr auto kRegistryAccessor =
    signature<signature_length(kRegistryAccessorText)>(kRegistryAccessorText);

/** Every native item-table consumer obtains its array descriptor through this accessor. */
constexpr std::size_t kItemTableVtableOffset = 0x1A8;
/** Resolves the item definition named by one row in that table. */
constexpr std::size_t kDefinitionVtableOffset = 0x558;
/** Resolves an authored definition hash to the index used by the item table. */
constexpr std::size_t kHashIndexVtableOffset = 0x540;
/** Returns the 24-byte item-table row for one definition index. */
constexpr std::size_t kIndexRowVtableOffset = 0x560;

using RegistryAccessor = void*(__fastcall*)();
using DefinitionLookup = const void*(__fastcall*)(void*, std::uint16_t);
using HashIndexLookup = std::int16_t*(__fastcall*)(void*, std::int16_t*, const std::uint32_t*);
using IndexRowLookup = const void*(__fastcall*)(void*, std::uint16_t);

hooking::detour::Handle g_itemTableHandle{};
hooking::detour::Handle g_definitionHandle{};
hooking::detour::Handle g_hashIndexHandle{};
hooking::detour::Handle g_indexRowHandle{};
std::atomic_bool g_installed{};
std::atomic_bool g_accessorFailureReported{};
std::atomic_bool g_registryPendingReported{};
std::atomic_bool g_tableHitReported{};
std::atomic_bool g_definitionHitReported{};
std::atomic_bool g_hashIndexHitReported{};
std::atomic_bool g_customDefinitionHitReported{};
std::atomic_bool g_indexRowHitReported{};

/** Supplies the complete package-authored descriptor, count, and rows to the native registry. */
const void* __fastcall item_index_table() noexcept {
    const void* const table =
        state::build_data::custom_ornaments::native_item_index_table();
    if (table != nullptr
        && !g_tableHitReported.exchange(true, std::memory_order_acq_rel)) {
        std::uint64_t count = 0;
        std::memcpy(&count, table, sizeof count);
        std::array<char, 128> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=investment_overlay stage=item_table_hit count=%llu result=ok",
                                          static_cast<unsigned long long>(count));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return table;
}

/** Resolves rows redirected by the expanded table from the same package-loaded catalogue. */
const void* __fastcall definition(void* registry, std::uint16_t definitionIndex) noexcept {
    const void* const custom =
        state::build_data::custom_ornaments::find_native(definitionIndex);
    if (custom != nullptr) {
        state::build_data::custom_ornaments::Definition ornament{};
        if (state::build_data::custom_ornaments::find_index(definitionIndex, ornament)
            && !g_customDefinitionHitReported.exchange(true, std::memory_order_acq_rel)) {
            std::array<char, 144> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=investment_overlay stage=custom_definition_hit index=%u result=ok",
                static_cast<unsigned>(definitionIndex));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        if (!g_definitionHitReported.exchange(true, std::memory_order_acq_rel)) {
            std::array<char, 128> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=investment_overlay stage=definition_hit index=%u result=ok",
                static_cast<unsigned>(definitionIndex));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        return custom;
    }
    const auto original = reinterpret_cast<DefinitionLookup>(g_definitionHandle.original);
    return original != nullptr ? original(registry, definitionIndex) : nullptr;
}

/** Exposes package-authored rows directly while retaining native behavior for all other indices. */
const void* __fastcall index_row(void* registry, std::uint16_t definitionIndex) noexcept {
    const void* const custom =
        state::build_data::custom_ornaments::find_native_index_row(definitionIndex);
    if (custom != nullptr) {
        state::build_data::custom_ornaments::Definition ornament{};
        if (state::build_data::custom_ornaments::find_index(definitionIndex, ornament)
            && !g_indexRowHitReported.exchange(true, std::memory_order_acq_rel)) {
            std::array<char, 136> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=investment_overlay stage=index_row_hit index=%u result=ok",
                static_cast<unsigned>(definitionIndex));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        return custom;
    }
    const auto original = reinterpret_cast<IndexRowLookup>(g_indexRowHandle.original);
    return original != nullptr ? original(registry, definitionIndex) : nullptr;
}

/** Adds package-authored hashes after preserving the native hash map's result. */
std::int16_t* __fastcall hash_index(void* registry,
                                   std::int16_t* output,
                                   const std::uint32_t* definitionHash) noexcept {
    const auto original = reinterpret_cast<HashIndexLookup>(g_hashIndexHandle.original);
    if (original != nullptr) {
        (void)original(registry, output, definitionHash);
    }
    if (output == nullptr || definitionHash == nullptr || *output != -1) {
        return output;
    }
    state::build_data::custom_ornaments::Definition custom{};
    if (!state::build_data::custom_ornaments::find_hash(*definitionHash, custom)) {
        return output;
    }
    *output = static_cast<std::int16_t>(custom.definitionIndex);
    if (!g_hashIndexHitReported.exchange(true, std::memory_order_acq_rel)) {
        std::array<char, 160> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=investment_overlay stage=hash_index_hit hash=%08X index=%u result=ok",
            static_cast<unsigned>(*definitionHash),
            static_cast<unsigned>(custom.definitionIndex));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return output;
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)
        || state::build_data::custom_ornaments::native_count() == 0) {
        return true;
    }
    std::byte* const accessorBytes =
        scan_main_image_unique(kRegistryAccessor, "investment_item_registry_accessor");
    if (accessorBytes == nullptr) {
        if (!g_accessorFailureReported.exchange(true, std::memory_order_acq_rel)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=investment_overlay stage=accessor result=fail");
        }
        return false;
    }
    const auto accessor = reinterpret_cast<RegistryAccessor>(accessorBytes);
    void* const registry = accessor();
    if (registry == nullptr) {
        if (!g_registryPendingReported.exchange(true, std::memory_order_acq_rel)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::debug,
                             "ev=investment_overlay stage=registry result=pending");
        }
        return false;
    }
    auto* const vtable = *static_cast<void***>(registry);
    void* const target =
        vtable != nullptr ? vtable[kItemTableVtableOffset / sizeof(void*)] : nullptr;
    void* const definitionTarget =
        vtable != nullptr ? vtable[kDefinitionVtableOffset / sizeof(void*)] : nullptr;
    void* const hashIndexTarget =
        vtable != nullptr ? vtable[kHashIndexVtableOffset / sizeof(void*)] : nullptr;
    void* const indexRowTarget =
        vtable != nullptr ? vtable[kIndexRowVtableOffset / sizeof(void*)] : nullptr;
    if (target == nullptr || definitionTarget == nullptr || hashIndexTarget == nullptr
        || indexRowTarget == nullptr
        || !hooking::detour::install(
            hooking::detour::Spec{target, reinterpret_cast<void*>(&item_index_table)},
            g_itemTableHandle)
        || !hooking::detour::install(
            hooking::detour::Spec{definitionTarget, reinterpret_cast<void*>(&definition)},
            g_definitionHandle)
        || !hooking::detour::install(
            hooking::detour::Spec{hashIndexTarget, reinterpret_cast<void*>(&hash_index)},
            g_hashIndexHandle)
        || !hooking::detour::install(
            hooking::detour::Spec{indexRowTarget, reinterpret_cast<void*>(&index_row)},
            g_indexRowHandle)) {
        if (g_itemTableHandle.attached) {
            (void)hooking::detour::uninstall(g_itemTableHandle);
            g_itemTableHandle = {};
        }
        if (g_definitionHandle.attached) {
            (void)hooking::detour::uninstall(g_definitionHandle);
            g_definitionHandle = {};
        }
        if (g_hashIndexHandle.attached) {
            (void)hooking::detour::uninstall(g_hashIndexHandle);
            g_hashIndexHandle = {};
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment_overlay stage=native_path result=fail");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    g_registryPendingReported.store(false, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=investment_overlay stage=native_path result=ok");
    return true;
}

void uninstall() noexcept {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)hooking::detour::uninstall(g_itemTableHandle);
    (void)hooking::detour::uninstall(g_definitionHandle);
    (void)hooking::detour::uninstall(g_hashIndexHandle);
    (void)hooking::detour::uninstall(g_indexRowHandle);
    g_itemTableHandle = {};
    g_definitionHandle = {};
    g_hashIndexHandle = {};
    g_indexRowHandle = {};
    g_accessorFailureReported.store(false, std::memory_order_release);
    g_registryPendingReported.store(false, std::memory_order_release);
    g_tableHitReported.store(false, std::memory_order_release);
    g_definitionHitReported.store(false, std::memory_order_release);
    g_hashIndexHitReported.store(false, std::memory_order_release);
    g_customDefinitionHitReported.store(false, std::memory_order_release);
    g_indexRowHitReported.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::investment
