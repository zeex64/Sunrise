#include "sobject_apply_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"
#include "sobject_bind_probe.h"

namespace sunrise::client::hooks::network::sobject_apply_probe {
namespace {

constexpr std::uint32_t kEntitySlotMask = 0x1FFF;
constexpr std::uint32_t kApplyReportLimit = 32;
constexpr std::uint32_t kKind0ReportLimit = 16;
constexpr std::uint32_t kPromotionReportLimit = 16;
constexpr std::uint32_t kDirtyServiceReportLimit = 8;
constexpr std::uint32_t kType2JobReportLimit = 16;
constexpr std::size_t kManagerSlotMapOffset = 0x114;
constexpr std::size_t kManagerSlotMapStride = 6;
constexpr std::size_t kManagerOccupiedBitsetOffset = 0xC520;
constexpr std::size_t kManagerDirtyBitsetOffset = 0xCA20;
constexpr std::int16_t kInternalObjectCapacity = 0x400;

using ApplyJob = void(__fastcall*)(std::byte*);
using Kind0Constructor = bool(__fastcall*)(void*, const std::uint32_t*, std::uint32_t, int);
using RecordPromotion = void(__fastcall*)(std::byte*, std::byte*);
using DirtyService = void(__fastcall*)(std::byte*);
using BackendBusy = bool(__fastcall*)(const std::byte*);
using Type2Job = int(__fastcall*)(std::byte*, const void*, void**);

std::atomic_uint32_t g_applyReports{};
std::atomic_uint32_t g_kind0Reports{};
std::atomic_uint32_t g_promotionReports{};
std::atomic_uint32_t g_dirtyServiceReports{};
std::atomic_uint32_t g_type2JobReports{};

struct JobSnapshot {
    std::uint8_t type{};
    std::uint8_t flags{};
    std::uint32_t entity{};
    std::uintptr_t storage{};
    bool readable{};
};

struct PromotionSnapshot {
    const std::byte* manager{};
    std::int32_t namespaceId{-1};
    std::uint32_t entity{};
    std::uint16_t cell{};
    std::uint16_t wireFlags{};
    std::uint16_t internalFlags{};
    std::uint8_t objectGeneration{};
    bool occupied{};
    bool readable{};
};

struct DirtyServiceSnapshot {
    const std::byte* manager{};
    std::int32_t namespaceId{-1};
    std::uint32_t entity{};
    std::int16_t internalIndex{-1};
    bool mapped{};
    bool dirty{};
    bool readable{};
};

struct BackendBusyTrace {
    const std::byte* context{};
    std::int32_t count{-1};
    bool result{};
    bool readable{};
    bool seen{};
    bool armed{};
};

thread_local BackendBusyTrace g_backendBusyTrace{};

struct Type2JobSnapshot {
    const std::byte* object{};
    const void* masks{};
    void** jobOut{};
    void* job{};
    std::uint32_t handle{};
    std::uint32_t entity{};
    std::uint16_t flags{};
    std::uint8_t kind{};
    bool jobOutReadable{};
    bool readable{};
};

/** Reads only the fields consumed by the type-2 apply dispatcher. */
[[nodiscard]] bool inspect_job(const std::byte* job, JobSnapshot& output) noexcept {
    output = {};
    if (job == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&output.type, job, sizeof output.type);
        std::memcpy(&output.flags, job + 1, sizeof output.flags);
        std::memcpy(&output.entity, job + 4, sizeof output.entity);
        std::memcpy(&output.storage, job + 8, sizeof output.storage);
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Reads the fields consumed by the immediate decoded-record promotion without changing them. */
[[nodiscard]] bool inspect_promotion(const std::byte* manager,
                                     const std::byte* record,
                                     PromotionSnapshot& output) noexcept {
    output = {};
    output.namespaceId = -1;
    if (manager == nullptr || record == nullptr) {
        return false;
    }
    __try {
        output.manager = manager;
        const std::byte* provider = nullptr;
        std::memcpy(&provider, manager + 8, sizeof provider);
        if (provider != nullptr) {
            std::memcpy(&output.namespaceId, provider + 8, sizeof output.namespaceId);
        }
        std::memcpy(&output.cell, record + 0x02, sizeof output.cell);
        std::memcpy(&output.entity, record + 0x08, sizeof output.entity);
        std::memcpy(&output.wireFlags, record + 0x40, sizeof output.wireFlags);
        std::memcpy(&output.internalFlags, record + 0x42, sizeof output.internalFlags);
        std::memcpy(&output.objectGeneration, record + 0x44, sizeof output.objectGeneration);
        const std::size_t slot = output.entity & kEntitySlotMask;
        std::uint32_t occupiedWord = 0;
        std::memcpy(&occupiedWord,
                    manager + kManagerOccupiedBitsetOffset + (slot >> 5U) * sizeof occupiedWord,
                    sizeof occupiedWord);
        output.occupied = (occupiedWord & (1U << (slot & 31U))) != 0;
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.namespaceId = -1;
        return false;
    }
}

/** Reads the manager namespace without retaining the provider pointer. */
[[nodiscard]] bool inspect_manager_namespace(const std::byte* manager,
                                             std::int32_t& namespaceId) noexcept {
    namespaceId = -1;
    if (manager == nullptr) {
        return false;
    }
    __try {
        const std::byte* provider = nullptr;
        std::memcpy(&provider, manager + 8, sizeof provider);
        if (provider == nullptr) {
            return false;
        }
        std::memcpy(&namespaceId, provider + 8, sizeof namespaceId);
        return namespaceId >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        namespaceId = -1;
        return false;
    }
}

/** Reads one watched slot's internal mapping and the dirty bit consumed by the tick service. */
[[nodiscard]] bool inspect_dirty_service(const std::byte* manager,
                                         std::int32_t namespaceId,
                                         std::uint32_t entity,
                                         DirtyServiceSnapshot& output) noexcept {
    output = {};
    output.namespaceId = -1;
    if (manager == nullptr || namespaceId < 0) {
        return false;
    }
    __try {
        output.manager = manager;
        output.namespaceId = namespaceId;
        output.entity = entity;
        const std::size_t slot = entity & kEntitySlotMask;
        std::memcpy(&output.internalIndex,
                    manager + kManagerSlotMapOffset + slot * kManagerSlotMapStride,
                    sizeof output.internalIndex);
        output.mapped = output.internalIndex >= 0 && output.internalIndex < kInternalObjectCapacity;
        if (output.mapped) {
            const auto internal = static_cast<std::uint16_t>(output.internalIndex);
            std::uint32_t dirtyWord = 0;
            std::memcpy(&dirtyWord,
                        manager + kManagerDirtyBitsetOffset + (internal >> 5U) * sizeof dirtyWord,
                        sizeof dirtyWord);
            output.dirty = (dirtyWord & (1U << (internal & 31U))) != 0;
        }
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.namespaceId = -1;
        return false;
    }
}

/** Reads the stable replicated-object header and job-out pointer without dereferencing the job. */
[[nodiscard]] bool inspect_type2_job(const std::byte* object,
                                     const void* masks,
                                     void** jobOut,
                                     Type2JobSnapshot& output) noexcept {
    output = {};
    if (object == nullptr) {
        return false;
    }
    __try {
        output.object = object;
        output.masks = masks;
        output.jobOut = jobOut;
        std::memcpy(&output.kind, object, sizeof output.kind);
        std::memcpy(&output.handle, object + 4, sizeof output.handle);
        std::memcpy(&output.entity, object + 8, sizeof output.entity);
        std::memcpy(&output.flags, object + 0x68, sizeof output.flags);
        if (jobOut != nullptr) {
            std::memcpy(&output.job, jobOut, sizeof output.job);
            output.jobOutReadable = true;
        }
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Reads only the returned job pointer value; the allocated job remains entirely opaque. */
[[nodiscard]] bool inspect_job_pointer(void** jobOut, void*& job) noexcept {
    job = nullptr;
    if (jobOut == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&job, jobOut, sizeof job);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job = nullptr;
        return false;
    }
}

/** Reports one watched job boundary without changing its dispatch or result. */
void report_apply(const char* phase,
                  std::uint32_t occurrence,
                  const JobSnapshot& before,
                  const JobSnapshot& after) noexcept {
    std::array<char, 320> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=gameplay stage=sobject-apply phase=%s occurrence=%u "
                                      "entity=0x%08X slot=%u type=%u->%u flags=0x%02X->0x%02X "
                                      "storage=%p after_readable=%u",
                                      phase,
                                      occurrence,
                                      before.entity,
                                      before.entity & kEntitySlotMask,
                                      before.type,
                                      after.type,
                                      before.flags,
                                      after.flags,
                                      reinterpret_cast<void*>(before.storage),
                                      after.readable ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports one watched dirty-service boundary without walking unrelated manager slots. */
void report_dirty_service(const char* phase,
                          std::uint32_t occurrence,
                          const DirtyServiceSnapshot& before,
                          const DirtyServiceSnapshot& current,
                          const BackendBusyTrace& busy) noexcept {
    std::array<char, 512> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=sobject-dirty-service phase=%s occurrence=%u "
                      "manager=%p namespace=%d entity=0x%08X slot=%u "
                      "internal=%d->%d mapped=%u->%u dirty=%u->%u readable=%u "
                      "backend_seen=%u backend_context=%p backend_count=%d backend_busy=%u",
                      phase,
                      occurrence,
                      static_cast<const void*>(before.manager),
                      before.namespaceId,
                      before.entity,
                      before.entity & kEntitySlotMask,
                      static_cast<int>(before.internalIndex),
                      static_cast<int>(current.internalIndex),
                      before.mapped ? 1U : 0U,
                      current.mapped ? 1U : 0U,
                      before.dirty ? 1U : 0U,
                      current.dirty ? 1U : 0U,
                      current.readable ? 1U : 0U,
                      busy.seen ? 1U : 0U,
                      static_cast<const void*>(busy.context),
                      busy.readable ? busy.count : -1,
                      busy.seen && busy.result ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports one watched type-2 job allocation without inspecting the allocated job. */
void report_type2_job(const char* phase,
                      std::uint32_t occurrence,
                      const Type2JobSnapshot& snapshot,
                      int result,
                      bool returnedJob,
                      bool jobOutReadable) noexcept {
    std::array<char, 384> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=gameplay stage=sobject-type2-job phase=%s occurrence=%u "
                                      "object=%p masks=%p job_out=%p kind=%u handle=0x%08X "
                                      "entity=0x%08X slot=%u flags=0x%04X result=%d "
                                      "job_before=%u job_returned=%u job_out_readable=%u",
                                      phase,
                                      occurrence,
                                      static_cast<const void*>(snapshot.object),
                                      snapshot.masks,
                                      static_cast<void*>(snapshot.jobOut),
                                      static_cast<unsigned>(snapshot.kind),
                                      snapshot.handle,
                                      snapshot.entity,
                                      snapshot.entity & kEntitySlotMask,
                                      static_cast<unsigned>(snapshot.flags),
                                      result,
                                      snapshot.job != nullptr ? 1U : 0U,
                                      returnedJob ? 1U : 0U,
                                      jobOutReadable ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Preserves the async apply job and exposes whether a watched record reaches it. */
__declspec(noinline) void __fastcall apply_body(std::byte* job) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::sobjectApplyJob, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ApplyJob>(lease.original);
    __try {
        JobSnapshot before{};
        const bool watched = lease.accepting && inspect_job(job, before)
                             && sobject_bind_probe::watched(before.entity);
        const std::uint32_t occurrence =
            watched ? g_applyReports.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (call != nullptr) {
            call(job);
        }
        if (occurrence != 0 && occurrence <= kApplyReportLimit) {
            JobSnapshot after{};
            (void)inspect_job(job, after);
            report_apply("return", occurrence, before, after);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves kind-0 construction and reports its definitive return value for watched slots. */
__declspec(noinline) bool __fastcall
kind0_body(void* codec, const std::uint32_t* creation, std::uint32_t entity, int parent) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectKind0Constructor, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Kind0Constructor>(lease.original);
    bool result = false;
    __try {
        const bool watched = lease.accepting && sobject_bind_probe::watched(entity);
        std::uint32_t rsat = 0;
        bool readable = false;
        if (watched && creation != nullptr) {
            __try {
                std::memcpy(&rsat, creation, sizeof rsat);
                readable = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                rsat = 0;
            }
        }
        if (call != nullptr) {
            result = call(codec, creation, entity, parent);
        }
        const std::uint32_t occurrence =
            watched ? g_kind0Reports.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (occurrence != 0 && occurrence <= kKind0ReportLimit) {
            std::array<char, 256> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=gameplay stage=sobject-kind0 occurrence=%u "
                                              "entity=0x%08X slot=%u rsat_readable=%u "
                                              "rsat=0x%08X parent=%d result=%u",
                                              occurrence,
                                              entity,
                                              entity & kEntitySlotMask,
                                              readable ? 1U : 0U,
                                              rsat,
                                              parent,
                                              result ? 1U : 0U);
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
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

/** Preserves immediate decoded-record promotion and reports its exact occupied-bit postcondition.
 */
__declspec(noinline) void __fastcall promotion_body(std::byte* manager,
                                                    std::byte* record) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectRecordPromotion, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<RecordPromotion>(lease.original);
    __try {
        PromotionSnapshot before{};
        const bool watched = lease.accepting && inspect_promotion(manager, record, before)
                             && sobject_bind_probe::watched(before.namespaceId, before.entity);
        const std::uint32_t occurrence =
            watched ? g_promotionReports.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (call != nullptr) {
            call(manager, record);
        }
        if (occurrence != 0 && occurrence <= kPromotionReportLimit) {
            PromotionSnapshot after{};
            const bool afterReadable = inspect_promotion(manager, record, after);
            std::array<char, 384> line{};
            const int written =
                std::snprintf(line.data(),
                              line.size(),
                              "ev=gameplay stage=sobject-promote phase=return occurrence=%u "
                              "manager=%p namespace=%d entity=0x%08X slot=%u cell=0x%04X "
                              "wire_flags=0x%04X internal_flags=0x%04X ogen=%u "
                              "occupied=%u->%u after_readable=%u",
                              occurrence,
                              static_cast<const void*>(before.manager),
                              before.namespaceId,
                              before.entity,
                              before.entity & kEntitySlotMask,
                              static_cast<unsigned>(before.cell),
                              static_cast<unsigned>(before.wireFlags),
                              static_cast<unsigned>(before.internalFlags),
                              static_cast<unsigned>(before.objectGeneration),
                              before.occupied ? 1U : 0U,
                              afterReadable && after.occupied ? 1U : 0U,
                              afterReadable ? 1U : 0U);
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves per-tick dirty service and reports the watched slot's mapping at both boundaries. */
__declspec(noinline) void __fastcall dirty_service_body(std::byte* manager) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectDirtyService, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<DirtyService>(lease.original);
    __try {
        std::int32_t namespaceId = -1;
        std::uint32_t entity = 0;
        DirtyServiceSnapshot before{};
        const bool watched = lease.accepting && inspect_manager_namespace(manager, namespaceId)
                             && sobject_bind_probe::first_watched(namespaceId, entity)
                             && inspect_dirty_service(manager, namespaceId, entity, before)
                             && sobject_bind_probe::watched(namespaceId, entity);
        const std::uint32_t occurrence =
            watched ? g_dirtyServiceReports.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (occurrence != 0 && occurrence <= kDirtyServiceReportLimit) {
            report_dirty_service("entry", occurrence, before, before, {});
        }
        const BackendBusyTrace previousTrace = g_backendBusyTrace;
        g_backendBusyTrace = {};
        g_backendBusyTrace.armed = occurrence != 0 && occurrence <= kDirtyServiceReportLimit;
        if (call != nullptr) {
            call(manager);
        }
        const BackendBusyTrace busy = g_backendBusyTrace;
        g_backendBusyTrace = previousTrace;
        if (occurrence != 0 && occurrence <= kDirtyServiceReportLimit) {
            DirtyServiceSnapshot after{};
            (void)inspect_dirty_service(manager, namespaceId, entity, after);
            report_dirty_service("return", occurrence, before, after, busy);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves the dirty-service suppression predicate and records only an armed watched call. */
__declspec(noinline) bool __fastcall backend_busy_body(const std::byte* context) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectBackendBusy, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<BackendBusy>(lease.original);
    bool result = false;
    __try {
        std::int32_t count = -1;
        bool readable = false;
        if (lease.accepting && g_backendBusyTrace.armed && context != nullptr) {
            __try {
                std::memcpy(&count, context + 0x560E4, sizeof count);
                readable = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                count = -1;
            }
        }
        if (call != nullptr) {
            result = call(context);
        }
        if (lease.accepting && g_backendBusyTrace.armed) {
            g_backendBusyTrace.context = context;
            g_backendBusyTrace.count = count;
            g_backendBusyTrace.result = result;
            g_backendBusyTrace.readable = readable;
            g_backendBusyTrace.seen = true;
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves type-2 serialization/allocation and never dereferences the returned job. */
__declspec(noinline) int __fastcall
type2_job_body(std::byte* object, const void* masks, void** jobOut) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::sobjectType2Job, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Type2Job>(lease.original);
    int result = 0;
    __try {
        Type2JobSnapshot snapshot{};
        const bool watched = lease.accepting && inspect_type2_job(object, masks, jobOut, snapshot)
                             && sobject_bind_probe::watched_exact(snapshot.entity);
        const std::uint32_t occurrence =
            watched ? g_type2JobReports.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (occurrence != 0 && occurrence <= kType2JobReportLimit) {
            report_type2_job("entry",
                             occurrence,
                             snapshot,
                             -1,
                             snapshot.job != nullptr,
                             snapshot.jobOutReadable);
        }
        if (call != nullptr) {
            result = call(object, masks, jobOut);
        }
        if (occurrence != 0 && occurrence <= kType2JobReportLimit) {
            void* returnedJob = nullptr;
            const bool jobOutReadable = inspect_job_pointer(jobOut, returnedJob);
            report_type2_job("return",
                             occurrence,
                             snapshot,
                             result,
                             jobOutReadable && returnedJob != nullptr,
                             jobOutReadable);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* apply_entry_point() noexcept {
    return reinterpret_cast<void*>(&apply_body);
}

void* kind0_entry_point() noexcept {
    return reinterpret_cast<void*>(&kind0_body);
}

void* promotion_entry_point() noexcept {
    return reinterpret_cast<void*>(&promotion_body);
}

void* dirty_service_entry_point() noexcept {
    return reinterpret_cast<void*>(&dirty_service_body);
}

void* backend_busy_entry_point() noexcept {
    return reinterpret_cast<void*>(&backend_busy_body);
}

void* type2_job_entry_point() noexcept {
    return reinterpret_cast<void*>(&type2_job_body);
}

void reset() noexcept {
    g_applyReports.store(0, std::memory_order_relaxed);
    g_kind0Reports.store(0, std::memory_order_relaxed);
    g_promotionReports.store(0, std::memory_order_relaxed);
    g_dirtyServiceReports.store(0, std::memory_order_relaxed);
    g_type2JobReports.store(0, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::network::sobject_apply_probe
