#include "sobject_rsat_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../../state/gameplay/definition.h"
#include "sobject_update_probe.h"

namespace sunrise::client::hooks::network::sobject_rsat_probe {
namespace {

/** The create decoder's queue/ready call pair is unique inside its small fixed entry window. */
constexpr std::size_t kDecoderScanBytes = 0x100;
constexpr std::array<int, 21> kQueueReadyPattern{
    0x8B, 0x0B, 0x88, 0x43, 0x04, 0xE8, -1,   -1,   -1,   -1,   0x8B,
    0x0B, 0xE8, -1,   -1,   -1,   -1,   0x84, 0xC0, 0x0F, 0x85,
};
constexpr std::size_t kQueueCallOffset = 5;
constexpr std::size_t kReadyCallOffset = 12;

enum class State : std::uint8_t {
    unresolved,
    queued,
    ready,
    fault,
};

using Queue = void(__fastcall*)(std::uint32_t);
using Ready = bool(__fastcall*)(std::uint32_t);

std::atomic_uintptr_t g_queue{};
std::atomic_uintptr_t g_ready{};
std::atomic<State> g_state{State::unresolved};
std::atomic_bool g_updateReady{};
std::atomic_flag g_polling = ATOMIC_FLAG_INIT;

/** Reports only state transitions, so the hot view lookup does not generate repeated lines. */
void report(const char* result) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=gameplay stage=sobject-rsat-preload result=%s "
                                      "rsat=0x%08X",
                                      result,
                                      state::gameplay::kFirstEntityRsat);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         std::strcmp(result, "fault") == 0 ? core::log::Level::warn
                                                           : core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Resolves one x64 E8 rel32 target without retaining a pointer into decoder scratch. */
[[nodiscard]] bool resolve_call(const std::byte* call, std::uintptr_t& output) noexcept {
    output = 0;
    if (call == nullptr || call[0] != std::byte{0xE8}) {
        return false;
    }
    std::int32_t displacement{};
    std::memcpy(&displacement, call + 1, sizeof displacement);
    const auto end = reinterpret_cast<std::intptr_t>(call + 5);
    output = static_cast<std::uintptr_t>(end + displacement);
    return output != 0;
}

/** Finds the adjacent private queue and residency calls in the live kind-0 create decoder. */
[[nodiscard]] bool
resolve(const void* decoderAddress, std::uintptr_t& queue, std::uintptr_t& ready) noexcept {
    queue = 0;
    ready = 0;
    if (decoderAddress == nullptr) {
        return false;
    }
    const auto* const decoder = static_cast<const std::byte*>(decoderAddress);
    __try {
        for (std::size_t offset = 0; offset + kQueueReadyPattern.size() <= kDecoderScanBytes;
             ++offset) {
            bool match = true;
            for (std::size_t byte = 0; byte < kQueueReadyPattern.size(); ++byte) {
                const int expected = kQueueReadyPattern[byte];
                if (expected >= 0 && decoder[offset + byte] != static_cast<std::byte>(expected)) {
                    match = false;
                    break;
                }
            }
            if (match && resolve_call(decoder + offset + kQueueCallOffset, queue)
                && resolve_call(decoder + offset + kReadyCallOffset, ready)) {
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        queue = 0;
        ready = 0;
    }
    return false;
}

} // namespace

void poll_first_entity(const void* createDecoder) noexcept {
    const State current = g_state.load(std::memory_order_acquire);
    if ((current == State::ready && g_updateReady.load(std::memory_order_acquire))
        || current == State::fault || g_polling.test_and_set(std::memory_order_acquire)) {
        return;
    }

    __try {
        std::uintptr_t queue = g_queue.load(std::memory_order_acquire);
        std::uintptr_t ready = g_ready.load(std::memory_order_acquire);
        if (queue == 0 || ready == 0) {
            if (!resolve(createDecoder, queue, ready)) {
                g_state.store(State::fault, std::memory_order_release);
                report("fault");
                __leave;
            }
            g_queue.store(queue, std::memory_order_release);
            g_ready.store(ready, std::memory_order_release);
        }

        if (g_state.load(std::memory_order_relaxed) == State::unresolved) {
            reinterpret_cast<Queue>(queue)(state::gameplay::kFirstEntityRsat);
            g_state.store(State::queued, std::memory_order_release);
            report("queued");
        }
        if (reinterpret_cast<Ready>(ready)(state::gameplay::kFirstEntityRsat)) {
            g_state.store(State::ready, std::memory_order_release);
            if (current != State::ready) {
                report("ready");
            }
            if (sobject_update_probe::prime_first_entity_update(
                    state::gameplay::kFirstEntityRsat)) {
                g_updateReady.store(true, std::memory_order_release);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_state.store(State::fault, std::memory_order_release);
        report("fault");
    }
    g_polling.clear(std::memory_order_release);
}

bool first_entity_ready() noexcept {
    return g_state.load(std::memory_order_acquire) == State::ready
           && g_updateReady.load(std::memory_order_acquire);
}

void reset() noexcept {
    g_queue.store(0, std::memory_order_relaxed);
    g_ready.store(0, std::memory_order_relaxed);
    g_state.store(State::unresolved, std::memory_order_relaxed);
    g_updateReady.store(false, std::memory_order_relaxed);
    g_polling.clear(std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::network::sobject_rsat_probe
