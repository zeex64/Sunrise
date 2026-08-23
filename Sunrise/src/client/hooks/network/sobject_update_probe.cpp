#include "sobject_update_probe.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "../../../core/logging/log.h"
#include "../../player/player_position.h"
#include "../../targets/game.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::sobject_update_probe {
namespace {

constexpr std::size_t kCreateBufferSize = 0x10;
constexpr std::size_t kMaskCaptureSize = 0x10;
constexpr std::size_t kSeenCapacity = 32;
constexpr std::size_t kFlushedCaptureCapacity = 64;
/** Named component scratch ends at 0x84 and the RSAT-defined region is 16-byte aligned. */
constexpr std::size_t kNamedComponentRsatOffset = 0x90;
/** Private component storage used only while asking the native encoder to measure a shape. */
constexpr std::size_t kSyntheticComponentCapacity = 0x200;
/** More than enough room for an all-clean component-presence update. */
constexpr std::size_t kSyntheticWireCapacity = 0x100;
/** Native update contexts extend through the diagnostic byte at +0x54. */
constexpr std::size_t kSyntheticContextSize = 0x60;
/** The decoded transform stores its second float4 immediately after the quaternion. */
constexpr std::size_t kTransformSecondFloat4Offset = 0x10;
constexpr std::size_t kStreamSourceOffset = 0x70;
constexpr std::size_t kStreamSourceSize = 0x20;
constexpr std::uint32_t kSharedVandalRsat = 0x815B204B;
constexpr std::uint32_t kNaturalPlayerRsat = 0x80EF143E;
/** The disk RSAT has 53 descriptors; its loaded wire map contains only 20 compiled rows. */
constexpr std::size_t kVandalComponentCount = 20;
constexpr std::uint32_t kVandalActiveComponentCount = 20;
constexpr std::size_t kVandalNestedCaptureCapacity = 128;
constexpr std::size_t kNestedValueCaptureSize = 16;

std::array<std::atomic_uint64_t, kSeenCapacity> g_seen{};
std::atomic_bool g_decodedRecordProbed{};
std::atomic_bool g_preloadAttempted{};
std::atomic_bool g_vandalComponentMapReported{};
std::atomic_bool g_vandalComponentMapFailureReported{};
SRWLOCK g_nearbyUpdateLock{SRWLOCK_INIT};
NearbyUpdateCapture g_nearbyUpdate{};

struct NearbyTransformCapture {
    std::array<float, 8> transform{};
    std::uint32_t rsat{};
    bool present{};
};

NearbyTransformCapture g_nearbyTransform{};

struct StreamSourceCapture {
    std::array<std::byte, kStreamSourceSize> bytes{};
    bool present{};
};

SRWLOCK g_streamSourceLock{SRWLOCK_INIT};
StreamSourceCapture g_streamSource{};

using Encoder = std::uint64_t(__fastcall*)(void*, const void*);

/** Native bit-writer fields needed to isolate the bits appended by one codec call. */
struct WriterSnapshot {
    std::int32_t flushedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    const std::byte* cursor{};
};

/** Exact native bit-writer fields used by the sobject update encoder. */
struct NativeWriter {
    std::byte* begin{};
    std::byte* end{};
    std::uint64_t reserved10{};
    std::uint64_t reserved18{};
    std::int32_t flushedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    std::uint32_t reserved34{};
    std::byte* cursor{};
};

static_assert(offsetof(NativeWriter, flushedBits) == 0x20);
static_assert(offsetof(NativeWriter, totalBits) == 0x24);
static_assert(offsetof(NativeWriter, accumulator) == 0x28);
static_assert(offsetof(NativeWriter, pendingBits) == 0x30);
static_assert(offsetof(NativeWriter, cursor) == 0x38);
static_assert(sizeof(NativeWriter) == 0x40);

/** Stable, shallow fields read from the kind-0 update context before native encoding. */
struct UpdateSnapshot {
    const void* dirty{};
    const void* sent{};
    const void* createBuffer{};
    std::int64_t componentBase{};
    void* writerAddress{};
    std::uint8_t diagnostics{};
    std::array<std::byte, kCreateBufferSize> create{};
    std::array<std::byte, kMaskCaptureSize> dirtyBytes{};
    std::array<std::byte, kMaskCaptureSize> sentBytes{};
    WriterSnapshot writer{};
};

/** One runtime-built top-level RSAT component descriptor. */
struct ComponentDescriptorSample {
    std::uint32_t componentClass{};
    std::uint32_t schemaTag{};
    std::uint32_t key{};
    std::uint32_t nestedCount{};
    std::int32_t decision{-1};
    bool active{};
};

/** One runtime-built nested codec record owned by an active component descriptor. */
struct NestedDescriptorSample {
    std::uint32_t componentIndex{};
    std::uint32_t nestedIndex{};
    std::uint32_t codecTag{};
    std::int32_t scratchOffset{};
    std::int32_t dataOffset{};
    std::int32_t dirtyBit{};
    std::int32_t repeatCount{};
    std::int32_t scratchStride{};
    std::int32_t bitStride{};
    bool dirty{};
    bool sent{};
    bool valueReadable{};
    std::array<std::byte, kNestedValueCaptureSize> value{};
};

/** Fixed-capacity one-shot snapshot of the shared-Vandal's runtime descriptor map. */
struct ComponentMapSnapshot {
    const void* tableSlot{};
    const void* tableWrapper{};
    const void* table{};
    const void* page{};
    const void* entries{};
    const void* resource{};
    std::uint32_t pageIndex{};
    std::int32_t stride{};
    std::int32_t adjustmentMask{};
    std::uint64_t encodedAdjustment{};
    std::uint64_t resourceCount{};
    std::int64_t listRelative{};
    std::uint32_t activeCount{};
    std::uint32_t nestedTotal{};
    std::uint32_t nestedCaptured{};
    std::int32_t dirtyBase{};
    bool nestedTruncated{};
    std::array<ComponentDescriptorSample, kVandalComponentCount> components{};
    std::array<NestedDescriptorSample, kVandalNestedCaptureCapacity> nested{};
};

enum class ComponentMapStatus : std::uint8_t {
    complete,
    activeMismatch,
    resourceUnavailable,
    headerMismatch,
    fault,
};

/** @return Stable diagnostic name for one component-map snapshot outcome. */
[[nodiscard]] const char* component_map_status_name(ComponentMapStatus status) noexcept {
    switch (status) {
    case ComponentMapStatus::complete:
        return "complete";
    case ComponentMapStatus::activeMismatch:
        return "active-mismatch";
    case ComponentMapStatus::resourceUnavailable:
        return "resource-unavailable";
    case ComponentMapStatus::headerMismatch:
        return "header-mismatch";
    case ComponentMapStatus::fault:
        return "fault";
    default:
        return "unknown";
    }
}

/** Sign-extends the low `width` bits without relying on implementation-defined shifts. */
[[nodiscard]] constexpr std::int32_t sign_extend(std::uint32_t value,
                                                 std::uint32_t width) noexcept {
    const std::uint32_t sign = 1U << (width - 1U);
    const std::uint32_t mask = (1U << width) - 1U;
    value &= mask;
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

/** Reads one native dirty-mask bit using the same inline/sparse layout as FUN_140A00AA0. */
[[nodiscard]] bool mask_bit(const void* maskAddress, std::int32_t bit) noexcept {
    if (maskAddress == nullptr || bit < 0) {
        return false;
    }
    __try {
        const auto* const words = static_cast<const std::uint32_t*>(maskAddress);
        const std::uint32_t metadata = words[2];
        const std::int32_t baseWord = sign_extend(metadata, 14);
        const std::uint32_t wordIndex = static_cast<std::uint32_t>(bit) >> 5U;
        const std::uint32_t bitMask = 1U << (static_cast<std::uint32_t>(bit) & 31U);
        if (baseWord == -1 || baseWord == -2) {
            const std::int32_t bitCount = sign_extend(metadata >> 14U, 14);
            const std::int32_t wordCount = bitCount > 0 ? (bitCount + 31) / 32 : 0;
            const std::uint32_t* sparse{};
            std::memcpy(&sparse, maskAddress, sizeof sparse);
            return sparse != nullptr && wordIndex < static_cast<std::uint32_t>(wordCount)
                   && (sparse[wordIndex] & bitMask) != 0;
        }
        return baseWord >= 0 && wordIndex == static_cast<std::uint32_t>(baseWord)
               && (words[0] & bitMask) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Resolves a loaded tag through the exact page-table arithmetic used by the native encoder. */
[[nodiscard]] bool resolve_runtime_resource(std::uint32_t tag,
                                            ComponentMapSnapshot& snapshot) noexcept {
    snapshot.tableSlot = nullptr;
    snapshot.tableWrapper = nullptr;
    snapshot.table = nullptr;
    snapshot.page = nullptr;
    snapshot.entries = nullptr;
    snapshot.resource = nullptr;
    const targets::game::network::Targets& resolved = targets::game::network::get();
    snapshot.tableSlot = resolved.sobjectResourceTableBaseSlot;
    if (resolved.sobjectResourceTableBaseSlot == nullptr) {
        return false;
    }
    __try {
        const auto signedPage = static_cast<std::int32_t>(tag) >> 13;
        const std::uint64_t pageIndex =
            ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(signedPage)) | 0x0FFC0000ULL)
             >> 18U)
            & static_cast<std::uint16_t>(signedPage);
        snapshot.pageIndex = static_cast<std::uint32_t>(pageIndex);
        const std::byte* tableWrapper{};
        std::memcpy(&tableWrapper, resolved.sobjectResourceTableBaseSlot, sizeof tableWrapper);
        snapshot.tableWrapper = tableWrapper;
        if (tableWrapper == nullptr || pageIndex > 0xFFFF) {
            return false;
        }
        const std::byte* table{};
        std::memcpy(&table, tableWrapper, sizeof table);
        snapshot.table = table;
        if (table == nullptr) {
            return false;
        }
        const std::byte* const page = table + pageIndex * 0x40;
        snapshot.page = page;
        const std::byte* entries{};
        std::int32_t stride{};
        std::int32_t adjustmentMask{};
        std::memcpy(&entries, page + 0x08, sizeof entries);
        std::memcpy(&stride, page + 0x30, sizeof stride);
        std::memcpy(&adjustmentMask, page + 0x34, sizeof adjustmentMask);
        snapshot.entries = entries;
        snapshot.stride = stride;
        snapshot.adjustmentMask = adjustmentMask;
        if (entries == nullptr || stride <= 0) {
            return false;
        }
        const std::uint32_t rowOffset = (tag & 0x1FFFU) * static_cast<std::uint32_t>(stride);
        const std::byte* const row = entries + rowOffset;
        std::uint64_t encodedAdjustment{};
        std::memcpy(&encodedAdjustment, row + 0x08, sizeof encodedAdjustment);
        snapshot.encodedAdjustment = encodedAdjustment;
        const std::uint64_t adjustment =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(adjustmentMask))
            & encodedAdjustment;
        const std::uintptr_t rowAddress = reinterpret_cast<std::uintptr_t>(row);
        // The native table stores a signed relative displacement behind an all-ones mask.
        // Its SUB deliberately wraps in uintptr_t space when the encoded value is negative.
        snapshot.resource = reinterpret_cast<const std::byte*>(rowAddress - adjustment);
        return snapshot.resource != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snapshot.resource = nullptr;
        return false;
    }
}

/** Builds a bounded read-only snapshot of the loaded shared-Vandal component map. */
[[nodiscard]] ComponentMapStatus inspect_component_map(const void* dirty,
                                                       const void* sent,
                                                       std::int32_t dirtyBase,
                                                       const void* componentBase,
                                                       std::size_t componentSize,
                                                       ComponentMapSnapshot& output) noexcept {
    output = {};
    if (!resolve_runtime_resource(kSharedVandalRsat, output)) {
        return ComponentMapStatus::resourceUnavailable;
    }
    const auto* const resource = static_cast<const std::byte*>(output.resource);
    output.dirtyBase = dirtyBase;
    __try {
        std::uint64_t count{};
        std::int64_t listRelative{};
        std::memcpy(&count, resource + 0x30, sizeof count);
        std::memcpy(&listRelative, resource + 0x38, sizeof listRelative);
        output.resourceCount = count;
        output.listRelative = listRelative;
        if (count != kVandalComponentCount || listRelative < -0x100000 || listRelative > 0x100000) {
            return ComponentMapStatus::headerMismatch;
        }
        const std::byte* const container = resource + 0x38 + listRelative;
        std::int32_t decision = 3;
        for (std::size_t index = 0; index < output.components.size(); ++index) {
            const std::byte* const row = container + 0x10 + index * 0x20;
            ComponentDescriptorSample& component = output.components[index];
            std::uint64_t nestedCount{};
            std::int64_t nestedRelative{};
            std::memcpy(&component.componentClass, row + 0x00, sizeof component.componentClass);
            std::memcpy(&component.schemaTag, row + 0x04, sizeof component.schemaTag);
            std::memcpy(&nestedCount, row + 0x08, sizeof nestedCount);
            std::memcpy(&nestedRelative, row + 0x10, sizeof nestedRelative);
            std::memcpy(&component.key, row + 0x18, sizeof component.key);
            component.nestedCount =
                nestedCount > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(nestedCount);
            if (nestedCount == 0 || nestedRelative < -0x1000000 || nestedRelative > 0x1000000) {
                continue;
            }
            const std::byte* const nestedBase = row + 0x10 + nestedRelative;
            std::uint32_t firstCodec{};
            std::memcpy(&firstCodec, nestedBase + 0x10, sizeof firstCodec);
            component.active = firstCodec != 0xFFFFFFFF;
            if (!component.active) {
                continue;
            }
            component.decision = decision++;
            ++output.activeCount;
            output.nestedTotal += component.nestedCount;
            const std::uint64_t boundedCount =
                nestedCount < 64 ? nestedCount : static_cast<std::uint64_t>(64);
            output.nestedTruncated = output.nestedTruncated || nestedCount > boundedCount;
            for (std::uint64_t nestedIndex = 0; nestedIndex < boundedCount; ++nestedIndex) {
                if (output.nestedCaptured >= output.nested.size()) {
                    output.nestedTruncated = true;
                    break;
                }
                const std::byte* const record = nestedBase + nestedIndex * 0x28;
                NestedDescriptorSample& nested = output.nested[output.nestedCaptured++];
                nested.componentIndex = static_cast<std::uint32_t>(index);
                nested.nestedIndex = static_cast<std::uint32_t>(nestedIndex);
                std::memcpy(&nested.codecTag, record + 0x10, sizeof nested.codecTag);
                std::memcpy(&nested.scratchOffset, record + 0x1C, sizeof nested.scratchOffset);
                std::memcpy(&nested.dataOffset, record + 0x20, sizeof nested.dataOffset);
                std::memcpy(&nested.dirtyBit, record + 0x24, sizeof nested.dirtyBit);
                std::memcpy(&nested.repeatCount, record + 0x28, sizeof nested.repeatCount);
                std::memcpy(&nested.scratchStride, record + 0x30, sizeof nested.scratchStride);
                std::memcpy(&nested.bitStride, record + 0x34, sizeof nested.bitStride);
                const std::int32_t absoluteDirty = dirtyBase + nested.dirtyBit;
                nested.dirty = mask_bit(dirty, absoluteDirty);
                nested.sent = mask_bit(sent, absoluteDirty);
                const auto scratchOffset = static_cast<std::size_t>(nested.scratchOffset);
                if (componentBase != nullptr && nested.scratchOffset >= 0
                    && scratchOffset <= componentSize
                    && nested.value.size() <= componentSize - scratchOffset) {
                    const auto* const value =
                        static_cast<const std::byte*>(componentBase) + scratchOffset;
                    std::memcpy(nested.value.data(), value, nested.value.size());
                    nested.valueReadable = true;
                }
            }
        }
        return output.activeCount == kVandalActiveComponentCount
                   ? ComponentMapStatus::complete
                   : ComponentMapStatus::activeMismatch;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ComponentMapStatus::fault;
    }
}

/** Reads only the bit-writer fields used by the probe. */
[[nodiscard]] bool inspect_writer(const void* writerAddress, WriterSnapshot& output) noexcept {
    if (writerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(writerAddress);
        std::memcpy(&output.flushedBits, bytes + 0x20, sizeof output.flushedBits);
        std::memcpy(&output.totalBits, bytes + 0x24, sizeof output.totalBits);
        std::memcpy(&output.accumulator, bytes + 0x28, sizeof output.accumulator);
        std::memcpy(&output.pendingBits, bytes + 0x30, sizeof output.pendingBits);
        std::memcpy(&output.cursor, bytes + 0x38, sizeof output.cursor);
        return output.pendingBits <= 64;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Reads the update context and its two mask objects without changing native state. */
[[nodiscard]] bool inspect_update(const void* contextAddress, UpdateSnapshot& output) noexcept {
    if (contextAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const context = static_cast<const std::byte*>(contextAddress);
        std::memcpy(&output.dirty, context + 0x08, sizeof output.dirty);
        std::memcpy(&output.sent, context + 0x10, sizeof output.sent);
        std::memcpy(&output.createBuffer, context + 0x20, sizeof output.createBuffer);
        std::memcpy(&output.componentBase, context + 0x30, sizeof output.componentBase);
        std::memcpy(&output.writerAddress, context + 0x48, sizeof output.writerAddress);
        std::memcpy(&output.diagnostics, context + 0x54, sizeof output.diagnostics);
        if (output.dirty == nullptr || output.sent == nullptr || output.createBuffer == nullptr
            || output.writerAddress == nullptr) {
            return false;
        }
        std::memcpy(output.create.data(), output.createBuffer, output.create.size());
        std::memcpy(output.dirtyBytes.data(), output.dirty, output.dirtyBytes.size());
        std::memcpy(output.sentBytes.data(), output.sent, output.sentBytes.size());
        return inspect_writer(output.writerAddress, output.writer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** FNV-1a hashes the stable input bytes into the bounded duplicate filter. */
[[nodiscard]] std::uint64_t update_key(const UpdateSnapshot& update) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto append = [&hash](std::span<const std::byte> bytes) noexcept {
        for (const std::byte value : bytes) {
            hash ^= std::to_integer<std::uint8_t>(value);
            hash *= 1099511628211ULL;
        }
    };
    append(update.create);
    append(update.dirtyBytes);
    append(update.sentBytes);
    return hash == 0 ? 1 : hash;
}

/** @return True only for the first bounded capture of this input state. */
[[nodiscard]] bool record_once(const UpdateSnapshot& update) noexcept {
    const std::uint64_t key = update_key(update);
    for (std::atomic_uint64_t& seen : g_seen) {
        std::uint64_t current = seen.load(std::memory_order_relaxed);
        if (current == key) {
            return false;
        }
        if (current == 0
            && seen.compare_exchange_strong(
                current, key, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
        if (current == key) {
            return false;
        }
    }
    return false;
}

/** Converts a bounded byte capture to uppercase hexadecimal. */
void hex(std::span<const std::byte> input, std::span<char> output) noexcept {
    constexpr char kDigits[] = "0123456789ABCDEF";
    if (output.empty()) {
        return;
    }
    const std::size_t count =
        input.size() < (output.size() - 1) / 2 ? input.size() : (output.size() - 1) / 2;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<std::uint8_t>(input[index]);
        output[index * 2] = kDigits[value >> 4];
        output[index * 2 + 1] = kDigits[value & 0x0F];
    }
    output[count * 2] = '\0';
}

/** Emits one bounded runtime descriptor map after a complete snapshot was obtained. */
void report_component_map(const char* source,
                          ComponentMapStatus status,
                          const ComponentMapSnapshot& map) noexcept {
    const bool complete =
        status == ComponentMapStatus::complete || status == ComponentMapStatus::activeMismatch;
    if (complete) {
        if (g_vandalComponentMapReported.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
    } else if (g_vandalComponentMapFailureReported.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=vandal-component-map status=%s source=%s rsat=0x%08X "
        "slot=%p wrapper=%p table=%p page_index=0x%X page=%p entries=%p stride=%d "
        "adjust_mask=0x%08X encoded_adjust=0x%016llX resource=%p "
        "resource_count=%llu list_relative=%lld rows=%zu active=%u "
        "expected_active=%u decisions=%u nested_total=%u nested_captured=%u dirty_base=%d "
        "truncated=%u",
        component_map_status_name(status),
        source,
        kSharedVandalRsat,
        map.tableSlot,
        map.tableWrapper,
        map.table,
        map.pageIndex,
        map.page,
        map.entries,
        map.stride,
        static_cast<std::uint32_t>(map.adjustmentMask),
        static_cast<unsigned long long>(map.encodedAdjustment),
        map.resource,
        static_cast<unsigned long long>(map.resourceCount),
        static_cast<long long>(map.listRelative),
        map.components.size(),
        map.activeCount,
        kVandalActiveComponentCount,
        map.activeCount + 3,
        map.nestedTotal,
        map.nestedCaptured,
        map.dirtyBase,
        map.nestedTruncated ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    if (!complete) {
        return;
    }

    for (std::size_t index = 0; index < map.components.size(); ++index) {
        const ComponentDescriptorSample& component = map.components[index];
        written = std::snprintf(line.data(),
                                line.size(),
                                "ev=gameplay stage=vandal-component-row index=%zu "
                                "class=0x%08X schema=0x%08X key=0x%08X nested=%u active=%u "
                                "decision=%d",
                                index,
                                component.componentClass,
                                component.schemaTag,
                                component.key,
                                component.nestedCount,
                                component.active ? 1U : 0U,
                                component.decision);
        if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }

    for (std::size_t index = 0; index < map.nestedCaptured; ++index) {
        const NestedDescriptorSample& nested = map.nested[index];
        std::array<char, kNestedValueCaptureSize * 2 + 1> valueHex{};
        if (nested.valueReadable) {
            hex(nested.value, valueHex);
        } else {
            std::memcpy(valueHex.data(), "unreadable", sizeof "unreadable");
        }
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=gameplay stage=vandal-component-nested row=%u nested=%u codec=0x%08X "
            "scratch=%d data=%d dirty_bit=%d repeat=%d scratch_stride=%d bit_stride=%d "
            "dirty=%u sent=%u value=%s",
            nested.componentIndex,
            nested.nestedIndex,
            nested.codecTag,
            nested.scratchOffset,
            nested.dataOffset,
            nested.dirtyBit,
            nested.repeatCount,
            nested.scratchStride,
            nested.bitStride,
            nested.dirty ? 1U : 0U,
            nested.sent ? 1U : 0U,
            valueHex.data());
        if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Writes one pointer into the known native update-context layout. */
void set_context_pointer(std::span<std::byte> context,
                         std::size_t offset,
                         const void* value) noexcept {
    std::memcpy(context.data() + offset, &value, sizeof value);
}

/** Changes one private transform float after rejecting values the native codec cannot use. */
[[nodiscard]] bool
set_component_float(std::span<std::byte> component, std::size_t offset, float value) noexcept {
    if (!std::isfinite(value) || component.size() < sizeof value
        || offset > component.size() - sizeof value) {
        return false;
    }
    std::memcpy(component.data() + offset, &value, sizeof value);
    return true;
}

/**
 * Runs the game's own update encoder against private copies and reports its exact bit result.
 * The dirty and sent masks keep the decoded mask's native metadata but contain no dirty fields.
 */
void encode_synthetic_variant(const char* variant,
                              std::span<const std::byte, kCreateBufferSize> create,
                              std::span<const std::byte> component,
                              std::span<const std::byte, kMaskCaptureSize> mask,
                              std::int32_t dirtyBit,
                              bool publishNearby = false,
                              std::int32_t secondDirtyBit = -1) noexcept {
    alignas(16) std::array<std::byte, kSyntheticWireCapacity> wire{};
    alignas(16) std::array<std::byte, kMaskCaptureSize> dirty{};
    alignas(16) std::array<std::byte, kMaskCaptureSize> sent{};
    alignas(16) std::array<std::byte, kSyntheticContextSize> context{};
    std::copy(mask.begin(), mask.end(), dirty.begin());
    std::copy(mask.begin(), mask.end(), sent.begin());
    // The first eight bytes are the inline bits for this 64-entry mask. Preserve only its shape.
    std::fill_n(dirty.begin(), sizeof(std::uint64_t), std::byte{});
    std::fill_n(sent.begin(), sizeof(std::uint64_t), std::byte{});
    if (dirtyBit >= 0 && dirtyBit < 64) {
        const auto bit = static_cast<std::uint32_t>(dirtyBit);
        dirty[bit / 8] = static_cast<std::byte>(1U << (bit % 8));
    }
    if (secondDirtyBit >= 0 && secondDirtyBit < 64) {
        const auto bit = static_cast<std::uint32_t>(secondDirtyBit);
        dirty[bit / 8] |= static_cast<std::byte>(1U << (bit % 8));
    }

    NativeWriter writer{};
    writer.begin = wire.data();
    writer.end = wire.data() + wire.size();
    writer.cursor = wire.data();
    set_context_pointer(context, 0x08, dirty.data());
    set_context_pointer(context, 0x10, sent.data());
    set_context_pointer(context, 0x20, create.data());
    set_context_pointer(context, 0x30, component.data());
    set_context_pointer(context, 0x48, &writer);

    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectUpdateEncoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Encoder>(lease.original);
    std::uint64_t result = 0;
    bool faulted = false;
    __try {
        if (lease.accepting && call != nullptr) {
            result = call(nullptr, context.data());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    coordinator::g_callEgress();

    std::size_t flushed = 0;
    const auto begin = reinterpret_cast<std::uintptr_t>(writer.begin);
    const auto cursor = reinterpret_cast<std::uintptr_t>(writer.cursor);
    const auto end = reinterpret_cast<std::uintptr_t>(writer.end);
    if (begin != 0 && cursor >= begin && cursor <= end) {
        flushed = static_cast<std::size_t>(cursor - begin);
    }
    std::array<char, kSyntheticWireCapacity * 2 + 1> wireHex{};
    hex(std::span<const std::byte>{wire.data(), flushed}, wireHex);
    std::array<std::byte, kSyntheticWireCapacity> completeWire{};
    const std::size_t copied = flushed < completeWire.size() ? flushed : completeWire.size();
    std::copy_n(wire.begin(), copied, completeWire.begin());
    std::uint32_t pending = writer.pendingBits;
    std::size_t completeSize = copied;
    while (pending != 0 && completeSize < completeWire.size()) {
        const std::uint32_t width = pending < 8 ? pending : 8;
        const std::uint32_t shift = pending - width;
        const std::uint64_t mask = (1ULL << width) - 1ULL;
        const auto value = static_cast<std::uint8_t>((writer.accumulator >> shift) & mask);
        completeWire[completeSize++] = static_cast<std::byte>(value << (8 - width));
        pending -= width;
    }
    std::array<char, kSyntheticWireCapacity * 2 + 1> completeWireHex{};
    hex(std::span<const std::byte>{completeWire.data(), completeSize}, completeWireHex);
    std::uint32_t metadata = 0;
    std::memcpy(&metadata, mask.data() + sizeof(std::uint64_t), sizeof metadata);
    std::uint32_t rsat = 0;
    std::memcpy(&rsat, create.data(), sizeof rsat);
    const bool spatial = (std::to_integer<unsigned>(create[4]) & 1U) != 0;
    if (rsat == kSharedVandalRsat
        && !g_vandalComponentMapReported.load(std::memory_order_acquire)) {
        const std::size_t componentOffset = spatial ? kNamedComponentRsatOffset : 0;
        ComponentMapSnapshot componentMap{};
        if (componentOffset <= component.size()) {
            const ComponentMapStatus status =
                inspect_component_map(dirty.data(),
                                      sent.data(),
                                      spatial ? 3 : 0,
                                      component.data() + componentOffset,
                                      component.size() - componentOffset,
                                      componentMap);
            report_component_map("private-native-encode", status, componentMap);
        }
    }
    if (publishNearby && !faulted && result != 0 && writer.totalBits > 0
        && writer.totalBits <= static_cast<std::int32_t>(kNearbyUpdateCapacity * 8)
        && completeSize <= kNearbyUpdateCapacity) {
        NearbyUpdateCapture capture{};
        std::copy_n(completeWire.begin(), completeSize, capture.wire.begin());
        capture.rsat = rsat;
        capture.bitCount = static_cast<std::uint16_t>(writer.totalBits);
        capture.present = true;
        NearbyTransformCapture transformCapture{};
        if (component.size() >= sizeof transformCapture.transform) {
            std::memcpy(transformCapture.transform.data(),
                        component.data(),
                        sizeof transformCapture.transform);
            transformCapture.present =
                std::all_of(transformCapture.transform.begin(),
                            transformCapture.transform.end(),
                            [](float value) noexcept { return std::isfinite(value); });
            transformCapture.rsat = rsat;
        }
        AcquireSRWLockExclusive(&g_nearbyUpdateLock);
        g_nearbyUpdate = capture;
        g_nearbyTransform = transformCapture;
        ReleaseSRWLockExclusive(&g_nearbyUpdateLock);
    }
    const unsigned trailing = spatial ? 1U : 0U;
    std::array<char, 65> componentHex{};
    hex(component.first(component.size() < 32 ? component.size() : 32), componentHex);

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=sobject-native-update-probe variant=%s result=%llu fault=%u "
        "rsat=0x%08X flag=%u dirty_bit=%d/%d mask_meta=0x%08X bits=%d flushed=%d pending=%u "
        "accum=0x%016llX bytes=%zu hex=%s full_bytes=%zu full_hex=%s component0=%s",
        variant,
        static_cast<unsigned long long>(result),
        faulted ? 1U : 0U,
        rsat,
        trailing,
        static_cast<int>(dirtyBit),
        static_cast<int>(secondDirtyBit),
        metadata,
        writer.totalBits,
        writer.flushedBits,
        static_cast<unsigned>(writer.pendingBits),
        static_cast<unsigned long long>(writer.accumulator),
        flushed,
        wireHex.data(),
        completeSize,
        completeWireHex.data(),
        componentHex.data());
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         faulted ? core::log::Level::warn : core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Copies the natural local actor's live stream-residency context while its scratch is valid. */
void capture_stream_source(const UpdateSnapshot& update) noexcept {
    std::uint32_t rsat{};
    std::memcpy(&rsat, update.create.data(), sizeof rsat);
    if (rsat != kNaturalPlayerRsat || (std::to_integer<unsigned>(update.create[4]) & 1U) == 0
        || update.componentBase == 0) {
        return;
    }
    AcquireSRWLockShared(&g_streamSourceLock);
    const bool alreadyPresent = g_streamSource.present;
    ReleaseSRWLockShared(&g_streamSourceLock);
    if (alreadyPresent) {
        return;
    }
    StreamSourceCapture capture{};
    __try {
        const auto* const component = reinterpret_cast<const std::byte*>(update.componentBase);
        std::memcpy(capture.bytes.data(), component + kStreamSourceOffset, capture.bytes.size());
        capture.present = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        capture = {};
    }
    if (!capture.present) {
        return;
    }
    AcquireSRWLockExclusive(&g_streamSourceLock);
    if (!g_streamSource.present) {
        g_streamSource = capture;
    }
    ReleaseSRWLockExclusive(&g_streamSourceLock);
}

/** Copies bytes flushed by the update call when its cursor movement is bounded and readable. */
[[nodiscard]] std::size_t
capture_flushed(const WriterSnapshot& before,
                const WriterSnapshot& after,
                std::array<std::byte, kFlushedCaptureCapacity>& output) noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(before.cursor);
    const auto end = reinterpret_cast<std::uintptr_t>(after.cursor);
    if (begin == 0 || end < begin || end - begin > output.size()) {
        return 0;
    }
    const std::size_t size = static_cast<std::size_t>(end - begin);
    __try {
        if (size != 0) {
            std::memcpy(output.data(), before.cursor, size);
        }
        return size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/** Preserves native encoding and captures a bounded set of kind-0 update payloads. */
__declspec(noinline) std::uint64_t __fastcall encode_body(void* codec,
                                                          const void* contextAddress) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectUpdateEncoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Encoder>(lease.original);
    UpdateSnapshot before{};
    const bool beforeReadable = inspect_update(contextAddress, before);
    if (beforeReadable) {
        capture_stream_source(before);
    }
    std::uint64_t result{};
    __try {
        if (call != nullptr) {
            result = call(codec, contextAddress);
        }
        WriterSnapshot after{};
        const bool afterReadable = lease.accepting && inspect_writer(before.writerAddress, after);
        if (beforeReadable && afterReadable && record_once(before)) {
            std::uint32_t rsat{};
            std::memcpy(&rsat, before.create.data(), sizeof rsat);
            const unsigned trailing = std::to_integer<unsigned>(before.create[4]) & 1U;
            const std::int32_t bitDelta = after.totalBits - before.writer.totalBits;
            const bool scalarValid = bitDelta > 0 && bitDelta <= 64
                                     && before.writer.cursor == after.cursor
                                     && before.writer.flushedBits == after.flushedBits;
            std::uint64_t appended{};
            if (scalarValid) {
                appended = bitDelta == 64 ? after.accumulator
                                          : after.accumulator & ((1ULL << bitDelta) - 1ULL);
            }

            std::array<std::byte, kFlushedCaptureCapacity> flushed{};
            const std::size_t flushedSize = capture_flushed(before.writer, after, flushed);
            std::array<char, kCreateBufferSize * 2 + 1> createHex{};
            std::array<char, kMaskCaptureSize * 2 + 1> dirtyHex{};
            std::array<char, kMaskCaptureSize * 2 + 1> sentHex{};
            std::array<char, kFlushedCaptureCapacity * 2 + 1> flushedHex{};
            hex(before.create, createHex);
            hex(before.dirtyBytes, dirtyHex);
            hex(before.sentBytes, sentHex);
            hex(std::span<const std::byte>{flushed.data(), flushedSize}, flushedHex);

            std::array<char, core::log::kLineCapacity> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=sobject-update rsat=0x%08X flag=%u input=%s base=%lld "
                "diagnostics=%u dirty=%s sent=%s bits=%d "
                "before[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                "after[total=%d flushed=%d pending=%u accum=0x%016llX cursor=%p] "
                "flushed_bytes=%zu flushed_hex=%s scalar=%u append=0x%016llX",
                rsat,
                trailing,
                createHex.data(),
                static_cast<long long>(before.componentBase),
                static_cast<unsigned>(before.diagnostics),
                dirtyHex.data(),
                sentHex.data(),
                bitDelta,
                before.writer.totalBits,
                before.writer.flushedBits,
                static_cast<unsigned>(before.writer.pendingBits),
                static_cast<unsigned long long>(before.writer.accumulator),
                static_cast<const void*>(before.writer.cursor),
                after.totalBits,
                after.flushedBits,
                static_cast<unsigned>(after.pendingBits),
                static_cast<unsigned long long>(after.accumulator),
                static_cast<const void*>(after.cursor),
                flushedSize,
                flushedHex.data(),
                scalarValid ? 1U : 0U,
                static_cast<unsigned long long>(appended));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* encoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&encode_body);
}

void probe_decoded_record(std::span<const std::byte> create,
                          std::span<const std::byte> update,
                          std::span<const std::byte> mask) noexcept {
    if (create.size() != kCreateBufferSize || update.empty()
        || update.size() > kSyntheticComponentCapacity - kNamedComponentRsatOffset
        || mask.size() != kMaskCaptureSize
        || g_decodedRecordProbed.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    std::array<std::byte, kCreateBufferSize> plainCreate{};
    std::array<std::byte, kCreateBufferSize> spatialCreate{};
    std::array<std::byte, kMaskCaptureSize> nativeMask{};
    alignas(16) std::array<std::byte, kSyntheticComponentCapacity> plainComponent{};
    alignas(16) std::array<std::byte, kSyntheticComponentCapacity> spatialComponent{};
    std::copy(create.begin(), create.end(), plainCreate.begin());
    spatialCreate = plainCreate;
    std::copy(mask.begin(), mask.end(), nativeMask.begin());
    std::copy(update.begin(), update.end(), plainComponent.begin());

    // Flag zero starts the RSAT-defined scratch at component offset zero. Flag one reserves the
    // transform, parent, and stream-source regions and moves that same scratch to aligned +0x90.
    plainCreate[4] &= std::byte{0xFE};
    spatialCreate[4] |= std::byte{0x01};
    const bool incomingSpatial = (std::to_integer<unsigned>(create[4]) & 1U) != 0;
    if (!incomingSpatial) {
        std::copy(
            update.begin(), update.end(), spatialComponent.begin() + kNamedComponentRsatOffset);
        encode_synthetic_variant("plain-clean", plainCreate, plainComponent, nativeMask, -1);
        encode_synthetic_variant("spatial-clean", spatialCreate, spatialComponent, nativeMask, -1);
        return;
    }

    // A decoded spatial record already owns the complete named + RSAT scratch layout. Re-encode it
    // once clean, then privately mark only transform dirty to recover the exact native payload.
    std::copy(update.begin(), update.end(), spatialComponent.begin());
    encode_synthetic_variant("spatial-clean", spatialCreate, spatialComponent, nativeMask, -1);
    encode_synthetic_variant(
        "spatial-transform-decoded", spatialCreate, spatialComponent, nativeMask, 0);

    // Perturb each element of the transform's second float4 independently. These private calls
    // identify translation/auxiliary fields and recover exact native wire without touching the
    // object that the live decoder just accepted.
    std::array<std::byte, kSyntheticComponentCapacity> positionComponent = spatialComponent;
    constexpr std::array<const char*, 4> kPositionVariants{
        "spatial-transform-second-x1",
        "spatial-transform-second-y1",
        "spatial-transform-second-z1",
        "spatial-transform-second-w1",
    };
    for (std::size_t index = 0; index < kPositionVariants.size(); ++index) {
        positionComponent = spatialComponent;
        static_cast<void>(set_component_float(
            positionComponent, kTransformSecondFloat4Offset + index * sizeof(std::uint32_t), 1.0F));
        encode_synthetic_variant(
            kPositionVariants[index], spatialCreate, positionComponent, nativeMask, 0);
    }

    // The player-position publisher is already fed by the physics sync and protected by a
    // seqlock. Use it only as private source data for two native encodes; the accepted object and
    // its component storage are never changed here.
    const player::position::Snapshot player = player::position::snapshot();
    bool playerValid = player.present;
    positionComponent = spatialComponent;
    for (std::size_t axis = 0; axis < player.position.size(); ++axis) {
        playerValid = playerValid
                      && set_component_float(positionComponent,
                                             kTransformSecondFloat4Offset + axis * sizeof(float),
                                             player.position[axis]);
    }
    if (playerValid) {
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=gameplay stage=entity-player-position result=ok "
                                          "x=%.9g y=%.9g z=%.9g",
                                          static_cast<double>(player.position[0]),
                                          static_cast<double>(player.position[1]),
                                          static_cast<double>(player.position[2]));
        if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        encode_synthetic_variant(
            "spatial-transform-player", spatialCreate, positionComponent, nativeMask, 0);
        positionComponent = spatialComponent;
        constexpr float kNearbyOffset = 3.0F;
        for (std::size_t axis = 0; axis < player.position.size(); ++axis) {
            const float value = player.position[axis] + (axis == 0 ? kNearbyOffset : 0.0F);
            playerValid =
                playerValid
                && set_component_float(
                    positionComponent, kTransformSecondFloat4Offset + axis * sizeof(float), value);
        }
        if (playerValid) {
            encode_synthetic_variant("spatial-transform-player-x3",
                                     spatialCreate,
                                     positionComponent,
                                     nativeMask,
                                     0,
                                     true);
        }
    } else {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         "ev=gameplay stage=entity-player-position result=missing");
    }
}

/** @return True when the requested retained native update is already available. */
[[nodiscard]] bool nearby_update_present(std::uint32_t rsat) noexcept {
    AcquireSRWLockShared(&g_nearbyUpdateLock);
    const bool present = g_nearbyUpdate.present && g_nearbyUpdate.rsat == rsat
                         && g_nearbyUpdate.bitCount != 0
                         && g_nearbyUpdate.bitCount <= kNearbyUpdateCapacity * 8;
    ReleaseSRWLockShared(&g_nearbyUpdateLock);
    return present;
}

bool prime_first_entity_update(std::uint32_t rsat) noexcept {
    constexpr std::uint32_t kSharedVandalRsat = 0x815B204B;
    if (rsat != kSharedVandalRsat) {
        return false;
    }
    if (nearby_update_present(rsat)) {
        return true;
    }

    const player::position::Snapshot player = player::position::snapshot();
    if (!player.present || g_preloadAttempted.exchange(true, std::memory_order_relaxed)) {
        return false;
    }

    // Exact accepted shared-Vandal profile from the namespace-2 Basin record. The native mask's
    // inline bits are clean and +0x08 retains the observed 0x4000 schema metadata.
    constexpr std::array<std::byte, kCreateBufferSize> kCreate{
        std::byte{0x4B},
        std::byte{0x20},
        std::byte{0x5B},
        std::byte{0x81},
        std::byte{0x01},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0xAC},
        std::byte{0x22},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x5C},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
    };
    alignas(16) std::array<std::byte, kSyntheticComponentCapacity> component{};
    alignas(16) std::array<std::byte, kMaskCaptureSize> mask{};
    constexpr std::uint32_t kMaskMetadata = 0x00004000;
    std::memcpy(mask.data() + sizeof(std::uint64_t), &kMaskMetadata, sizeof kMaskMetadata);

    // The accepted zero baseline is an identity transform with translation in the second float4.
    bool valid = set_component_float(component, 0x0C, 1.0F);
    constexpr float kNearbyOffset = 3.0F;
    for (std::size_t axis = 0; axis < player.position.size(); ++axis) {
        const float value = player.position[axis] + (axis == 0 ? kNearbyOffset : 0.0F);
        valid = valid
                && set_component_float(
                    component, kTransformSecondFloat4Offset + axis * sizeof(float), value);
    }
    if (!valid) {
        return false;
    }

    StreamSourceCapture streamSource{};
    AcquireSRWLockShared(&g_streamSourceLock);
    streamSource = g_streamSource;
    ReleaseSRWLockShared(&g_streamSourceLock);
    if (streamSource.present) {
        std::copy(streamSource.bytes.begin(),
                  streamSource.bytes.end(),
                  component.begin() + kStreamSourceOffset);
    }

    encode_synthetic_variant(streamSource.present ? "spatial-transform-stream-player-x3-preload"
                                                  : "spatial-transform-player-x3-preload",
                             kCreate,
                             component,
                             mask,
                             0,
                             true,
                             streamSource.present ? 2 : -1);
    return nearby_update_present(rsat);
}

bool take_nearby_player_update(std::uint32_t rsat, NearbyUpdateCapture& output) noexcept {
    output = {};
    AcquireSRWLockExclusive(&g_nearbyUpdateLock);
    const bool present = g_nearbyUpdate.present && g_nearbyUpdate.rsat == rsat;
    if (present) {
        output = g_nearbyUpdate;
        g_nearbyUpdate = {};
    }
    ReleaseSRWLockExclusive(&g_nearbyUpdateLock);
    return present;
}

bool nearby_player_transform(std::uint32_t rsat, std::array<float, 8>& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_nearbyUpdateLock);
    const bool present = g_nearbyTransform.present && g_nearbyTransform.rsat == rsat;
    if (present) {
        output = g_nearbyTransform.transform;
    }
    ReleaseSRWLockShared(&g_nearbyUpdateLock);
    return present;
}

void reset() noexcept {
    for (std::atomic_uint64_t& seen : g_seen) {
        seen.store(0, std::memory_order_relaxed);
    }
    g_decodedRecordProbed.store(false, std::memory_order_relaxed);
    g_preloadAttempted.store(false, std::memory_order_relaxed);
    g_vandalComponentMapReported.store(false, std::memory_order_relaxed);
    g_vandalComponentMapFailureReported.store(false, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_nearbyUpdateLock);
    g_nearbyUpdate = {};
    g_nearbyTransform = {};
    ReleaseSRWLockExclusive(&g_nearbyUpdateLock);
    AcquireSRWLockExclusive(&g_streamSourceLock);
    g_streamSource = {};
    ReleaseSRWLockExclusive(&g_streamSourceLock);
}

} // namespace sunrise::client::hooks::network::sobject_update_probe
