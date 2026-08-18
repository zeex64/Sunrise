#include "view_signature_capture.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::view_signature {
namespace {

/** Native c_network_channel_view::m_view_signature_valid. */
constexpr std::size_t kPresentOffset = 0x8C;
/** Native c_network_channel_view::m_view_signature_bytes. */
constexpr std::size_t kBytesOffset = 0x8D;
/** Native c_network_channel_view::m_view_signature_size. */
constexpr std::size_t kCountOffset = 0xA0;
/** Four-byte world datum precedes the group-session id. */
constexpr std::size_t kSessionOffset = 4;
/** A valid signature has the world datum and complete session id. */
constexpr std::size_t kMinimumSignatureBytes = kSessionOffset + sizeof(std::uint64_t);
/** Native networking holds at most a few overlapping views during a region transition. */
constexpr std::size_t kCaptureCapacity = 8;

struct CaptureSlot {
    CapturedSignature signature{};
    std::uint64_t sessionId{};
    bool occupied{};
};

SRWLOCK g_captureLock{SRWLOCK_INIT};
std::array<CaptureSlot, kCaptureCapacity> g_captures{};
std::size_t g_replacementCursor{};
std::atomic_bool g_refreshSeen{false};

using Refresh = void(__fastcall*)(void*);

/** Publishes a validated native signature without retaining the game-owned view. */
void publish(std::uint64_t sessionId, const CapturedSignature& signature) noexcept {
    AcquireSRWLockExclusive(&g_captureLock);
    CaptureSlot* destination = nullptr;
    bool first = false;
    for (CaptureSlot& slot : g_captures) {
        if (slot.occupied && slot.sessionId == sessionId) {
            destination = &slot;
            break;
        }
        if (destination == nullptr && !slot.occupied) {
            destination = &slot;
        }
    }
    if (destination == nullptr) {
        destination = &g_captures[g_replacementCursor % g_captures.size()];
        ++g_replacementCursor;
        first = true;
    } else if (!destination->occupied) {
        first = true;
    }
    destination->sessionId = sessionId;
    destination->signature = signature;
    destination->occupied = true;
    ReleaseSRWLockExclusive(&g_captureLock);
    if (first) {
        std::uint32_t worldDatum = 0;
        std::memcpy(&worldDatum, signature.bytes.data(), sizeof worldDatum);
        std::array<char, 160> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=gameplay stage=view-signature result=captured token=0x%llX world=0x%08X "
            "bytes=%u",
            static_cast<unsigned long long>(sessionId),
            worldDatum,
            static_cast<unsigned>(signature.count));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Copies the fields written by the original refresh while its view remains borrowed. */
void capture(void* nativeView) noexcept {
    if (nativeView == nullptr) {
        return;
    }
    const auto* const bytes = static_cast<const std::byte*>(nativeView);
    CapturedSignature signature{};
    std::uint32_t nativeCount = 0;
    bool present = false;
    __try {
        present = std::to_integer<unsigned>(bytes[kPresentOffset]) != 0;
        std::memcpy(&nativeCount, bytes + kCountOffset, sizeof nativeCount);
        if (present && nativeCount >= kMinimumSignatureBytes
            && nativeCount <= signature.bytes.size()) {
            std::memcpy(signature.bytes.data(), bytes + kBytesOffset, nativeCount);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!g_refreshSeen.exchange(true, std::memory_order_relaxed)) {
        std::array<char, 128> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=gameplay stage=view-signature result=refresh "
                                          "present=%u bytes=%u",
                                          present ? 1U : 0U,
                                          nativeCount);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (!present || nativeCount < kMinimumSignatureBytes
        || nativeCount > signature.bytes.size()) {
        return;
    }
    signature.count = static_cast<std::uint8_t>(nativeCount);
    std::uint64_t sessionId = 0;
    std::memcpy(&sessionId, signature.bytes.data() + kSessionOffset, sizeof sessionId);
    if (sessionId != 0) {
        publish(sessionId, signature);
    }
}

/** Calls the native builder first, then snapshots its completed signature. */
__declspec(noinline) void __fastcall refresh_body(void* nativeView) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::viewSignatureRefresh, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Refresh>(lease.original);
    __try {
        if (call != nullptr) {
            call(nativeView);
        }
        if (lease.accepting) {
            capture(nativeView);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

} // namespace

/** @return The internal-linkage refresh body, kept safe while the detour is removed. */
void* refresh_entry_point() noexcept {
    return reinterpret_cast<void*>(&refresh_body);
}

/** Copies one captured signature by its embedded group-session id. */
bool find(std::uint64_t sessionId, CapturedSignature& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_captureLock);
    bool found = false;
    for (const CaptureSlot& slot : g_captures) {
        if (slot.occupied && slot.sessionId == sessionId) {
            output = slot.signature;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_captureLock);
    return found;
}

/** Clears every captured signature during shutdown. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_captureLock);
    g_captures = {};
    g_replacementCursor = 0;
    g_refreshSeen.store(false, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_captureLock);
}

} // namespace sunrise::client::hooks::network::view_signature
