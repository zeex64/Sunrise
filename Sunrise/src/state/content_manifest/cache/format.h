#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../definition.h"

namespace sunrise::state::content_manifest::cache {

/** 8 owned ASCII bytes mark generated content-manifest caches. */
inline constexpr std::array<char, 8> kCacheMagic{'S', 'U', 'N', 'C', 'M', 'A', 'N', 'F'};
/**
 * Version 3 binds the served manifest identity to the package-directory identity. Version 2
 * fingerprinted only public rows, so replacing a package without changing its build signature
 * could leave the Client's private package-header metadata stale.
 */
inline constexpr std::uint32_t kCacheVersion = 3;

#pragma pack(push, 1)
/** Fixed prefix that checks cache version, layout, directory and selected build. */
struct Header final {
    std::array<char, kCacheMagic.size()> magic{};
    std::uint32_t version{};
    std::uint32_t headerSize{};
    std::uint32_t rowSize{};
    std::uint32_t rowCount{};
    /** Zero keeps the fixed header canonical for future extensions. */
    std::uint32_t reserved{};
    Fingerprint directoryFingerprint{};
    Fingerprint buildFingerprint{};
};

/** Stable disk row with only the public package fields ContentConfig needs. */
struct DiskRow final {
    std::array<char, kPackageNameCapacity> name{};
    std::uint16_t nameLength{};
    std::uint16_t packageId{};
    std::uint64_t buildSignature{};
};
#pragma pack(pop)

static_assert(sizeof(Header)
              == kCacheMagic.size() + 5 * sizeof(std::uint32_t) + 2 * kFingerprintSize);
static_assert(sizeof(DiskRow)
              == kPackageNameCapacity + 2 * sizeof(std::uint16_t) + sizeof(std::uint64_t));

} // namespace sunrise::state::content_manifest::cache
