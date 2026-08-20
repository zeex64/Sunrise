#include "sobject_apply_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../../server/gameplay/group/group_host_sessions.h"
#include "../../../server/gameplay/peer/peer_transport.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../targets/game.h"
#include "../bootflow/bootflow_hook_lifecycle.h"
#include "coordinator/network_call_coordinator.h"
#include "entity_slot_probe.h"
#include "platform.h"
#include "sobject_bind_probe.h"

namespace sunrise::client::hooks::network::sobject_apply_probe {
namespace {

constexpr std::uint32_t kEntitySlotMask = 0x1FFF;
constexpr std::uint32_t kApplyReportLimit = 32;
constexpr std::uint32_t kKind0ReportLimit = 16;
constexpr std::uint32_t kPromotionReportLimit = 16;
constexpr std::uint32_t kDirtyServiceReportLimit = 8;
constexpr std::uint32_t kDirtyRowReportLimit = 8;
constexpr std::uint32_t kType2JobReportLimit = 16;
constexpr std::uint32_t kActiveManagerSelectionReportLimit = 8;
constexpr std::uint32_t kCitizenSessionReportLimit = 64;
constexpr std::uint32_t kCitizenJoinReportLimit = 32;
constexpr std::uint32_t kZLegReportLimit = 64;
constexpr std::size_t kCitizenSessionCapacity = 8;
constexpr std::size_t kCitizenJoinCapacity = 4;
constexpr std::uint64_t kCitizenJoinHeartbeatMilliseconds = 5000;
constexpr std::uint64_t kZLegHeartbeatMilliseconds = 5000;
constexpr std::uint64_t kZLegFreshMilliseconds = 1500;
constexpr std::size_t kManagerSlotMapOffset = 0x114;
constexpr std::size_t kManagerSlotMapStride = 6;
constexpr std::size_t kManagerOccupiedBitsetOffset = 0xC520;
constexpr std::size_t kManagerDirtyBitsetOffset = 0xCA20;
constexpr std::int16_t kInternalObjectCapacity = 0x400;
constexpr std::size_t kInternalObjectStride = 0x70;
/** Runtime storage holding the selected simulation-manager identity. */
constexpr std::size_t kRuntimeActiveManagerOffset = 0x560E0;
/** First of the runtime's three fixed-stride simulation-manager containers. */
constexpr std::size_t kRuntimeManagerContainerOffset = 0x206C8;
/** Distance between adjacent simulation-manager containers. */
constexpr std::size_t kRuntimeManagerStride = 0x11E08;
/** Replicated-object manager embedded in each simulation-manager container. */
constexpr std::size_t kContainerObjectManagerOffset = 0x270;
/** Active-manager observations expire before they may authorize an entity send. */
constexpr std::uint64_t kActiveManagerFreshMilliseconds = 500;
constexpr std::int32_t kSimulationManagerCount = 3;
/** Fields read by FUN_141788810 and the world-controller acceptance check that calls it. */
constexpr std::size_t kCitizenRoleOffset = 0x854;
constexpr std::size_t kCitizenReadyCountOffset = 0x86C;
constexpr std::size_t kCitizenSequenceOffset = 0x87C;
constexpr std::size_t kCitizenSelectedPeerOffset = 0xE93C;
constexpr std::size_t kCitizenLifecycleOffset = 0x1AEF8;
constexpr std::size_t kCitizenPeerRowsOffset = 0x1FB8;
constexpr std::size_t kCitizenPeerRowStride = 0x120;
constexpr std::int32_t kCitizenPeerCapacity = 32;

using ApplyJob = void(__fastcall*)(std::byte*);
using Kind0Constructor = bool(__fastcall*)(void*, const std::uint32_t*, std::uint32_t, int);
using RecordPromotion = void(__fastcall*)(std::byte*, std::byte*);
using DirtyService = void(__fastcall*)(std::byte*);
using BackendBusy = bool(__fastcall*)(const std::byte*);
using DirtyRow = std::uint8_t(__fastcall*)(std::byte*,
                                           std::uint16_t,
                                           int*,
                                           std::uint8_t,
                                           std::uint8_t,
                                           std::uint8_t*,
                                           std::uint8_t*,
                                           int*,
                                           std::uint8_t,
                                           std::uint8_t);
using Type2Job = int(__fastcall*)(std::byte*, const void*, void**);
using ActiveManagerRefresh = void(__fastcall*)(std::byte*);
using CitizenSessionReady = bool(__fastcall*)(const std::byte*);
using CitizenJoinStatus = std::int32_t(__fastcall*)(std::int32_t, std::uint64_t);
using ZLegState = void(__fastcall*)(std::byte*, std::int32_t);

std::atomic_uint32_t g_applyReports{};
std::atomic_uint32_t g_kind0Reports{};
std::atomic_uint32_t g_promotionReports{};
std::atomic_uint32_t g_dirtyServiceReports{};
std::atomic_uint32_t g_dirtyRowReports{};
std::atomic_uint32_t g_type2JobReports{};
std::atomic_uint32_t g_activeManagerSelectionReports{};
std::atomic_uint32_t g_citizenSessionReports{};
std::atomic_uint32_t g_citizenJoinReports{};
std::atomic_uint32_t g_zLegReports{};
std::atomic_uint64_t g_lastActiveManagerSelection{};
std::atomic_uintptr_t g_runtime{};
SRWLOCK g_activeManagerLock{SRWLOCK_INIT};
ActiveManagerDebugSnapshot g_activeManagerDebug{};
SRWLOCK g_citizenSessionLock{SRWLOCK_INIT};
SRWLOCK g_citizenJoinLock{SRWLOCK_INIT};
SRWLOCK g_zLegLock{SRWLOCK_INIT};
ZLegDebugSnapshot g_zLegDebug{};
std::uint64_t g_zLegReportedAt{};
std::uint64_t g_zLegReportSignature{};

struct CitizenSessionObservation {
    const std::byte* session{};
    std::uint64_t signature{};
};

std::array<CitizenSessionObservation, kCitizenSessionCapacity> g_citizenSessions{};

struct CitizenJoinObservation {
    std::uint64_t handle{};
    std::uint64_t reportedAt{};
    std::int32_t state{-1};
    bool occupied{};
};

std::array<CitizenJoinObservation, kCitizenJoinCapacity> g_citizenJoins{};

struct CitizenSessionSnapshot {
    const std::byte* session{};
    std::int32_t role{-1};
    std::int32_t readyCount{-1};
    std::int32_t sequence{-1};
    std::int32_t selectedPeer{-1};
    std::int32_t selectedState{-1};
    std::int32_t lifecycle{-1};
    bool result{};
    bool readable{};
};

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

struct DirtyRowSnapshot {
    const std::byte* manager{};
    const std::byte* object{};
    std::int32_t namespaceId{-1};
    std::uint16_t internalIndex{};
    std::uint8_t kind{};
    std::uint8_t state{};
    std::uint16_t cell{};
    std::uint32_t handle{};
    std::uint32_t entity{};
    std::uint32_t parent{};
    std::uintptr_t creation{};
    std::uint8_t controlFlags{};
    std::uint64_t deferUntil{};
    std::uint16_t dirtyFlags{};
    std::uint16_t retryState{};
    int batchCount{};
    int countOut{};
    std::uint8_t stateOut{};
    std::uint8_t continueOut{};
    bool batchReadable{};
    bool countOutReadable{};
    bool stateOutReadable{};
    bool continueOutReadable{};
    bool readable{};
};

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

/** Reads optional row output values without changing caller-owned storage. */
[[nodiscard]] bool inspect_optional_byte(const std::uint8_t* address,
                                         std::uint8_t& value) noexcept {
    value = 0;
    if (address == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&value, address, sizeof value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

/** Reads an optional integer row output without changing caller-owned storage. */
[[nodiscard]] bool inspect_optional_int(const int* address, int& value) noexcept {
    value = 0;
    if (address == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&value, address, sizeof value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

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

/** Reads the watched internal object and the inputs/outputs used by its dirty-row processor. */
[[nodiscard]] bool inspect_dirty_row(const std::byte* manager,
                                     std::int32_t namespaceId,
                                     std::uint16_t internalIndex,
                                     const int* batch,
                                     const std::uint8_t* stateOut,
                                     const std::uint8_t* continueOut,
                                     const int* countOut,
                                     DirtyRowSnapshot& output) noexcept {
    output = {};
    output.namespaceId = -1;
    const targets::game::network::Targets& resolved = targets::game::network::get();
    if (manager == nullptr || namespaceId < 0 || internalIndex >= kInternalObjectCapacity
        || resolved.sobjectObjectTable == nullptr) {
        return false;
    }
    __try {
        output.manager = manager;
        output.namespaceId = namespaceId;
        output.internalIndex = internalIndex;
        output.object = resolved.sobjectObjectTable
                        + static_cast<std::size_t>(internalIndex) * kInternalObjectStride;
        std::memcpy(&output.kind, output.object, sizeof output.kind);
        std::memcpy(&output.state, output.object + 1, sizeof output.state);
        std::memcpy(&output.cell, output.object + 2, sizeof output.cell);
        std::memcpy(&output.handle, output.object + 4, sizeof output.handle);
        std::memcpy(&output.entity, output.object + 8, sizeof output.entity);
        std::memcpy(&output.parent, output.object + 0x0C, sizeof output.parent);
        std::memcpy(&output.creation, output.object + 0x30, sizeof output.creation);
        std::memcpy(&output.controlFlags, output.object + 0x50, sizeof output.controlFlags);
        std::memcpy(&output.deferUntil, output.object + 0x58, sizeof output.deferUntil);
        std::memcpy(&output.dirtyFlags, output.object + 0x68, sizeof output.dirtyFlags);
        std::memcpy(&output.retryState, output.object + 0x6A, sizeof output.retryState);
        output.readable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.namespaceId = -1;
        return false;
    }
    output.batchReadable = inspect_optional_int(batch, output.batchCount);
    output.countOutReadable = inspect_optional_int(countOut, output.countOut);
    output.stateOutReadable = inspect_optional_byte(stateOut, output.stateOut);
    output.continueOutReadable = inspect_optional_byte(continueOut, output.continueOut);
    return true;
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

/** Reports why one watched dirty row was retained before reaching the type-2 builder. */
void report_dirty_row(std::uint32_t occurrence,
                      const DirtyRowSnapshot& before,
                      const DirtyRowSnapshot& after,
                      std::uint8_t argumentState,
                      std::uint8_t reservedArgument,
                      std::uint8_t suppressCreate,
                      std::uint8_t finalPass,
                      std::uint8_t result) noexcept {
    std::array<char, 1024> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=sobject-dirty-row phase=return occurrence=%u "
        "manager=%p namespace=%d internal=%u object=%p entity=0x%08X slot=%u "
        "kind=%u state=%u cell=0x%04X handle=0x%08X parent=0x%08X creation=%p "
        "control=0x%02X->0x%02X dirty_flags=0x%04X->0x%04X "
        "defer=%llu->%llu retry=0x%04X->0x%04X batch=%d->%d batch_readable=%u->%u "
        "state_out=%u->%u state_readable=%u->%u continue_out=%u->%u "
        "continue_readable=%u->%u count_out=%d->%d count_readable=%u->%u "
        "arg_state=%u reserved=%u suppress_create=%u final=%u result=%u after_readable=%u",
        occurrence,
        static_cast<const void*>(before.manager),
        before.namespaceId,
        static_cast<unsigned>(before.internalIndex),
        static_cast<const void*>(before.object),
        before.entity,
        before.entity & kEntitySlotMask,
        static_cast<unsigned>(before.kind),
        static_cast<unsigned>(before.state),
        static_cast<unsigned>(before.cell),
        before.handle,
        before.parent,
        reinterpret_cast<void*>(before.creation),
        static_cast<unsigned>(before.controlFlags),
        static_cast<unsigned>(after.controlFlags),
        static_cast<unsigned>(before.dirtyFlags),
        static_cast<unsigned>(after.dirtyFlags),
        static_cast<unsigned long long>(before.deferUntil),
        static_cast<unsigned long long>(after.deferUntil),
        static_cast<unsigned>(before.retryState),
        static_cast<unsigned>(after.retryState),
        before.batchCount,
        after.batchCount,
        before.batchReadable ? 1U : 0U,
        after.batchReadable ? 1U : 0U,
        static_cast<unsigned>(before.stateOut),
        static_cast<unsigned>(after.stateOut),
        before.stateOutReadable ? 1U : 0U,
        after.stateOutReadable ? 1U : 0U,
        static_cast<unsigned>(before.continueOut),
        static_cast<unsigned>(after.continueOut),
        before.continueOutReadable ? 1U : 0U,
        after.continueOutReadable ? 1U : 0U,
        before.countOut,
        after.countOut,
        before.countOutReadable ? 1U : 0U,
        after.countOutReadable ? 1U : 0U,
        static_cast<unsigned>(argumentState),
        static_cast<unsigned>(reservedArgument),
        static_cast<unsigned>(suppressCreate),
        static_cast<unsigned>(finalPass),
        static_cast<unsigned>(result),
        after.readable ? 1U : 0U);
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

/** Reads only the public-session fields used by the citizen-join acceptance path. */
[[nodiscard]] bool inspect_citizen_session(const std::byte* session,
                                           bool result,
                                           CitizenSessionSnapshot& output) noexcept {
    output = {};
    output.session = session;
    output.role = -1;
    output.readyCount = -1;
    output.sequence = -1;
    output.selectedPeer = -1;
    output.selectedState = -1;
    output.lifecycle = -1;
    output.result = result;
    if (session == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&output.role, session + kCitizenRoleOffset, sizeof output.role);
        std::memcpy(
            &output.readyCount, session + kCitizenReadyCountOffset, sizeof output.readyCount);
        std::memcpy(&output.sequence, session + kCitizenSequenceOffset, sizeof output.sequence);
        std::memcpy(
            &output.selectedPeer, session + kCitizenSelectedPeerOffset, sizeof output.selectedPeer);
        std::memcpy(&output.lifecycle, session + kCitizenLifecycleOffset, sizeof output.lifecycle);
        if (output.selectedPeer >= 0 && output.selectedPeer < kCitizenPeerCapacity) {
            const std::size_t peerOffset =
                kCitizenPeerRowsOffset
                + static_cast<std::size_t>(output.selectedPeer) * kCitizenPeerRowStride;
            std::memcpy(&output.selectedState, session + peerOffset, sizeof output.selectedState);
        }
        output.readable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.readable = false;
    }
    return output.readable;
}

/** @return Stable key for one acceptance-relevant public-session state. */
[[nodiscard]] std::uint64_t
citizen_session_signature(const CitizenSessionSnapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        for (std::size_t index = 0; index < sizeof value; ++index) {
            hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
            hash *= 1099511628211ULL;
        }
    };
    mix(static_cast<std::uint32_t>(snapshot.role));
    mix(static_cast<std::uint32_t>(snapshot.readyCount));
    mix(static_cast<std::uint32_t>(snapshot.sequence));
    mix(static_cast<std::uint32_t>(snapshot.selectedPeer));
    mix(static_cast<std::uint32_t>(snapshot.selectedState));
    mix(static_cast<std::uint32_t>(snapshot.lifecycle));
    mix(snapshot.result ? 1U : 0U);
    return hash == 0 ? 1 : hash;
}

/** Claims one bounded report when a session's acceptance-relevant state changes. */
[[nodiscard]] bool claim_citizen_session_report(const CitizenSessionSnapshot& snapshot,
                                                std::uint32_t& occurrence) noexcept {
    occurrence = 0;
    if (g_citizenSessionReports.load(std::memory_order_relaxed) >= kCitizenSessionReportLimit
        || !TryAcquireSRWLockExclusive(&g_citizenSessionLock)) {
        return false;
    }
    const std::uint64_t signature = citizen_session_signature(snapshot);
    CitizenSessionObservation* selected = nullptr;
    CitizenSessionObservation* empty = nullptr;
    for (CitizenSessionObservation& entry : g_citizenSessions) {
        if (entry.session == snapshot.session) {
            selected = &entry;
            break;
        }
        if (entry.session == nullptr && empty == nullptr) {
            empty = &entry;
        }
    }
    if (selected == nullptr) {
        selected = empty;
    }
    const bool changed = selected != nullptr && selected->signature != signature;
    if (changed) {
        selected->session = snapshot.session;
        selected->signature = signature;
    }
    ReleaseSRWLockExclusive(&g_citizenSessionLock);
    if (!changed) {
        return false;
    }
    occurrence = g_citizenSessionReports.fetch_add(1, std::memory_order_relaxed) + 1;
    return occurrence <= kCitizenSessionReportLimit;
}

/** Reports the exact two gates immediately before a citizen join may be accepted. */
void report_citizen_session(const CitizenSessionSnapshot& snapshot,
                            std::uint32_t occurrence) noexcept {
    std::array<char, 384> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=citizen-acceptance occurrence=%u session=%p "
                      "role_field=%d ready_count=%d sequence=%d selected=%d "
                      "selected_state=%d lifecycle=%d initialized=%u peer_ready=%u result=%u",
                      occurrence,
                      static_cast<const void*>(snapshot.session),
                      snapshot.role,
                      snapshot.readyCount,
                      snapshot.sequence,
                      snapshot.selectedPeer,
                      snapshot.selectedState,
                      snapshot.lifecycle,
                      snapshot.readyCount != 0 ? 1U : 0U,
                      snapshot.selectedState == 10 ? 1U : 0U,
                      snapshot.result ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Claims a bounded state-change or heartbeat report for one asynchronous citizen join. */
[[nodiscard]] bool claim_citizen_join_report(std::uint64_t handle,
                                             std::int32_t state,
                                             std::uint32_t& occurrence) noexcept {
    occurrence = 0;
    if (g_citizenJoinReports.load(std::memory_order_relaxed) >= kCitizenJoinReportLimit
        || !TryAcquireSRWLockExclusive(&g_citizenJoinLock)) {
        return false;
    }
    const std::uint64_t now = GetTickCount64();
    CitizenJoinObservation* selected = nullptr;
    CitizenJoinObservation* oldest = &g_citizenJoins.front();
    for (CitizenJoinObservation& entry : g_citizenJoins) {
        if (entry.occupied && entry.handle == handle) {
            selected = &entry;
            break;
        }
        if (!entry.occupied) {
            selected = &entry;
            break;
        }
        if (entry.reportedAt < oldest->reportedAt) {
            oldest = &entry;
        }
    }
    if (selected == nullptr) {
        selected = oldest;
    }
    const bool due = !selected->occupied || selected->handle != handle || selected->state != state
                     || now - selected->reportedAt >= kCitizenJoinHeartbeatMilliseconds;
    if (due) {
        selected->occupied = true;
        selected->handle = handle;
        selected->state = state;
        selected->reportedAt = now;
    }
    ReleaseSRWLockExclusive(&g_citizenJoinLock);
    if (!due) {
        return false;
    }
    occurrence = g_citizenJoinReports.fetch_add(1, std::memory_order_relaxed) + 1;
    return occurrence <= kCitizenJoinReportLimit;
}

/** Reports the outer world-controller gate that must return one before citizen acceptance. */
void report_citizen_join(std::uint64_t handle,
                         std::int32_t state,
                         std::uint32_t occurrence) noexcept {
    std::array<char, 224> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=gameplay stage=citizen-join-status occurrence=%u kind=2 "
                                      "handle=0x%016llX state=%d ready=%u",
                                      occurrence,
                                      static_cast<unsigned long long>(handle),
                                      state,
                                      state == 1 ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reads only fields used by the native positional normal-z-leg classifier. */
[[nodiscard]] bool inspect_z_leg(const std::byte* controller, ZLegDebugSnapshot& output) noexcept {
    output = {};
    output.controller = reinterpret_cast<std::uintptr_t>(controller);
    output.requestedState = -1;
    output.storedBefore = -1;
    output.storedAfter = -1;
    output.transitionMode = -1;
    output.targetRegion = -1;
    output.regionA = -1;
    output.regionB = -1;
    output.authoredRegion = -1;
    output.entryIndex = -1;
    output.axis = -1;
    if (controller == nullptr) {
        return false;
    }
    __try {
        std::uint8_t transitionMode = 0;
        std::uint8_t transitionFlags = 0;
        std::int8_t storedState = -1;
        std::int16_t authoredRegion = -1;
        std::uint8_t positionValid = 0;
        std::memcpy(&transitionMode, controller + 0x209, sizeof transitionMode);
        std::memcpy(&transitionFlags, controller + 0x20A, sizeof transitionFlags);
        std::memcpy(&output.targetRegion, controller + 0x210, sizeof output.targetRegion);
        std::memcpy(&output.regionA, controller + 0x2B0, sizeof output.regionA);
        std::memcpy(&output.regionB, controller + 0x2B4, sizeof output.regionB);
        std::memcpy(&storedState, controller + 0x352, sizeof storedState);
        std::memcpy(&authoredRegion, controller + 0x4EC, sizeof authoredRegion);
        std::memcpy(
            &output.previousCoordinate, controller + 0x4F0, sizeof output.previousCoordinate);
        std::memcpy(&output.entryIndex, controller + 0x4F4, sizeof output.entryIndex);
        std::memcpy(&output.targetCoordinate, controller + 0x4F8, sizeof output.targetCoordinate);
        std::memcpy(&output.axis, controller + 0x4FC, sizeof output.axis);
        std::memcpy(&output.currentCoordinate, controller + 0x500, sizeof output.currentCoordinate);
        std::memcpy(&positionValid, controller + 0x510, sizeof positionValid);
        std::memcpy(&output.positionReference, controller + 0x514, sizeof output.positionReference);
        output.storedAfter = storedState;
        output.transitionMode = transitionMode;
        output.transitionFlags = transitionFlags;
        output.authoredRegion = authoredRegion;
        output.positionValid = positionValid != 0;
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.requestedState = -1;
        output.storedBefore = -1;
        output.storedAfter = -1;
        output.transitionMode = -1;
        output.targetRegion = -1;
        output.regionA = -1;
        output.regionB = -1;
        output.authoredRegion = -1;
        output.entryIndex = -1;
        output.axis = -1;
        return false;
    }
}

/** @return Stable key for one native positional-classifier observation. */
[[nodiscard]] std::uint64_t z_leg_signature(const ZLegDebugSnapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        for (std::size_t index = 0; index < sizeof value; ++index) {
            hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
            hash *= 1099511628211ULL;
        }
    };
    mix(snapshot.controller);
    mix(static_cast<std::uint32_t>(snapshot.requestedState));
    mix(static_cast<std::uint32_t>(snapshot.storedAfter));
    mix(static_cast<std::uint32_t>(snapshot.transitionMode));
    mix(snapshot.transitionFlags);
    mix(static_cast<std::uint32_t>(snapshot.targetRegion));
    mix(static_cast<std::uint32_t>(snapshot.regionA));
    mix(static_cast<std::uint32_t>(snapshot.regionB));
    mix(static_cast<std::uint32_t>(snapshot.authoredRegion));
    mix(static_cast<std::uint32_t>(snapshot.entryIndex));
    mix(static_cast<std::uint32_t>(snapshot.axis));
    mix(snapshot.positionReference);
    mix(snapshot.positionValid ? 1U : 0U);
    return hash == 0 ? 1 : hash;
}

/** Publishes every fresh observation and claims only bounded changes or heartbeats for logging. */
[[nodiscard]] bool publish_z_leg(const ZLegDebugSnapshot& snapshot,
                                 std::uint32_t& occurrence) noexcept {
    occurrence = 0;
    if (!TryAcquireSRWLockExclusive(&g_zLegLock)) {
        return false;
    }
    const std::uint64_t signature = z_leg_signature(snapshot);
    const bool due = signature != g_zLegReportSignature || g_zLegReportedAt == 0
                     || snapshot.observedAt - g_zLegReportedAt >= kZLegHeartbeatMilliseconds;
    g_zLegDebug = snapshot;
    if (due) {
        g_zLegReportSignature = signature;
        g_zLegReportedAt = snapshot.observedAt;
    }
    ReleaseSRWLockExclusive(&g_zLegLock);
    if (!due || g_zLegReports.load(std::memory_order_relaxed) >= kZLegReportLimit) {
        return false;
    }
    occurrence = g_zLegReports.fetch_add(1, std::memory_order_relaxed) + 1;
    return occurrence <= kZLegReportLimit;
}

/** Reports the exact native positional band that blocks PUBLIC TARGET promotion. */
void report_z_leg(const ZLegDebugSnapshot& snapshot, std::uint32_t occurrence) noexcept {
    std::array<char, 512> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=z-leg-state occurrence=%u controller=%p requested=%d "
        "stored=%d->%d mode=%d flags=0x%02X target=%d region_fields=%d/%d authored=%d "
        "entry=%d axis=%d coordinates=%.5g/%.5g/%.5g position_valid=%u ref=0x%08X",
        occurrence,
        reinterpret_cast<void*>(snapshot.controller),
        snapshot.requestedState,
        snapshot.storedBefore,
        snapshot.storedAfter,
        snapshot.transitionMode,
        snapshot.transitionFlags,
        snapshot.targetRegion,
        snapshot.regionA,
        snapshot.regionB,
        snapshot.authoredRegion,
        snapshot.entryIndex,
        snapshot.axis,
        static_cast<double>(snapshot.previousCoordinate),
        static_cast<double>(snapshot.targetCoordinate),
        static_cast<double>(snapshot.currentCoordinate),
        snapshot.positionValid ? 1U : 0U,
        snapshot.positionReference);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Publishes one coherent current-region manager observation for the overlay and send gate. */
void publish_active_manager(const ActiveManagerDebugSnapshot& snapshot) noexcept {
    AcquireSRWLockExclusive(&g_activeManagerLock);
    g_activeManagerDebug = snapshot;
    ReleaseSRWLockExclusive(&g_activeManagerLock);
}

/** @return A stable report key for one region/token/namespace promotion. */
[[nodiscard]] std::uint64_t
active_manager_report_key(const ActiveManagerDebugSnapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        for (std::size_t index = 0; index < sizeof value; ++index) {
            hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
            hash *= 1099511628211ULL;
        }
    };
    mix(snapshot.token);
    mix(static_cast<std::uint32_t>(snapshot.region));
    mix(static_cast<std::uint32_t>(snapshot.requestedNamespace));
    return hash == 0 ? 1 : hash;
}

/** Reports a bounded native manager selection once per exact current-world owner. */
void report_active_manager_selection(const ActiveManagerDebugSnapshot& snapshot) noexcept {
    const std::uint64_t key = active_manager_report_key(snapshot);
    if (g_lastActiveManagerSelection.exchange(key, std::memory_order_relaxed) == key) {
        return;
    }
    const std::uint32_t occurrence =
        g_activeManagerSelectionReports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (occurrence > kActiveManagerSelectionReportLimit) {
        return;
    }
    std::array<char, 384> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=active-region-manager result=native-current occurrence=%u "
                      "region=%d slice=%d token=0x%016llX namespace=%d active=%d manager_match=%u",
                      occurrence,
                      snapshot.region,
                      snapshot.nativeSlice,
                      static_cast<unsigned long long>(snapshot.token),
                      snapshot.requestedNamespace,
                      snapshot.activeAfter,
                      snapshot.managerMatched ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Observes whether native PUBLIC CURRENT owns the player's coherent current region.
 *
 * A bound target view is not enough to activate its world cells. Only FUN_1416EC250's native
 * PUBLIC-role selection may change the active identity; forcing that integer early services an
 * unready manager and can strand the surrounding slice transition.
 */
void observe_current_region_manager(std::byte* runtime) noexcept {
    ActiveManagerDebugSnapshot snapshot{};
    snapshot.observedAt = GetTickCount64();
    if (runtime == nullptr || !bootflow::in_world()) {
        publish_active_manager(snapshot);
        return;
    }
    // A normal z-leg changes the client-reported region before the old native slice retires. That
    // is precisely the stuck PUBLIC CURRENT overlap this reconciliation repairs. Keep the slice
    // as a diagnostic, but use in_world to distinguish it from unsafe initial loading.
    (void)bootflow::current_slice_set(snapshot.nativeSlice);
    // Native PUBLIC CURRENT exists before the initial gameplay view reaches the server's later
    // bound marker. Read the native identity first so the session overlay can name that row
    // correctly even while the semantic current-region capture is still incomplete.
    __try {
        std::memcpy(&snapshot.activeBefore,
                    runtime + kRuntimeActiveManagerOffset,
                    sizeof snapshot.activeBefore);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        publish_active_manager(snapshot);
        return;
    }
    if (snapshot.activeBefore < 0 || snapshot.activeBefore >= kSimulationManagerCount) {
        publish_active_manager(snapshot);
        return;
    }
    snapshot.activeAfter = snapshot.activeBefore;

    state::activity::membership::WorldSnapshot world{};
    if (!state::activity::membership::primary_world(world)
        || world.region == state::activity::membership::kAbsentRegionIndex) {
        snapshot.region = world.region;
        publish_active_manager(snapshot);
        return;
    }
    snapshot.region = world.region;

    const std::uint64_t groupSession =
        server::gameplay::group::advertised_group_session(world.region);
    if (groupSession == 0 || !server::gameplay::peer::view_bound(groupSession)) {
        publish_active_manager(snapshot);
        return;
    }
    snapshot.token = server::gameplay::group::held_host_session(groupSession);
    if (snapshot.token == 0
        || server::gameplay::group::holding_group_session(snapshot.token) != groupSession) {
        publish_active_manager(snapshot);
        return;
    }

    entity_slot_probe::ViewCapture capture{};
    if (!entity_slot_probe::find(snapshot.token, capture) || capture.token != snapshot.token
        || capture.manager == nullptr || capture.namespaceId < 0
        || capture.namespaceId >= kSimulationManagerCount) {
        publish_active_manager(snapshot);
        return;
    }
    snapshot.requestedNamespace = capture.namespaceId;

    std::byte* const container =
        runtime + kRuntimeManagerContainerOffset
        + static_cast<std::size_t>(capture.namespaceId) * kRuntimeManagerStride;
    const std::byte* const expectedManager = container + kContainerObjectManagerOffset;
    std::int32_t managerIdentity = -1;
    __try {
        std::memcpy(&managerIdentity, container + 8, sizeof managerIdentity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        publish_active_manager(snapshot);
        return;
    }
    snapshot.managerMatched =
        capture.manager == expectedManager && managerIdentity == capture.namespaceId;
    if (!snapshot.managerMatched) {
        publish_active_manager(snapshot);
        return;
    }

    snapshot.ready = snapshot.activeAfter == capture.namespaceId;
    publish_active_manager(snapshot);
    if (snapshot.ready) {
        report_active_manager_selection(snapshot);
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
        if (watched) {
            sobject_bind_probe::record_apply(before.entity);
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
        if (watched) {
            sobject_bind_probe::record_kind0(entity, result);
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
            sobject_bind_probe::record_promoted(
                before.namespaceId, before.entity, afterReadable && after.occupied);
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
            sobject_bind_probe::record_dirty_service(namespaceId, entity);
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

/** Preserves one dirty-row pass and reports its exact object state and caller outputs. */
__declspec(noinline) std::uint8_t __fastcall dirty_row_body(std::byte* manager,
                                                            std::uint16_t internalIndex,
                                                            int* batch,
                                                            std::uint8_t argumentState,
                                                            std::uint8_t reservedArgument,
                                                            std::uint8_t* stateOut,
                                                            std::uint8_t* continueOut,
                                                            int* countOut,
                                                            std::uint8_t suppressCreate,
                                                            std::uint8_t finalPass) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::sobjectDirtyRow, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<DirtyRow>(lease.original);
    std::uint8_t result = argumentState;
    __try {
        std::int32_t namespaceId = -1;
        std::uint32_t entity = 0;
        DirtyServiceSnapshot mapping{};
        DirtyRowSnapshot before{};
        const bool watched =
            lease.accepting && inspect_manager_namespace(manager, namespaceId)
            && sobject_bind_probe::first_watched(namespaceId, entity)
            && inspect_dirty_service(manager, namespaceId, entity, mapping) && mapping.mapped
            && static_cast<std::uint16_t>(mapping.internalIndex) == internalIndex
            && inspect_dirty_row(
                manager, namespaceId, internalIndex, batch, stateOut, continueOut, countOut, before)
            && before.entity == entity && sobject_bind_probe::watched(namespaceId, before.entity);
        const std::uint32_t occurrence =
            watched ? g_dirtyRowReports.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (call != nullptr) {
            result = call(manager,
                          internalIndex,
                          batch,
                          argumentState,
                          reservedArgument,
                          stateOut,
                          continueOut,
                          countOut,
                          suppressCreate,
                          finalPass);
        }
        if (occurrence != 0 && occurrence <= kDirtyRowReportLimit) {
            DirtyRowSnapshot after{};
            (void)inspect_dirty_row(
                manager, namespaceId, internalIndex, batch, stateOut, continueOut, countOut, after);
            report_dirty_row(occurrence,
                             before,
                             after,
                             argumentState,
                             reservedArgument,
                             suppressCreate,
                             finalPass,
                             result);
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
            sobject_bind_probe::record_type2(
                snapshot.entity, result, jobOutReadable && returnedJob != nullptr);
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

/** Preserves native public-session discovery, then observes the selected current-region manager. */
__declspec(noinline) void __fastcall active_manager_refresh_body(std::byte* runtime) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activeManagerRefresh, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ActiveManagerRefresh>(lease.original);
    __try {
        if (call != nullptr) {
            call(runtime);
        }
        if (lease.accepting && runtime != nullptr) {
            g_runtime.store(reinterpret_cast<std::uintptr_t>(runtime), std::memory_order_release);
            observe_current_region_manager(runtime);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves the public-session predicate and passively exposes both citizen acceptance gates. */
__declspec(noinline) bool __fastcall citizen_session_ready_body(const std::byte* session) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::citizenSessionReady, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CitizenSessionReady>(lease.original);
    bool result = false;
    __try {
        if (call != nullptr) {
            result = call(session);
        }
        CitizenSessionSnapshot snapshot{};
        std::uint32_t occurrence = 0;
        if (lease.accepting && inspect_citizen_session(session, result, snapshot)
            && claim_citizen_session_report(snapshot, occurrence)) {
            report_citizen_session(snapshot, occurrence);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the sole asynchronous citizen-join query and traces its exact outer-gate state. */
__declspec(noinline) std::int32_t __fastcall
citizen_join_status_body(std::int32_t kind, std::uint64_t handle) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::citizenJoinStatus, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CitizenJoinStatus>(lease.original);
    std::int32_t state = 0;
    __try {
        if (call != nullptr) {
            state = call(kind, handle);
        }
        std::uint32_t occurrence = 0;
        if (lease.accepting && kind == 2 && claim_citizen_join_report(handle, state, occurrence)) {
            report_citizen_join(handle, state, occurrence);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return state;
}

/** Preserves native z-leg state publication and passively records its positional classifier. */
__declspec(noinline) void __fastcall z_leg_state_body(std::byte* controller,
                                                      std::int32_t requestedState) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::zLegState, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ZLegState>(lease.original);
    __try {
        ZLegDebugSnapshot before{};
        if (lease.accepting) {
            (void)inspect_z_leg(controller, before);
        }
        if (call != nullptr) {
            call(controller, requestedState);
        }
        ZLegDebugSnapshot after{};
        std::uint32_t occurrence = 0;
        if (lease.accepting && inspect_z_leg(controller, after)) {
            after.observedAt = GetTickCount64();
            after.requestedState = requestedState;
            after.storedBefore = before.readable ? before.storedAfter : -1;
            if (publish_z_leg(after, occurrence)) {
                report_z_leg(after, occurrence);
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
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

void* dirty_row_entry_point() noexcept {
    return reinterpret_cast<void*>(&dirty_row_body);
}

void* type2_job_entry_point() noexcept {
    return reinterpret_cast<void*>(&type2_job_body);
}

void* active_manager_refresh_entry_point() noexcept {
    return reinterpret_cast<void*>(&active_manager_refresh_body);
}

void* citizen_session_ready_entry_point() noexcept {
    return reinterpret_cast<void*>(&citizen_session_ready_body);
}

void* citizen_join_status_entry_point() noexcept {
    return reinterpret_cast<void*>(&citizen_join_status_body);
}

void* z_leg_state_entry_point() noexcept {
    return reinterpret_cast<void*>(&z_leg_state_body);
}

void service_current_region_manager() noexcept {
    auto* const runtime = reinterpret_cast<std::byte*>(g_runtime.load(std::memory_order_acquire));
    observe_current_region_manager(runtime);
}

bool current_region_manager_active(std::uint64_t token, std::int32_t namespaceId) noexcept {
    ActiveManagerDebugSnapshot snapshot{};
    if (!active_manager_debug_snapshot(snapshot)) {
        return false;
    }
    return snapshot.ready && snapshot.managerMatched && snapshot.token == token
           && snapshot.requestedNamespace == namespaceId && snapshot.region >= 0
           && snapshot.observedAt != 0
           && GetTickCount64() - snapshot.observedAt < kActiveManagerFreshMilliseconds;
}

bool current_region_manager_active(std::uint64_t token) noexcept {
    ActiveManagerDebugSnapshot snapshot{};
    if (!active_manager_debug_snapshot(snapshot)) {
        return false;
    }
    return snapshot.ready && snapshot.managerMatched && snapshot.token == token
           && snapshot.region >= 0 && snapshot.observedAt != 0
           && GetTickCount64() - snapshot.observedAt < kActiveManagerFreshMilliseconds;
}

bool native_manager_active(std::uint64_t token) noexcept {
    ActiveManagerDebugSnapshot snapshot{};
    if (token == 0 || !active_manager_debug_snapshot(snapshot) || snapshot.activeAfter < 0
        || snapshot.activeAfter >= kSimulationManagerCount || snapshot.observedAt == 0
        || GetTickCount64() - snapshot.observedAt >= kActiveManagerFreshMilliseconds) {
        return false;
    }
    entity_slot_probe::ViewCapture capture{};
    return entity_slot_probe::find(token, capture) && capture.token == token
           && capture.manager != nullptr && capture.namespaceId == snapshot.activeAfter;
}

bool active_manager_debug_snapshot(ActiveManagerDebugSnapshot& output) noexcept {
    output = {};
    output.region = -1;
    output.nativeSlice = -1;
    output.requestedNamespace = -1;
    output.activeBefore = -1;
    output.activeAfter = -1;
    AcquireSRWLockShared(&g_activeManagerLock);
    output = g_activeManagerDebug;
    ReleaseSRWLockShared(&g_activeManagerLock);
    return output.observedAt != 0;
}

bool z_leg_debug_snapshot(ZLegDebugSnapshot& output) noexcept {
    output = {};
    output.requestedState = -1;
    output.storedBefore = -1;
    output.storedAfter = -1;
    output.transitionMode = -1;
    output.targetRegion = -1;
    output.regionA = -1;
    output.regionB = -1;
    output.authoredRegion = -1;
    output.entryIndex = -1;
    output.axis = -1;
    AcquireSRWLockShared(&g_zLegLock);
    output = g_zLegDebug;
    ReleaseSRWLockShared(&g_zLegLock);
    return output.readable && output.observedAt != 0
           && GetTickCount64() - output.observedAt < kZLegFreshMilliseconds;
}

void reset() noexcept {
    g_applyReports.store(0, std::memory_order_relaxed);
    g_kind0Reports.store(0, std::memory_order_relaxed);
    g_promotionReports.store(0, std::memory_order_relaxed);
    g_dirtyServiceReports.store(0, std::memory_order_relaxed);
    g_dirtyRowReports.store(0, std::memory_order_relaxed);
    g_type2JobReports.store(0, std::memory_order_relaxed);
    g_activeManagerSelectionReports.store(0, std::memory_order_relaxed);
    g_citizenSessionReports.store(0, std::memory_order_relaxed);
    g_citizenJoinReports.store(0, std::memory_order_relaxed);
    g_zLegReports.store(0, std::memory_order_relaxed);
    g_lastActiveManagerSelection.store(0, std::memory_order_relaxed);
    g_runtime.store(0, std::memory_order_release);
    AcquireSRWLockExclusive(&g_activeManagerLock);
    g_activeManagerDebug = {};
    ReleaseSRWLockExclusive(&g_activeManagerLock);
    AcquireSRWLockExclusive(&g_citizenSessionLock);
    g_citizenSessions = {};
    ReleaseSRWLockExclusive(&g_citizenSessionLock);
    AcquireSRWLockExclusive(&g_citizenJoinLock);
    g_citizenJoins = {};
    ReleaseSRWLockExclusive(&g_citizenJoinLock);
    AcquireSRWLockExclusive(&g_zLegLock);
    g_zLegDebug = {};
    g_zLegReportedAt = 0;
    g_zLegReportSignature = 0;
    ReleaseSRWLockExclusive(&g_zLegLock);
}

} // namespace sunrise::client::hooks::network::sobject_apply_probe
