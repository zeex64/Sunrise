#include "sobject_lifecycle_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../targets/game/network.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"
#include "sobject_bind_probe.h"
#include "sobject_update_probe.h"

namespace sunrise::client::hooks::network::sobject_lifecycle_probe {
namespace {

constexpr std::uint32_t kReportLimit = 16;
constexpr std::uint32_t kCandidateReportLimit = 4;
constexpr std::uint32_t kSharedVandalRsat = 0x815B204B;
constexpr std::uint32_t kInvalidObject = 0xFFFFFFFF;
constexpr std::uint32_t kObjectSlotMask = 0x1FFF;
constexpr std::uint32_t kNativeObjectStride = 0xE0;
constexpr std::size_t kNativeObjectDatumHandleOffset = 0x0C;
constexpr std::size_t kNativeObjectCurrentHandleOffset = 0x4C;
constexpr std::uint64_t kPendingTransformLifetimeMs = 2000;
constexpr std::uint64_t kPendingActivationLifetimeMs = 5000;
constexpr std::uint32_t kActiveLifecycleState = 1;

enum class Callback : std::size_t {
    inboundUpdate,
    lifecycleState,
    componentState,
    objectTransform,
    count,
};

struct TraceScope {
    Callback callback{};
    std::int32_t currentObject{-1};
    std::uint32_t resolvedEntity{};
    std::uint32_t definitionTag{};
    std::uint32_t selector{};
    std::uint32_t requestedState{};
    std::uint64_t generation{};
    bool currentObserved{};
    bool entityObserved{};
    bool definitionObserved{};
};

struct PendingTransform {
    std::array<float, 8> transform{};
    std::uint32_t currentHandle{kInvalidObject};
    std::uint32_t datumHandle{kInvalidObject};
    std::uint64_t generation{};
    std::uint64_t queuedAt{};
    bool present{};
};

struct PendingActivation {
    void* self{};
    std::uint32_t selector{};
    std::uint64_t generation{};
    std::uint64_t queuedAt{};
    bool present{};
};

std::array<std::atomic_uint32_t, static_cast<std::size_t>(Callback::count)> g_reports{};
std::array<std::atomic_uint32_t, static_cast<std::size_t>(Callback::count)> g_candidates{};
std::atomic_uint64_t g_generation{1};
std::atomic_bool g_transformPropagationAttempted{false};
std::atomic_bool g_activationAttempted{false};
SRWLOCK g_pendingTransformLock = SRWLOCK_INIT;
PendingTransform g_pendingTransform{};
SRWLOCK g_pendingActivationLock = SRWLOCK_INIT;
PendingActivation g_pendingActivation{};
thread_local TraceScope* g_activeTrace{};

using CurrentObjectLookup = std::int32_t*(__fastcall*)(std::int32_t*, std::uint32_t);
using CurrentObjectResolver = std::uint32_t*(__fastcall*)(std::uint32_t*, std::uint32_t);
using InboundUpdateApply =
    std::uintptr_t(__fastcall*)(void*, std::uint32_t, void*, std::uint32_t, void*, void*);
using StateCallback = void(__fastcall*)(void*, std::uint32_t, std::uint32_t);
using ObjectTransform = void(__fastcall*)(void*, const float*);

struct ObjectTransformSnapshot {
    const void* object{};
    std::uint32_t datumHandle{kInvalidObject};
    std::uint32_t currentHandle{kInvalidObject};
    std::array<float, 8> transform{};
};

/** Resolves and validates the live native object without retaining a game-owned pointer. */
[[nodiscard]] bool resolve_native_object(std::uint32_t currentHandle,
                                         std::uint32_t datumHandle,
                                         std::byte*& object) noexcept {
    object = nullptr;
    const targets::game::network::Targets& resolved = targets::game::network::get();
    if (currentHandle == kInvalidObject || datumHandle == kInvalidObject
        || resolved.sobjectNativeObjectTable == nullptr) {
        return false;
    }
    __try {
        std::byte* base = nullptr;
        std::uint32_t stride = 0;
        std::memcpy(&base, resolved.sobjectNativeObjectTable + 8, sizeof base);
        std::memcpy(&stride, resolved.sobjectNativeObjectTable + 0x10, sizeof stride);
        if (base == nullptr || stride != kNativeObjectStride) {
            return false;
        }
        std::byte* const candidate =
            base + static_cast<std::size_t>(datumHandle & kObjectSlotMask) * stride;
        std::uint32_t storedDatumHandle = kInvalidObject;
        std::uint32_t storedCurrentHandle = kInvalidObject;
        std::memcpy(&storedDatumHandle,
                    candidate + kNativeObjectDatumHandleOffset,
                    sizeof storedDatumHandle);
        std::memcpy(&storedCurrentHandle,
                    candidate + kNativeObjectCurrentHandleOffset,
                    sizeof storedCurrentHandle);
        if (storedDatumHandle != datumHandle || storedCurrentHandle != currentHandle) {
            return false;
        }
        object = candidate;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        object = nullptr;
        return false;
    }
}

/** Emits one bounded state line for the controlled post-update propagation experiment. */
void report_transform_propagation(const char* result,
                                  const PendingTransform& pending,
                                  const void* object,
                                  std::uint64_t age) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=gameplay stage=sobject-transform-propagation result=%s "
                                      "current=0x%08X datum=0x%08X object=%p age_ms=%llu "
                                      "rotation=%.5f/%.5f/%.5f/%.5f "
                                      "position=%.3f/%.3f/%.3f/%.3f",
                                      result,
                                      pending.currentHandle,
                                      pending.datumHandle,
                                      object,
                                      static_cast<unsigned long long>(age),
                                      static_cast<double>(pending.transform[0]),
                                      static_cast<double>(pending.transform[1]),
                                      static_cast<double>(pending.transform[2]),
                                      static_cast<double>(pending.transform[3]),
                                      static_cast<double>(pending.transform[4]),
                                      static_cast<double>(pending.transform[5]),
                                      static_cast<double>(pending.transform[6]),
                                      static_cast<double>(pending.transform[7]));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         std::strcmp(result, "fault") == 0 ? core::log::Level::warn
                                                           : core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Emits one bounded result for the exact post-bind lifecycle activation. */
void report_activation(const char* result,
                       const PendingActivation& pending,
                       const sobject_bind_probe::EntityDebugSnapshot* debug,
                       std::uint64_t age) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=sobject-vandal-activation result=%s self=%p selector=0x%08X "
        "state=%u age_ms=%llu native=0x%08X index=0x%08X bound=%u",
        result,
        pending.self,
        pending.selector,
        kActiveLifecycleState,
        static_cast<unsigned long long>(age),
        debug != nullptr ? debug->nativeObjectId : kInvalidObject,
        debug != nullptr ? debug->nativeObjectIndex : kInvalidObject,
        debug != nullptr && debug->bound ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         std::strcmp(result, "fault") == 0 ? core::log::Level::warn
                                                           : core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @return Stable log name for one callback. */
[[nodiscard]] const char* callback_name(Callback callback) noexcept {
    switch (callback) {
    case Callback::inboundUpdate:
        return "update-apply";
    case Callback::lifecycleState:
        return "object-state";
    case Callback::componentState:
        return "component-state";
    case Callback::objectTransform:
        return "object-transform";
    default:
        return "unknown";
    }
}

/** Reads the datum identity and exact transform input without retaining native pointers. */
[[nodiscard]] bool inspect_object_transform(const void* object,
                                            const float* transform,
                                            ObjectTransformSnapshot& output) noexcept {
    output = {};
    output.datumHandle = kInvalidObject;
    output.currentHandle = kInvalidObject;
    if (object == nullptr || transform == nullptr) {
        return false;
    }
    __try {
        output.object = object;
        const auto* const bytes = static_cast<const std::byte*>(object);
        std::memcpy(
            &output.datumHandle, bytes + kNativeObjectDatumHandleOffset, sizeof output.datumHandle);
        std::memcpy(&output.currentHandle,
                    bytes + kNativeObjectCurrentHandleOffset,
                    sizeof output.currentHandle);
        std::memcpy(output.transform.data(), transform, sizeof output.transform);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.datumHandle = kInvalidObject;
        output.currentHandle = kInvalidObject;
        return false;
    }
}

/** Reports exact target propagation plus four ambient calls that validate the passive hook. */
void report_object_transform(const ObjectTransformSnapshot& snapshot) noexcept {
    sobject_bind_probe::EntityDebugSnapshot debug{};
    const bool targetKnown = sobject_bind_probe::debug_snapshot(debug) && debug.present
                             && debug.sent && debug.nativeSeen;
    const bool matched = targetKnown && snapshot.currentHandle == debug.nativeObjectId;
    std::atomic_uint32_t& counter =
        matched ? g_reports[static_cast<std::size_t>(Callback::objectTransform)]
                : g_candidates[static_cast<std::size_t>(Callback::objectTransform)];
    const std::uint32_t occurrence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (occurrence > (matched ? kReportLimit : kCandidateReportLimit)) {
        return;
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=sobject-vandal-lifecycle callback=object-transform matched=%u "
        "occurrence=%u object=%p current=0x%08X datum=0x%08X "
        "target_native_object=0x%08X "
        "rotation=%.5f/%.5f/%.5f/%.5f position=%.3f/%.3f/%.3f/%.3f",
        matched ? 1U : 0U,
        occurrence,
        snapshot.object,
        snapshot.currentHandle,
        snapshot.datumHandle,
        targetKnown ? debug.nativeObjectId : 0xFFFFFFFF,
        static_cast<double>(snapshot.transform[0]),
        static_cast<double>(snapshot.transform[1]),
        static_cast<double>(snapshot.transform[2]),
        static_cast<double>(snapshot.transform[3]),
        static_cast<double>(snapshot.transform[4]),
        static_cast<double>(snapshot.transform[5]),
        static_cast<double>(snapshot.transform[6]),
        static_cast<double>(snapshot.transform[7]));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports exact target callbacks plus four bounded mismatches that validate correlation. */
void report(const TraceScope& trace, std::uintptr_t result) noexcept {
    if (!trace.currentObserved || trace.currentObject < 0
        || trace.generation != g_generation.load(std::memory_order_acquire)) {
        return;
    }

    sobject_bind_probe::EntityDebugSnapshot debug{};
    if (!sobject_bind_probe::debug_snapshot(debug) || !debug.present || !debug.sent
        || !debug.nativeSeen) {
        return;
    }

    const std::uint32_t targetEntity =
        debug.runtimeEntityId != 0 ? debug.runtimeEntityId : debug.entityId;
    const bool currentMatched =
        static_cast<std::uint32_t>(trace.currentObject) == debug.nativeObjectId;
    const bool matched =
        currentMatched
        && (trace.callback != Callback::inboundUpdate || trace.selector == targetEntity);
    if (!matched && !debug.bound) {
        return;
    }
    std::atomic_uint32_t& counter = matched
                                        ? g_reports[static_cast<std::size_t>(trace.callback)]
                                        : g_candidates[static_cast<std::size_t>(trace.callback)];
    const std::uint32_t occurrence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (occurrence > (matched ? kReportLimit : kCandidateReportLimit)) {
        return;
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=sobject-vandal-lifecycle callback=%s matched=%u "
                      "occurrence=%u current_object=0x%08X entity_resolver=0x%08X "
                      "definition_resolver=0x%08X target_entity=0x%08X "
                      "target_native_object=0x%08X "
                      "slot=%u native=0x%08X selector=0x%08X requested_state=%u "
                      "result=0x%016llX",
                      callback_name(trace.callback),
                      matched ? 1U : 0U,
                      occurrence,
                      static_cast<std::uint32_t>(trace.currentObject),
                      trace.entityObserved ? trace.resolvedEntity : 0xFFFFFFFF,
                      trace.definitionObserved ? trace.definitionTag : 0xFFFFFFFF,
                      targetEntity,
                      debug.nativeObjectId,
                      static_cast<unsigned>(debug.slot),
                      debug.nativeObjectIndex,
                      trace.selector,
                      trace.requestedState,
                      static_cast<unsigned long long>(result));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Queues the exact accepted target update for one main-thread transform propagation. */
void queue_target_transform(const TraceScope& trace, std::uintptr_t result) noexcept {
    if (result == 0 || !trace.currentObserved || !trace.entityObserved || trace.currentObject < 0
        || trace.resolvedEntity == kInvalidObject
        || trace.generation != g_generation.load(std::memory_order_acquire)
        || g_transformPropagationAttempted.load(std::memory_order_acquire)) {
        return;
    }

    sobject_bind_probe::EntityDebugSnapshot debug{};
    if (!sobject_bind_probe::debug_snapshot(debug) || !debug.present || !debug.sent
        || !debug.nativeSeen || !debug.bindSeen || !debug.bound || debug.rsat != kSharedVandalRsat
        || debug.nativeObjectId == kInvalidObject) {
        return;
    }
    const std::uint32_t targetEntity =
        debug.runtimeEntityId != 0 ? debug.runtimeEntityId : debug.entityId;
    if (static_cast<std::uint32_t>(trace.currentObject) != debug.nativeObjectId
        || trace.selector != targetEntity) {
        return;
    }

    PendingTransform pending{};
    if (!sobject_update_probe::nearby_player_transform(kSharedVandalRsat, pending.transform)) {
        return;
    }
    pending.currentHandle = debug.nativeObjectId;
    pending.datumHandle = trace.resolvedEntity;
    pending.generation = trace.generation;
    pending.queuedAt = GetTickCount64();
    pending.present = true;
    bool queued = false;
    AcquireSRWLockExclusive(&g_pendingTransformLock);
    if (!g_pendingTransform.present) {
        g_pendingTransform = pending;
        queued = true;
    }
    ReleaseSRWLockExclusive(&g_pendingTransformLock);
    if (queued) {
        report_transform_propagation("queued", pending, nullptr, 0);
    }
}

/** Retains the game-owned kind-0 callback receiver until the exact target reaches bind. */
void queue_target_activation(void* self, const TraceScope& trace) noexcept {
    if (self == nullptr || trace.generation != g_generation.load(std::memory_order_acquire)
        || g_activationAttempted.load(std::memory_order_acquire)) {
        return;
    }
    sobject_bind_probe::EntityDebugSnapshot debug{};
    if (!sobject_bind_probe::debug_snapshot(debug) || !debug.present || !debug.sent
        || debug.rsat != kSharedVandalRsat) {
        return;
    }
    const std::uint32_t targetEntity =
        debug.runtimeEntityId != 0 ? debug.runtimeEntityId : debug.entityId;
    if (trace.selector != targetEntity) {
        return;
    }

    PendingActivation pending{};
    pending.self = self;
    pending.selector = trace.selector;
    pending.generation = trace.generation;
    pending.queuedAt = GetTickCount64();
    pending.present = true;
    bool queued = false;
    AcquireSRWLockExclusive(&g_pendingActivationLock);
    if (!g_pendingActivation.present) {
        g_pendingActivation = pending;
        queued = true;
    }
    ReleaseSRWLockExclusive(&g_pendingActivationLock);
    if (queued) {
        report_activation("queued", pending, &debug, 0);
    }
}

/** Preserves one nested resolver and copies its output into the active lifecycle scope. */
std::uint32_t* run_resolver(HookSlot slot,
                            bool entityResolver,
                            std::uint32_t* output,
                            std::uint32_t currentObject) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, slot, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CurrentObjectResolver>(lease.original);
    std::uint32_t* result = output;
    __try {
        if (call != nullptr) {
            result = call(output, currentObject);
        }
        TraceScope* const trace = g_activeTrace;
        if (lease.accepting && trace != nullptr && output != nullptr
            && trace->generation == g_generation.load(std::memory_order_acquire)) {
            __try {
                if (entityResolver) {
                    trace->resolvedEntity = *output;
                    trace->entityObserved = true;
                } else {
                    trace->definitionTag = *output;
                    trace->definitionObserved = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (entityResolver) {
                    trace->entityObserved = false;
                } else {
                    trace->definitionObserved = false;
                }
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Captures the object selected by a nested game-owned lookup without making an extra native call.
 */
__declspec(noinline) std::int32_t* __fastcall
current_object_lookup_body(std::int32_t* output, std::uint32_t selector) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectCurrentObjectLookup, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CurrentObjectLookup>(lease.original);
    std::int32_t* result = output;
    __try {
        if (call != nullptr) {
            result = call(output, selector);
        }
        TraceScope* const trace = g_activeTrace;
        if (lease.accepting && trace != nullptr && output != nullptr
            && trace->generation == g_generation.load(std::memory_order_acquire)) {
            __try {
                trace->currentObject = *output;
                trace->currentObserved = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                trace->currentObject = -1;
                trace->currentObserved = false;
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

__declspec(noinline) std::uint32_t* __fastcall
current_entity_resolver_body(std::uint32_t* output, std::uint32_t currentObject) noexcept {
    return run_resolver(HookSlot::sobjectCurrentEntityResolver, true, output, currentObject);
}

__declspec(noinline) std::uint32_t* __fastcall
current_definition_resolver_body(std::uint32_t* output, std::uint32_t currentObject) noexcept {
    return run_resolver(HookSlot::sobjectCurrentDefinitionResolver, false, output, currentObject);
}

/** Preserves inbound application while correlating its own nested native-object lookup. */
__declspec(noinline) std::uintptr_t __fastcall inbound_update_apply_body(void* self,
                                                                         std::uint32_t selector,
                                                                         void* update,
                                                                         std::uint32_t flags,
                                                                         void* context,
                                                                         void* tail) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectInboundUpdateApply, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<InboundUpdateApply>(lease.original);
    TraceScope trace{};
    trace.callback = Callback::inboundUpdate;
    trace.selector = selector;
    trace.requestedState = flags;
    trace.generation = g_generation.load(std::memory_order_acquire);
    TraceScope* const previous = g_activeTrace;
    g_activeTrace = &trace;
    std::uintptr_t result{};
    __try {
        if (call != nullptr) {
            result = call(self, selector, update, flags, context, tail);
        }
        if (lease.accepting) {
            report(trace, result);
        }
    } __finally {
        g_activeTrace = previous;
        coordinator::g_callEgress();
    }
    return result;
}

/** Runs one void state callback under a nested current-object correlation scope. */
void run_state_callback(HookSlot slot,
                        Callback callback,
                        void* self,
                        std::uint32_t selector,
                        std::uint32_t state) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, slot, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<StateCallback>(lease.original);
    TraceScope trace{};
    trace.callback = callback;
    trace.selector = selector;
    trace.requestedState = state;
    trace.generation = g_generation.load(std::memory_order_acquire);
    TraceScope* const previous = g_activeTrace;
    g_activeTrace = &trace;
    __try {
        if (call != nullptr) {
            call(self, selector, state);
        }
        if (lease.accepting) {
            report(trace, 0);
        }
    } __finally {
        g_activeTrace = previous;
        coordinator::g_callEgress();
    }
}

/** Preserves the native transform propagation while observing exact target participation. */
__declspec(noinline) void __fastcall object_transform_body(void* object,
                                                           const float* transform) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectObjectTransform, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ObjectTransform>(lease.original);
    ObjectTransformSnapshot snapshot{};
    const bool readable = lease.accepting && inspect_object_transform(object, transform, snapshot);
    __try {
        if (call != nullptr) {
            call(object, transform);
        }
        if (readable) {
            report_object_transform(snapshot);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

__declspec(noinline) void __fastcall
lifecycle_state_body(void* self, std::uint32_t selector, std::uint32_t state) noexcept {
    run_state_callback(
        HookSlot::sobjectLifecycleState, Callback::lifecycleState, self, selector, state);
}

__declspec(noinline) void __fastcall
component_state_body(void* self, std::uint32_t selector, std::uint32_t state) noexcept {
    run_state_callback(
        HookSlot::sobjectComponentState, Callback::componentState, self, selector, state);
}

} // namespace

void* current_object_lookup_entry_point() noexcept {
    return reinterpret_cast<void*>(&current_object_lookup_body);
}

void* inbound_update_apply_entry_point() noexcept {
    return reinterpret_cast<void*>(&inbound_update_apply_body);
}

void* lifecycle_state_entry_point() noexcept {
    return reinterpret_cast<void*>(&lifecycle_state_body);
}

void* component_state_entry_point() noexcept {
    return reinterpret_cast<void*>(&component_state_body);
}

void* current_entity_resolver_entry_point() noexcept {
    return reinterpret_cast<void*>(&current_entity_resolver_body);
}

void* current_definition_resolver_entry_point() noexcept {
    return reinterpret_cast<void*>(&current_definition_resolver_body);
}

void* object_transform_entry_point() noexcept {
    return reinterpret_cast<void*>(&object_transform_body);
}

/** Activates the exact bound kind-0 target through its native object/component state callbacks. */
void poll_target_activation() noexcept {
    if (g_activationAttempted.load(std::memory_order_acquire)) {
        return;
    }
    PendingActivation pending{};
    AcquireSRWLockShared(&g_pendingActivationLock);
    pending = g_pendingActivation;
    ReleaseSRWLockShared(&g_pendingActivationLock);
    if (!pending.present) {
        return;
    }

    const std::uint64_t now = GetTickCount64();
    const std::uint64_t age = now >= pending.queuedAt ? now - pending.queuedAt : 0;
    if (pending.generation != g_generation.load(std::memory_order_acquire) || now < pending.queuedAt
        || age > kPendingActivationLifetimeMs) {
        AcquireSRWLockExclusive(&g_pendingActivationLock);
        if (g_pendingActivation.present && g_pendingActivation.generation == pending.generation
            && g_pendingActivation.selector == pending.selector) {
            g_pendingActivation = {};
        }
        ReleaseSRWLockExclusive(&g_pendingActivationLock);
        report_activation("expired", pending, nullptr, age);
        return;
    }

    sobject_bind_probe::EntityDebugSnapshot debug{};
    if (!sobject_bind_probe::debug_snapshot(debug) || !debug.present || !debug.sent
        || !debug.nativeSeen || !debug.bindSeen || !debug.bound || debug.rsat != kSharedVandalRsat
        || debug.nativeObjectId == kInvalidObject) {
        return;
    }
    const std::uint32_t targetEntity =
        debug.runtimeEntityId != 0 ? debug.runtimeEntityId : debug.entityId;
    if (targetEntity != pending.selector) {
        return;
    }

    coordinator::CallLease objectLease{};
    coordinator::g_callIngress(
        objectLease, HookSlot::sobjectLifecycleState, coordinator::ConsumerKind::none);
    const auto objectCall = reinterpret_cast<StateCallback>(objectLease.original);
    if (!objectLease.accepting || objectCall == nullptr) {
        coordinator::g_callEgress();
        return;
    }
    bool expected = false;
    if (!g_activationAttempted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        coordinator::g_callEgress();
        return;
    }

    AcquireSRWLockExclusive(&g_pendingActivationLock);
    if (g_pendingActivation.present && g_pendingActivation.generation == pending.generation
        && g_pendingActivation.selector == pending.selector) {
        g_pendingActivation = {};
    }
    ReleaseSRWLockExclusive(&g_pendingActivationLock);

    bool faulted = false;
    __try {
        objectCall(pending.self, pending.selector, kActiveLifecycleState);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    coordinator::g_callEgress();

    if (!faulted) {
        coordinator::CallLease componentLease{};
        coordinator::g_callIngress(
            componentLease, HookSlot::sobjectComponentState, coordinator::ConsumerKind::none);
        const auto componentCall = reinterpret_cast<StateCallback>(componentLease.original);
        if (!componentLease.accepting || componentCall == nullptr) {
            faulted = true;
        } else {
            __try {
                componentCall(pending.self, pending.selector, kActiveLifecycleState);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                faulted = true;
            }
        }
        coordinator::g_callEgress();
    }
    report_activation(faulted ? "fault" : "called", pending, &debug, age);
}

void poll_target_transform_propagation() noexcept {
    if (g_transformPropagationAttempted.load(std::memory_order_acquire)) {
        return;
    }

    PendingTransform pending{};
    AcquireSRWLockShared(&g_pendingTransformLock);
    pending = g_pendingTransform;
    ReleaseSRWLockShared(&g_pendingTransformLock);
    if (!pending.present) {
        return;
    }

    const std::uint64_t now = GetTickCount64();
    const std::uint64_t age = now >= pending.queuedAt ? now - pending.queuedAt : 0;
    if (pending.generation != g_generation.load(std::memory_order_acquire) || now < pending.queuedAt
        || age > kPendingTransformLifetimeMs) {
        AcquireSRWLockExclusive(&g_pendingTransformLock);
        if (g_pendingTransform.present && g_pendingTransform.generation == pending.generation
            && g_pendingTransform.currentHandle == pending.currentHandle
            && g_pendingTransform.datumHandle == pending.datumHandle) {
            g_pendingTransform = {};
        }
        ReleaseSRWLockExclusive(&g_pendingTransformLock);
        report_transform_propagation("expired", pending, nullptr, age);
        return;
    }

    sobject_bind_probe::EntityDebugSnapshot debug{};
    if (!sobject_bind_probe::debug_snapshot(debug) || !debug.present || !debug.sent
        || !debug.nativeSeen || !debug.bound || debug.rsat != kSharedVandalRsat
        || debug.nativeObjectId != pending.currentHandle) {
        return;
    }

    std::byte* object = nullptr;
    if (!resolve_native_object(pending.currentHandle, pending.datumHandle, object)) {
        return;
    }

    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::sobjectObjectTransform, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ObjectTransform>(lease.original);
    if (!lease.accepting || call == nullptr) {
        coordinator::g_callEgress();
        return;
    }
    bool expected = false;
    if (!g_transformPropagationAttempted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        coordinator::g_callEgress();
        return;
    }

    AcquireSRWLockExclusive(&g_pendingTransformLock);
    if (g_pendingTransform.present && g_pendingTransform.generation == pending.generation
        && g_pendingTransform.currentHandle == pending.currentHandle
        && g_pendingTransform.datumHandle == pending.datumHandle) {
        g_pendingTransform = {};
    }
    ReleaseSRWLockExclusive(&g_pendingTransformLock);

    bool faulted = false;
    __try {
        call(object, pending.transform.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    coordinator::g_callEgress();

    ObjectTransformSnapshot snapshot{};
    snapshot.object = object;
    snapshot.currentHandle = pending.currentHandle;
    snapshot.datumHandle = pending.datumHandle;
    snapshot.transform = pending.transform;
    report_object_transform(snapshot);
    report_transform_propagation(faulted ? "fault" : "called", pending, object, age);
}

void reset() noexcept {
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    for (std::atomic_uint32_t& counter : g_reports) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint32_t& counter : g_candidates) {
        counter.store(0, std::memory_order_relaxed);
    }
    g_transformPropagationAttempted.store(false, std::memory_order_relaxed);
    g_activationAttempted.store(false, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_pendingTransformLock);
    g_pendingTransform = {};
    ReleaseSRWLockExclusive(&g_pendingTransformLock);
    AcquireSRWLockExclusive(&g_pendingActivationLock);
    g_pendingActivation = {};
    ReleaseSRWLockExclusive(&g_pendingActivationLock);
}

} // namespace sunrise::client::hooks::network::sobject_lifecycle_probe
