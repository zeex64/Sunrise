#include "investment_lookup_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::diagnostics {
namespace {

constexpr std::wstring_view kRequestSuffix = L"\\investment_lookup_probe_request.txt";
constexpr std::wstring_view kOutputSuffix = L"\\investment_lookup_probe.bin";
constexpr std::size_t kWindowPrefix = 64;
constexpr std::size_t kWindowSize = 192;
constexpr std::uint32_t kMagic = 0x504C4953U;

#pragma pack(push, 1)
struct Header {
    std::uint32_t magic{kMagic};
    std::uint32_t version{1};
    std::uint64_t imageBase{};
    std::uint32_t recordCount{};
    std::uint32_t recordSize{};
};

struct Record {
    std::uint64_t matchRva{};
    std::uint32_t matchOffset{};
    std::uint32_t byteCount{};
    std::array<std::byte, kWindowSize> bytes{};
};
#pragma pack(pop)

[[nodiscard]] bool requested(core::path::Buffer& outputPath) noexcept {
    HMODULE owner{};
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags,
                           reinterpret_cast<LPCWSTR>(&capture_investment_lookup_candidates),
                           &owner)
            == FALSE
        || !core::path::artifact_directory(owner, outputPath)) {
        return false;
    }
    core::path::Buffer requestPath = outputPath;
    if (!core::path::append(requestPath, kRequestSuffix)
        || GetFileAttributesW(requestPath.chars.data()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return core::path::append(outputPath, kOutputSuffix);
}

[[nodiscard]] bool registry_call(std::span<const std::byte> section,
                                 std::size_t offset) noexcept {
    if (offset + 6 > section.size()) {
        return false;
    }
    const auto byte = [&section](std::size_t at) {
        return std::to_integer<std::uint8_t>(section[at]);
    };
    // call qword ptr [register + 0x558], including every base-register ModRM value.
    return byte(offset) == 0xFFU && (byte(offset + 1) & 0xF8U) == 0x90U
           && byte(offset + 2) == 0x58U && byte(offset + 3) == 0x05U
           && byte(offset + 4) == 0x00U && byte(offset + 5) == 0x00U;
}

[[nodiscard]] bool write_capture(std::wstring_view path,
                                 std::span<const std::byte> bytes) noexcept {
    std::wstring terminated(path);
    const HANDLE file = CreateFileW(terminated.c_str(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE || bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        return false;
    }
    DWORD written = 0;
    const bool success = WriteFile(file,
                                   bytes.data(),
                                   static_cast<DWORD>(bytes.size()),
                                   &written,
                                   nullptr)
                             != FALSE
                         && written == bytes.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return success;
}

} // namespace

/** Captures small code windows around item-registry vtable calls when explicitly requested. */
void capture_investment_lookup_candidates(const executable::ExecutableImage& image) noexcept {
    core::path::Buffer outputPath{};
    if (!requested(outputPath)) {
        return;
    }
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::vector<Record> records;
    for (std::size_t sectionIndex = 0; sectionIndex < image.count; ++sectionIndex) {
        const std::span<std::byte> section = image.sections[sectionIndex];
        for (std::size_t offset = 0; offset < section.size(); ++offset) {
            if (!registry_call(section, offset)) {
                continue;
            }
            const std::size_t start = offset > kWindowPrefix ? offset - kWindowPrefix : 0;
            const std::size_t available = section.size() - start;
            Record record{};
            record.matchRva = reinterpret_cast<std::uintptr_t>(section.data() + offset) - imageBase;
            record.matchOffset = static_cast<std::uint32_t>(offset - start);
            record.byteCount = static_cast<std::uint32_t>(
                (std::min)(available, record.bytes.size()));
            std::memcpy(record.bytes.data(), section.data() + start, record.byteCount);
            records.push_back(record);
        }
    }

    Header header{};
    header.imageBase = imageBase;
    header.recordCount = static_cast<std::uint32_t>(records.size());
    header.recordSize = sizeof(Record);
    std::vector<std::byte> output(sizeof header + records.size() * sizeof(Record));
    std::memcpy(output.data(), &header, sizeof header);
    if (!records.empty()) {
        std::memcpy(output.data() + sizeof header,
                    records.data(),
                    records.size() * sizeof(Record));
    }
    const bool success = write_capture(
        std::wstring_view(outputPath.chars.data(), outputPath.length), output);
    core::log::write(core::log::Channel::client,
                     success ? core::log::Level::info : core::log::Level::warn,
                     success ? "ev=ornament_lookup_probe result=ok"
                             : "ev=ornament_lookup_probe result=fail");
}

} // namespace sunrise::client::diagnostics
