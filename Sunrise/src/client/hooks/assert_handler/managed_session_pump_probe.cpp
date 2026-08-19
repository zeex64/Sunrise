#include "managed_session_pump_probe.h"

#include <Windows.h>

#include <atomic>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::assert_handler::managed_session_pump_probe {
namespace {

/**
 * `FUN_14175E520`, the managed-session state pump directly called by `network_update`.
 * The complete nonvolatile-save and 0x7A0-byte frame prologue is unique in the pinned image.
 */
constexpr std::string_view kPumpSignatureText =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 "
    "48 8D AC 24 60 F9 FF FF 48 81 EC A0 07 00 00";
constexpr auto kPumpSignature = patterns::signature<patterns::signature_length(kPumpSignatureText)>(
    kPumpSignatureText);

using Pump = void(__fastcall*)() noexcept;

hooking::detour::Handle g_handle{};
std::atomic<Pump> g_original{nullptr};
std::atomic<std::uint64_t> g_nextSerial{0};
std::atomic<std::uint64_t> g_enteredSerial{0};
std::atomic<std::uint64_t> g_returnedSerial{0};
std::atomic<std::uint32_t> g_activeCalls{0};
std::atomic<std::uint32_t> g_enteredThread{0};

/** Records only entry and return; the hot pump emits no log lines and takes no locks. */
__declspec(noinline) void __fastcall pump() noexcept {
    const std::uint64_t serial = g_nextSerial.fetch_add(1, std::memory_order_relaxed) + 1;
    g_enteredThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_activeCalls.fetch_add(1, std::memory_order_acq_rel);
    g_enteredSerial.store(serial, std::memory_order_release);
    const Pump original = g_original.load(std::memory_order_acquire);
    if (original != nullptr) {
        original();
    }
    g_returnedSerial.store(serial, std::memory_order_release);
    g_activeCalls.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace

/** Attaches the managed-session pump observer without making it a required game hook. */
bool install() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target =
        patterns::scan_main_image_unique(kPumpSignature, "managed_session_pump");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=assert stage=managed-session-probe result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&pump)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=assert stage=managed-session-probe result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<Pump>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=assert stage=managed-session-probe result=ok");
    return true;
}

/** Detaches the managed-session pump observer. */
void uninstall() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_handle = {};
    g_original.store(nullptr, std::memory_order_release);
    g_nextSerial.store(0, std::memory_order_release);
    g_enteredSerial.store(0, std::memory_order_release);
    g_returnedSerial.store(0, std::memory_order_release);
    g_activeCalls.store(0, std::memory_order_release);
    g_enteredThread.store(0, std::memory_order_release);
}

/** Reports pump progress without waiting on the potentially stalled network thread. */
ProgressSnapshot progress_snapshot() noexcept {
    ProgressSnapshot snapshot{};
    snapshot.enteredSerial = g_enteredSerial.load(std::memory_order_acquire);
    snapshot.returnedSerial = g_returnedSerial.load(std::memory_order_acquire);
    snapshot.activeCalls = g_activeCalls.load(std::memory_order_acquire);
    snapshot.enteredThread = g_enteredThread.load(std::memory_order_relaxed);
    return snapshot;
}

} // namespace sunrise::client::hooks::assert_handler::managed_session_pump_probe
