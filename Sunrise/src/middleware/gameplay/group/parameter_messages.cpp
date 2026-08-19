#include "parameter_messages.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "../../encoding/bit_raw.h"

namespace sunrise::middleware::gameplay::group {

namespace {

namespace bits = encoding::bits;

/** The mode flag is one bit. */
constexpr std::uint8_t kFlagWidth = 1;
/** Width of a tag-reflection root bit. Clear skips the whole field walk. */
constexpr std::uint8_t kRootBitClear = 1;
/** Width of the `activity-host` id, written most-significant-bit first. */
constexpr std::uint8_t kHostIdWidth = 64;
/** Width of the `activity-host` address. */
constexpr std::uint8_t kAddressWidth = 32;
/** Width of the `activity-host` port. */
constexpr std::uint8_t kPortWidth = 16;
/** Native `remote-join-data` fixed field widths. */
constexpr std::uint8_t kRemoteField00Width = 4;
constexpr std::uint8_t kRemoteField04Width = 2;
constexpr std::uint8_t kRemoteField08Width = 2;
constexpr std::uint8_t kRemoteOptionalPresenceWidth = 1;
constexpr std::uint8_t kRemoteOptionalValueWidth = 2;
constexpr std::uint8_t kRemoteField10Width = 3;
constexpr std::uint8_t kRemoteField11Width = 2;
constexpr std::uint8_t kRemoteRouteWidth = 1;
constexpr std::uint8_t kRemoteField13Width = 1;
constexpr std::uint8_t kRemoteDescriptorIndexWidth = 3;
constexpr std::uint8_t kRemoteFieldA1Width = 5;
constexpr std::uint8_t kRemoteFieldA2Width = 5;
constexpr std::uint8_t kRemoteFieldA3Width = 1;
/** Offsets inside the 128-byte address/session descriptor embedded at native +0x20. */
constexpr std::size_t kDescriptorMachineOffset = 0;
constexpr std::size_t kDescriptorNetAddrOffset = 8;
constexpr std::size_t kDescriptorJoinKeyOffset = 0x5E;
constexpr std::size_t kDescriptorSessionOffset = 0x6E;
constexpr std::size_t kDescriptorJoinKeySize = kDescriptorSessionOffset - kDescriptorJoinKeyOffset;
constexpr std::size_t kDescriptorSessionSize =
    descriptor::kDescriptorSize - kDescriptorSessionOffset;
/** The direct NetAddr codec carries its method plus bytes 0 through 40. */
constexpr std::size_t kDirectNetAddrBytes = 41;
/** Relay methods 6 and 7 carry every NetAddr byte except the separately written method. */
constexpr std::size_t kRelayNetAddrBytes = descriptor::kNetAddrSize - 1;
/** The NetAddr method is its last byte and occupies four bits on the wire. */
constexpr std::size_t kNetAddrMethodOffset = descriptor::kNetAddrSize - 1;
constexpr std::uint8_t kNetAddrMethodWidth = 4;
constexpr std::uint8_t kRelayMethodFirst = 6;
constexpr std::uint8_t kRelayMethodLast = 7;
/** Bits in one byte. */
constexpr unsigned kByteBits = 8;
/** Mask of one byte. */
constexpr std::uint32_t kByteMask = 0xFF;
/** Parameters one presence group covers. The encoder divides 25 by this and rounds up. */
constexpr std::uint8_t kGroupSize = 16;
/** Groups the 25 parameters fall into, and the width of the presence field that names them. */
constexpr std::uint8_t kGroupCount = 2;
/** Only the low 25 bits of either mask name a parameter. */
constexpr std::uint64_t kParameterMaskBits = 0x1FFFFFF;

/**
 * Reduces one parameter mask to the presence bits the encoder writes ahead of it.
 * @param mask Parameter mask.
 * @return Bit per group, set when that group holds any named parameter.
 */
[[nodiscard]] std::uint64_t group_presence(std::uint64_t mask) noexcept {
    std::uint64_t groups = 0;
    for (std::uint8_t group = 0; group < kGroupCount; ++group) {
        const std::uint64_t span = (mask >> (group * kGroupSize)) & ((1ULL << kGroupSize) - 1);
        if ((span & kParameterMaskBits) != 0) {
            groups |= 1ULL << group;
        }
    }
    return groups;
}

/**
 * Writes the `activity-host` body.
 * @param writer Open writer.
 * @param body Field values.
 * @return True when every field fit.
 */
[[nodiscard]] bool write_activity_host(bits::Writer& writer,
                                       const ActivityHostParameter& body) noexcept {
    std::array<std::byte, sizeof(std::uint32_t)> mask{};
    for (std::size_t index = 0; index < mask.size(); ++index) {
        const unsigned shift = static_cast<unsigned>(index) * kByteBits;
        mask[index] = static_cast<std::byte>((body.memberMask >> shift) & kByteMask);
    }
    return bits::write_raw_u64(writer, body.selectionId) && writer.write(body.hostId, kHostIdWidth)
           && bits::write_raw(writer, mask) && writer.write(body.address, kAddressWidth)
           && writer.write(body.port, kPortWidth);
}

/** @return True when every byte in one fixed descriptor range is zero. */
[[nodiscard]] bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(
        bytes.begin(), bytes.end(), [](std::byte value) { return value == std::byte{}; });
}

/** Writes the native NetAddr sub-codec used by `remote-join-data`. */
[[nodiscard]] bool
write_net_addr(bits::Writer& writer,
               std::span<const std::byte, descriptor::kNetAddrSize> address) noexcept {
    const std::uint8_t method = std::to_integer<std::uint8_t>(address[kNetAddrMethodOffset]);
    const std::size_t bodySize = method >= kRelayMethodFirst && method <= kRelayMethodLast
                                     ? kRelayNetAddrBytes
                                     : kDirectNetAddrBytes;
    return writer.write(method, kNetAddrMethodWidth)
           && bits::write_raw(writer, address.first(bodySize));
}

/**
 * Writes the optimized 128-byte descriptor codec used by `remote-join-data`.
 * A wholly empty descriptor is one set bit. A present descriptor carries machine id, join key,
 * compressed NetAddr, then an optional 18-byte online-session tail.
 */
[[nodiscard]] bool
write_join_descriptor(bits::Writer& writer,
                      std::span<const std::byte, descriptor::kDescriptorSize> body) noexcept {
    const bool empty = all_zero(body);
    if (!writer.write(empty ? 1U : 0U, kFlagWidth)) {
        return false;
    }
    if (empty) {
        return true;
    }
    const auto netAddr = body.subspan<kDescriptorNetAddrOffset, descriptor::kNetAddrSize>();
    const auto session = body.subspan<kDescriptorSessionOffset, kDescriptorSessionSize>();
    const bool hasSession = !all_zero(session);
    return bits::write_raw(writer, body.subspan<kDescriptorMachineOffset, sizeof(std::uint64_t)>())
           && bits::write_raw(writer,
                              body.subspan<kDescriptorJoinKeyOffset, kDescriptorJoinKeySize>())
           && write_net_addr(writer, netAddr) && writer.write(hasSession ? 1U : 0U, kFlagWidth)
           && (!hasSession || bits::write_raw(writer, session));
}

/** Writes the exact bounded codec registered for parameter 13 `remote-join-data`. */
[[nodiscard]] bool write_remote_join_data(bits::Writer& writer,
                                          const RemoteJoinDataParameter& body) noexcept {
    if (body.field00 >= 9 || body.field04 >= 3 || body.field08 >= 3
        || (body.optional0C != UINT32_MAX && body.optional0C >= 3) || body.field10 >= 8
        || body.field11 >= 4 || body.descriptorIndex >= 5 || body.fieldA1 >= 19
        || body.fieldA2 >= 19) {
        return false;
    }
    const bool hasOptional = body.optional0C != UINT32_MAX;
    return writer.write(body.field00, kRemoteField00Width)
           && writer.write(body.field04, kRemoteField04Width)
           && writer.write(body.field08, kRemoteField08Width)
           && writer.write(hasOptional ? 1U : 0U, kRemoteOptionalPresenceWidth)
           && (!hasOptional || writer.write(body.optional0C, kRemoteOptionalValueWidth))
           && writer.write(body.field10, kRemoteField10Width)
           && writer.write(body.route ? 1U : 0U, kRemoteRouteWidth)
           && writer.write(body.field11, kRemoteField11Width)
           && writer.write(body.field13 ? 1U : 0U, kRemoteField13Width)
           && bits::write_raw_u64(writer, body.identifier)
           && writer.write(body.descriptorIndex, kRemoteDescriptorIndexWidth)
           && writer.write(body.fieldA1, kRemoteFieldA1Width)
           && writer.write(body.fieldA2, kRemoteFieldA2Width)
           && writer.write(body.fieldA3 ? 1U : 0U, kRemoteFieldA3Width)
           && write_join_descriptor(writer, body.descriptor);
}

/**
 * Writes one carried parameter body.
 * A clear root bit is a complete body for both tag-reflection parameters. It skips the field
 * walk and leaves the reader's own values in place.
 * @param writer Open writer.
 * @param parameter Registry index.
 * @param update Update being written, for the parameters that carry field values.
 * @return True when the parameter has an encoder here and its body fit.
 */
[[nodiscard]] bool write_parameter_body(bits::Writer& writer,
                                        std::uint8_t parameter,
                                        const ParameterUpdate& update) noexcept {
    if (parameter == static_cast<std::uint8_t>(Parameter::publicSessionReservations)
        || parameter == static_cast<std::uint8_t>(Parameter::currentActivity)) {
        return writer.write(0U, kRootBitClear);
    }
    if (parameter == static_cast<std::uint8_t>(Parameter::activityHost)) {
        return write_activity_host(writer, update.activityHost);
    }
    if (parameter == static_cast<std::uint8_t>(Parameter::remoteJoinData)) {
        return write_remote_join_data(writer, update.remoteJoinData);
    }
    return false;
}

/**
 * Writes the per-parameter bits of one mask, in ascending index order.
 * Only parameters in a present group get a bit.
 * @param writer Open writer.
 * @param mask Parameter mask, already reduced to its meaningful bits.
 * @param groups Presence bits for that mask.
 * @param carriesBodies True for the value mask, whose named parameters carry a body.
 * @param update Update being written, for the parameters that carry field values.
 * @return True when every bit and body fit.
 */
[[nodiscard]] bool write_mask_bits(bits::Writer& writer,
                                   std::uint64_t mask,
                                   std::uint64_t groups,
                                   bool carriesBodies,
                                   const ParameterUpdate& update) noexcept {
    for (std::uint8_t parameter = 0; parameter < kParameterCount; ++parameter) {
        if ((groups & (1ULL << (parameter / kGroupSize))) == 0) {
            continue;
        }
        const bool named = ((mask >> parameter) & 1ULL) != 0;
        if (!writer.write(named ? 1U : 0U, kFlagWidth)) {
            return false;
        }
        // A body must follow its own bit. The consumer reads them interleaved, not in two runs.
        if (named && carriesBodies && !write_parameter_body(writer, parameter, update)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Reads the header of a parameter request. */
bool read_parameter_request(bits::Reader& reader, ParameterRequestHeader& output) noexcept {
    ParameterRequestHeader candidate{};
    std::uint64_t mode = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kFlagWidth, mode)
        || !bits::read_raw_u64(reader, candidate.requestedMask)) {
        return false;
    }
    candidate.modeFlag = mode != 0;
    output = candidate;
    return true;
}

/** Writes a parameter update. */
bool write_parameter_update(bits::Writer& writer, const ParameterUpdate& body) noexcept {
    const std::uint64_t released = body.releasedMask & kParameterMaskBits;
    const std::uint64_t carried = body.carriedMask & kParameterMaskBits;
    if ((carried & ~kEncodableParameters) != 0) {
        return false;
    }
    // Field order differs from the request. The flag leads here, and the session id follows it.
    return writer.write(body.resetFlag ? 1U : 0U, kFlagWidth)
           && bits::write_raw_u64(writer, body.sessionId)
           && writer.write(group_presence(released), kGroupCount)
           && writer.write(group_presence(carried), kGroupCount)
           && write_mask_bits(writer, released, group_presence(released), false, body)
           && write_mask_bits(writer, carried, group_presence(carried), true, body);
}

/** Reports whether one parameter was requested. */
bool requests(std::uint64_t mask, std::uint8_t parameter) noexcept {
    if (parameter >= kParameterCount) {
        return false;
    }
    return ((mask >> parameter) & 1U) != 0;
}

} // namespace sunrise::middleware::gameplay::group
