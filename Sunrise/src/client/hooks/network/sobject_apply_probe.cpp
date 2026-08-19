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

using ApplyJob = void(__fastcall*)(std::byte*);
using Kind0Constructor = bool(__fastcall*)(void*, const std::uint32_t*, std::uint32_t, int);

std::atomic_uint32_t g_applyReports{};
std::atomic_uint32_t g_kind0Reports{};

struct JobSnapshot {
    std::uint8_t type{};
    std::uint8_t flags{};
    std::uint32_t entity{};
    std::uintptr_t storage{};
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

} // namespace

void* apply_entry_point() noexcept {
    return reinterpret_cast<void*>(&apply_body);
}

void* kind0_entry_point() noexcept {
    return reinterpret_cast<void*>(&kind0_body);
}

void reset() noexcept {
    g_applyReports.store(0, std::memory_order_relaxed);
    g_kind0Reports.store(0, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::network::sobject_apply_probe
