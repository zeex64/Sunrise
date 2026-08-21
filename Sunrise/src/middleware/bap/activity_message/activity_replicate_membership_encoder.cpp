#include "replicate_membership.h"

namespace sunrise::middleware::bap::activity_message::replicate_membership {
namespace {

/** The local member sits in slot zero; a distinct reflected host sits in slot one. */
[[nodiscard]] constexpr std::uint32_t member_mask(const MembershipSnapshot& snapshot) noexcept {
    return snapshot.hasReflectedHost ? 3U : 1U;
}

/** Only the non-local reflected host is retained by the native per-peer replication lifecycle. */
constexpr std::uint32_t kReflectedHostRetainMask = 1U << 1U;

/**
 * Writes the three known top-level member masks followed by the two absent tail fields.
 * The first two masks describe every populated member. The retain mask deliberately excludes
 * local slot zero and exists only when the reflected host occupies slot one.
 */
[[nodiscard]] bool write_member_masks(encoding::bits::Writer& writer,
                                      const MembershipSnapshot& snapshot) noexcept {
    const std::uint32_t populated = member_mask(snapshot);
    bool encoded = writer.write(1, 1) && writer.write(populated, 32) && writer.write(1, 1)
                   && writer.write(populated, 32);
    encoded = encoded
              && (snapshot.hasReflectedHost
                      ? writer.write(1, 1) && writer.write(kReflectedHostRetainMask, 32)
                      : writer.write(0, 1));
    return encoded && writer.write(0, 1) && writer.write(0, 1);
}

static_assert(kMeaningfulBitCount == kEncodedSize * 8U);
static_assert(kReflectedHostRetainMask == 2U);

[[nodiscard]] constexpr MembershipSnapshot reflected_host_size_probe() noexcept {
    MembershipSnapshot snapshot{};
    snapshot.hasReflectedHost = true;
    return snapshot;
}

constexpr MembershipSnapshot kReflectedHostSizeProbe = reflected_host_size_probe();
static_assert(region_block_start_bit(kReflectedHostSizeProbe) == 2'202);
static_assert(region_block_end_bit(kReflectedHostSizeProbe) == 31'266);
static_assert(meaningful_bit_count(kReflectedHostSizeProbe) == 31'367);
static_assert(encoded_size(kReflectedHostSizeProbe) == 3'921);

} // namespace

/** Encodes one fixed full-player membership snapshot without allocation. */
bool encode_replicate_membership(const MembershipSnapshot& snapshot,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept {
    written = 0;
    const std::size_t size = encoded_size(snapshot);
    if (output.size() < size || !valid(snapshot)) {
        return false;
    }

    encoding::bits::Writer writer(output.first(size));
    const bool encoded = writer.write(1, 1) && writer.write(snapshot.revision, 32)
                         && writer.write(snapshot.epoch, 32)
                         && write_member_table(writer, snapshot) && writer.write(1, 1)
                         && write_region_block(writer, snapshot)
                         && write_member_masks(writer, snapshot);
    std::size_t encodedSize = 0;
    const std::size_t meaningfulBits = meaningful_bit_count(snapshot);
    if (!encoded || writer.bit_count() != meaningfulBits || !writer.finish(encodedSize)
        || encodedSize != size) {
        return false;
    }

    written = encodedSize;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::replicate_membership
