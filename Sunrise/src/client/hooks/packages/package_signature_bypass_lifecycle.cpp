#include "package_signature_bypass_lifecycle.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::packages {
namespace {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Call site used by the patchable-package path. The match includes the native result test and
 * state write so the selected call cannot drift to an unrelated package helper.
 */
constexpr std::string_view kPatchableRegistrarCallText =
    "48 8B 4B 18 48 8D 54 24 ? 8B 81 08 01 00 00 4C 8B 89 00 01 00 00 44 0F B7 81 10 01 "
    "00 00 89 44 24 ? 8B 81 0C 01 00 00 89 44 24 ? 48 C7 44 24 ? 00 00 00 00 E8 ? ? ? ? "
    "83 F8 01 74 ? 89 05 ? ? ? ?";
constexpr auto kPatchableRegistrarCall =
    signature<signature_length(kPatchableRegistrarCallText)>(kPatchableRegistrarCallText);

/**
 * Native package-header gate. Its sixth argument is the already-computed signature verdict; a
 * false verdict returns -93 immediately, while a true verdict continues through every ordinary
 * header, identity, size, locale and token validation.
 */
constexpr std::string_view kHeaderValidationGateText =
    "40 53 48 83 EC 20 80 7C 24 58 00 44 0F B7 DA 4C 8B D1 BB 01 00 00 00 75 0D BB A3 FF "
    "FF FF 8B C3 48 83 C4 20 5B C3";
constexpr auto kHeaderValidationGate =
    signature<signature_length(kHeaderValidationGateText)>(kHeaderValidationGateText);

constexpr std::size_t kRegistrarCallOperand = 0x36;
constexpr std::size_t kRegistrarCallEnd = 0x3A;
constexpr std::uint32_t kCustomPackageMinimum = 0x0AA0;
constexpr std::uint32_t kCustomPackageMaximum = 0x0CFF;
constexpr std::size_t kPackageNameCapacity = 256;
constexpr int kSignatureRejected = -93;

using Registrar = int(__fastcall*)(const char*,
                                   const std::uint32_t*,
                                   std::uint32_t,
                                   std::uint64_t,
                                   const void*,
                                   std::uint32_t);

using HeaderValidationGate = int(__fastcall*)(const std::uint32_t*,
                                               std::uint16_t,
                                               std::uint64_t,
                                               std::uint32_t,
                                               std::uint16_t,
                                               bool,
                                               const void*);

hooking::detour::Handle g_handle{};
hooking::detour::Handle g_headerValidationHandle{};
std::atomic_bool g_installed{false};
std::atomic_bool g_headerBypassObserved{false};
std::atomic_bool g_registrarResultObserved{false};

/** Lets reserved package IDs continue past only the native signature-verdict branch. */
int __fastcall validate_header(const std::uint32_t* policy,
                               std::uint16_t expectedPackageId,
                               std::uint64_t expectedBuildSignature,
                               std::uint32_t expectedFileSize,
                               std::uint16_t packageClass,
                               bool signatureAccepted,
                               const void* header) noexcept {
    const auto original =
        reinterpret_cast<HeaderValidationGate>(g_headerValidationHandle.original);
    if (original == nullptr) {
        return kSignatureRejected;
    }
    // The fifth native argument describes the registration class/range, not the authored ID.
    // The version-38 header carries its authoritative package ID at byte offset four; the native
    // validator subsequently compares that value with expectedPackageId when policy requires it.
    std::uint16_t headerPackageId = 0;
    if (header != nullptr) {
        std::memcpy(&headerPackageId,
                    static_cast<const std::byte*>(header) + sizeof(std::uint32_t),
                    sizeof headerPackageId);
    }
    const bool custom = headerPackageId >= kCustomPackageMinimum
                        && headerPackageId <= kCustomPackageMaximum;
    const bool report = custom && !signatureAccepted
                        && !g_headerBypassObserved.exchange(true, std::memory_order_acq_rel);
    const int result = original(policy,
                                expectedPackageId,
                                expectedBuildSignature,
                                expectedFileSize,
                                packageClass,
                                signatureAccepted || custom,
                                header);
    if (report) {
        std::array<char, 224> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=package_signature stage=header_gate scope=custom_id header=0x%04X expected=0x%04X class=0x%04X signed=0 native_result=%d result=bypassed",
            static_cast<unsigned>(headerPackageId),
            static_cast<unsigned>(expectedPackageId),
            static_cast<unsigned>(packageClass),
            result);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return result;
}

/** Observes the native registrar outcome after all validation and commit work. */
int __fastcall registrar_result(const char* path,
                                const std::uint32_t* metadata,
                                std::uint32_t packageId,
                                std::uint64_t buildSignature,
                                const void* header,
                                std::uint32_t headerSize) noexcept {
    const auto original = reinterpret_cast<Registrar>(g_handle.original);
    const int result = original != nullptr
                           ? original(path, metadata, packageId, buildSignature, header, headerSize)
                           : kSignatureRejected;
    const bool custom = packageId >= kCustomPackageMinimum && packageId <= kCustomPackageMaximum;
    const bool report =
        custom && !g_registrarResultObserved.exchange(true, std::memory_order_acq_rel);
    if (report) {
        const std::uint32_t word0 = metadata != nullptr ? metadata[0] : 0;
        const std::size_t nameLength =
            path != nullptr ? strnlen_s(path, kPackageNameCapacity) : 0;
        std::array<char, 384> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=package_signature stage=registrar scope=%s id=0x%04X name=%.*s signature=%016llX meta0=%08X flags=%08X result=%d",
            "custom_id",
            static_cast<unsigned>(packageId),
            static_cast<int>(nameLength),
            path != nullptr ? path : "",
            static_cast<unsigned long long>(buildSignature),
            word0,
            headerSize,
            result);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return result;
}

} // namespace

/** Installs the scoped custom-package signature-result hook. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const callSite =
        scan_main_image_unique(kPatchableRegistrarCall, "patchable_package_registrar_call");
    if (callSite == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=package_signature stage=install result=fail");
        return false;
    }
    std::byte* const target =
        resolve_relative(callSite + kRegistrarCallOperand, callSite + kRegistrarCallEnd);
    std::byte* const headerGate =
        scan_main_image_unique(kHeaderValidationGate, "package_header_validation_gate");
    if (target == nullptr || headerGate == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=package_signature stage=install result=fail");
        return false;
    }
    if (!hooking::detour::install(
            hooking::detour::Spec{headerGate, reinterpret_cast<void*>(&validate_header)},
            g_headerValidationHandle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=package_signature stage=install result=fail gate=header");
        return false;
    }
    if (!hooking::detour::install(
            hooking::detour::Spec{target, reinterpret_cast<void*>(&registrar_result)}, g_handle)) {
        (void)hooking::detour::uninstall(g_headerValidationHandle);
        g_headerValidationHandle = {};
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=package_signature stage=install result=fail");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=package_signature stage=install result=ok scope=0x0AA0-0x0CFF gate=header");
    return true;
}

/** Removes the custom-package signature-result hook. */
void uninstall() noexcept {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)hooking::detour::uninstall(g_handle);
    g_handle = {};
    if (g_headerValidationHandle.attached) {
        (void)hooking::detour::uninstall(g_headerValidationHandle);
    }
    g_headerValidationHandle = {};
}

} // namespace sunrise::client::hooks::packages
