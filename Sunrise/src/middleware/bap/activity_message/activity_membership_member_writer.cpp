#include <bit>
#include <limits>

#include "replicate_membership.h"

namespace sunrise::middleware::bap::activity_message::replicate_membership {
namespace {

/** 8 elements, low byte first, encode a member or host key. */
constexpr std::size_t kMemberKeyByteCount = 8;
/** The membership table has 32 fixed member slots. */
constexpr std::size_t kMemberCount = 32;
/** Each absent member adds 3 clear presence bits. */
constexpr std::uint8_t kAbsentMemberBitCount = 3;
/** Member field 1 uses 10 bits with a bias of 1. */
/**
 * Member field 1 is a skip test, not a value. The client drops the member when the stored value
 * read as unsigned is at or below 0x1FF, so the only usable wire value is the one that stores -1.
 * Wire 1 stores zero, and the member is dropped with nothing reported.
 */
constexpr std::uint32_t kField1Bias = 1;
/** Member field 2 uses the signed 32-bit midpoint as its bias. */
constexpr std::uint32_t kField2Bias = 0x80000000U;
/** A present local member carries a zero logical leave reason at bias 1. */
constexpr std::uint8_t kLeaveReasonWire = 1;
/** The nested identity block has presence bits on fields 0 through 14. */
constexpr std::size_t kIdentityPresenceFieldCount = 15;
/** Identity field zero is a six-bit scalar stored at member byte 0x38. */
constexpr std::uint8_t kViewGateBitWidth = 6;
constexpr std::uint8_t kViewGateMask = 0x10;
/** Identity field 11 starts at member +0x142, the address consumed by native view creation. */
constexpr std::size_t kViewAddressField = 11;
/** The minimal nested player blob is 18 bytes, including one zero pad bit. */
constexpr std::uint16_t kPlayerBlobByteCount = 18;

/**
 * Writes one 8-element key, low byte first.
 * @param writer Fixed-buffer MSB-first writer.
 * @param key Host-order key to split into byte elements.
 * @return True when all 8 elements fit.
 */
[[nodiscard]] bool write_member_key(encoding::bits::Writer& writer, std::uint64_t key) noexcept {
    for (std::size_t index = 0; index < kMemberKeyByteCount; ++index) {
        if (!writer.write((key >> (index * 8U)) & 0xFFU, 8)) {
            return false;
        }
    }
    return true;
}

/**
 * Writes the nested 18-byte player blob with matching identity values.
 * @param writer Fixed-buffer writer sitting after the 14-bit byte count.
 * @param identity Identity values repeated inside the nested player record.
 * @return True when all 144 blob bits fit.
 */
[[nodiscard]] bool write_player_blob(encoding::bits::Writer& writer,
                                     const client_identity::ClientIdentity& identity) noexcept {
    return writer.write(1, 3) && writer.write(0, 1) && writer.write(0, 10) && writer.write(1, 1)
           && writer.write(identity.accountSoid, 64) && writer.write(identity.field5, 64)
           && writer.write(0, 1);
}

/** Writes the fixed byte-array body of identity field 11 in element order. */
[[nodiscard]] bool write_network_address(
    encoding::bits::Writer& writer,
    const std::array<std::byte, gameplay::descriptor::kNetAddrSize>& address) noexcept {
    for (const std::byte value : address) {
        if (!writer.write(std::to_integer<std::uint8_t>(value), 8)) {
            return false;
        }
    }
    return true;
}

/**
 * Writes the present fields of the nested player identity block.
 * @param writer Fixed-buffer writer sitting at identity field zero.
 * @param identity Values mirrored from the client identity update.
 * @param viewEligible Whether field 0 carries the reflected-host view gate.
 * @return True when the selected fields and all presence bits fit.
 */
[[nodiscard]] bool write_player_identity(encoding::bits::Writer& writer,
                                         const client_identity::ClientIdentity& identity,
                                         bool viewEligible,
                                         const std::array<std::byte,
                                                          gameplay::descriptor::kNetAddrSize>&
                                             viewAddress) noexcept {
    for (std::size_t field = 0; field < kIdentityPresenceFieldCount; ++field) {
        const bool present =
            ((field == 0 || field == kViewAddressField) && viewEligible) || field == 3
            || field == 5 || field == 14;
        if (!writer.write(present ? 1U : 0U, 1)) {
            return false;
        }
        if (field == 0 && present && !writer.write(kViewGateMask, kViewGateBitWidth)) {
            return false;
        }
        if (field == 3 && !writer.write(identity.accountSoid, 64)) {
            return false;
        }
        if (field == 5 && !writer.write(identity.field5, 64)) {
            return false;
        }
        if (field == kViewAddressField && present
            && !write_network_address(writer, viewAddress)) {
            return false;
        }
        if (field == 14
            && (!writer.write(kPlayerBlobByteCount, 14) || !write_player_blob(writer, identity))) {
            return false;
        }
    }
    return writer.write(0, 1);
}

/** Writes one complete member record at the writer's current slot. */
[[nodiscard]] bool write_member(encoding::bits::Writer& writer,
                                const client_identity::ClientIdentity& identity,
                                bool viewEligible,
                                const std::array<std::byte,
                                                 gameplay::descriptor::kNetAddrSize>&
                                    viewAddress) noexcept {
    const std::uint32_t field1Wire = std::bit_cast<std::uint32_t>(identity.field1) + kField1Bias;
    const std::uint32_t field2Wire = std::bit_cast<std::uint32_t>(identity.field2) + kField2Bias;
    return writer.write(1, 1) && write_member_key(writer, identity.memberKey)
           && writer.write(field1Wire, 10) && writer.write(field2Wire, 32)
           && writer.write(identity.field3, 64) && writer.write(identity.accountSoid, 64)
           && writer.write(identity.field5, 64) && writer.write(identity.field6, 64)
           && writer.write(1, 1) && writer.write(1, 1)
           && write_player_identity(writer, identity, viewEligible, viewAddress)
           // These are optional-subtree presence bits, not the native view gate. Advertising the
           // first subtree requires a further 29 payload bits and shifts every later field.
           && writer.write(0, 3)
           && writer.write(1, 1) && writer.write(kLeaveReasonWire, 5);
}

} // namespace

/** Checks the one field that could otherwise encode outside its own wire width. */
bool valid(const MembershipSnapshot& snapshot) noexcept {
    namespace authoritative = client_authoritative_data;
    return snapshot.identity.memberKey != 0
           && (!snapshot.hasReflectedHost
               || (snapshot.reflectedHost.memberKey != 0
                   && snapshot.reflectedHost.memberKey != snapshot.identity.memberKey))
           && snapshot.teleport.sliceSetIndex >= authoritative::kAbsentSliceSetIndex
           && snapshot.teleport.sliceSetIndex <= authoritative::kMaximumSliceSetIndex;
}

/** Writes the one populated member and 31 absent member slots. */
bool write_member_table(encoding::bits::Writer& writer,
                        const MembershipSnapshot& snapshot) noexcept {
    bool encoded = writer.bit_count() == kMemberStartBit
                   && write_member(
                       writer, snapshot.identity, false, snapshot.reflectedHostAddress);
    std::size_t populated = 1;
    if (encoded && snapshot.hasReflectedHost) {
        encoded = write_member(
            writer, snapshot.reflectedHost, true, snapshot.reflectedHostAddress);
        populated = 2;
    }
    for (std::size_t member = populated; encoded && member < kMemberCount; ++member) {
        encoded = writer.write(0, kAbsentMemberBitCount);
    }
    return encoded && writer.bit_count() + 1 == region_block_start_bit(snapshot);
}

} // namespace sunrise::middleware::bap::activity_message::replicate_membership
