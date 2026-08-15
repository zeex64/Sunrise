#include <Windows.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "../../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

constexpr std::wstring_view kRequestSuffix = L"\\ornament_extract_request.txt";
constexpr std::wstring_view kOutputSuffix = L"\\ornament_extract";

[[nodiscard]] bool owner_module(HMODULE& module) noexcept {
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    return GetModuleHandleExW(flags,
                              reinterpret_cast<LPCWSTR>(&load_ornament_extraction_request),
                              &module)
           != FALSE;
}

[[nodiscard]] bool write_file(std::wstring_view path, std::span<const std::byte> bytes) noexcept {
    if (path.empty() || path.size() >= core::path::kExtendedPathCapacity
        || bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    std::wstring terminated(path);
    const HANDLE file = CreateFileW(terminated.c_str(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool success =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE
        && written == bytes.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return success;
}

[[nodiscard]] bool write_text(std::wstring_view path, const std::string& text) noexcept {
    return write_file(path,
                      {reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

[[nodiscard]] std::wstring output_path(const OrnamentExtractionRequest& request,
                                       const wchar_t* leaf) {
    std::wstring path(request.outputDirectory.chars.data(), request.outputDirectory.length);
    path.push_back(L'\\');
    path.append(leaf);
    return path;
}

[[nodiscard]] bool dump_blob(const OrnamentExtractionRequest& request,
                             const wchar_t* leaf,
                             std::span<const std::byte> bytes) noexcept {
    return write_file(output_path(request, leaf), bytes);
}

void append_table(std::string& manifest,
                  const char* tree,
                  std::size_t slot,
                  std::uint32_t tag,
                  std::uint32_t classId,
                  std::size_t size,
                  bool readable) {
    std::array<char, 256> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "%s,%zu,0x%08X,0x%08X,%zu,%s\n",
                                    tree,
                                    slot,
                                    tag,
                                    classId,
                                    size,
                                    readable ? "ok" : "unreadable");
    if (count > 0) {
        manifest.append(line.data(), static_cast<std::size_t>(count));
    }
}

void dump_tree(const OrnamentExtractionRequest& request,
               const reader::Source& source,
               reader::Scratch& scratch,
               std::span<const std::byte> tree,
               const char* treeName,
               bool globals,
               std::string& manifest) noexcept {
    const std::size_t count = globals ? tables::child_count(tree)
                                      : (tree.size() > 8 ? (tree.size() - 8) / 16 : 0);
    std::vector<std::byte> blob;
    for (std::size_t slot = 0; slot < count; ++slot) {
        std::uint32_t tag = 0;
        const bool hasTag = globals ? tables::child_tag(tree, slot, tag)
                                    : tables::slot_tag(tree, slot, tag);
        if (!hasTag || tables::package_of(tag) == tables::kAbsentPackageId) {
            append_table(manifest, treeName, slot, tag, 0, 0, false);
            continue;
        }
        std::uint32_t classId = 0;
        const bool read = reader::read_tag(source, scratch, tag, blob, classId);
        append_table(manifest, treeName, slot, tag, classId, read ? blob.size() : 0, read);
        if (!read) {
            continue;
        }
        std::array<wchar_t, 128> leaf{};
        const int length = std::swprintf(leaf.data(),
                                         leaf.size(),
                                         L"%hs_%03zu_0x%08X_0x%08X.bin",
                                         treeName,
                                         slot,
                                         tag,
                                         classId);
        if (length > 0) {
            (void)dump_blob(request, leaf.data(), blob);
        }
    }
}

} // namespace

/** Loads one optional, module-relative extraction request containing a hexadecimal item hash. */
bool load_ornament_extraction_request(OrnamentExtractionRequest& request) noexcept {
    request = {};
    HMODULE module{};
    core::path::Buffer artifact{};
    if (!owner_module(module) || !core::path::artifact_directory(module, artifact)) {
        return false;
    }
    core::path::Buffer requestPath = artifact;
    if (!core::path::append(requestPath, kRequestSuffix)) {
        return false;
    }
    const HANDLE file = CreateFileW(requestPath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<char, 512> text{};
    DWORD read = 0;
    const bool loaded = ReadFile(file, text.data(), text.size() - 1, &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!loaded || read == 0) {
        return false;
    }
    text[read] = '\0';
    char* cursor = text.data();
    while (*cursor != '\0' && request.definitionHashCount < request.definitionHashes.size()) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n'
               || *cursor == ',') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        errno = 0;
        char* end = nullptr;
        const unsigned long value = std::strtoul(cursor, &end, 0);
        if (errno != 0 || end == cursor
            || value > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        request.definitionHashes[request.definitionHashCount++] =
            static_cast<std::uint32_t>(value);
        cursor = end;
    }
    if (request.definitionHashCount == 0) {
        return false;
    }
    request.outputDirectory = artifact;
    if (!core::path::append(request.outputDirectory, kOutputSuffix)
        || (CreateDirectoryW(request.outputDirectory.chars.data(), nullptr) == FALSE
            && GetLastError() != ERROR_ALREADY_EXISTS)) {
        request = {};
        return false;
    }
    request.enabled = true;
    return true;
}

/** Tests one item hash against the optional multi-item extraction request. */
bool ornament_extraction_requested(const OrnamentExtractionRequest& request,
                                   std::uint32_t definitionHash) noexcept {
    if (!request.enabled) {
        return false;
    }
    for (std::size_t index = 0; index < request.definitionHashCount; ++index) {
        if (request.definitionHashes[index] == definitionHash) {
            return true;
        }
    }
    return false;
}

/** Resolves every requested definition from the native item table before the normal item walk. */
void extract_requested_ornament_graphs(const OrnamentExtractionRequest& request,
                                       const reader::Source& source,
                                       reader::Scratch& scratch,
                                       const tables::Array& itemArray,
                                       std::span<const std::byte> globals,
                                       std::span<const std::byte> root,
                                       std::span<const std::byte> itemTable) noexcept {
    if (!request.enabled) {
        return;
    }
    std::vector<std::byte> definition;
    for (std::uint64_t index = 0; index < itemArray.count; ++index) {
        tables::IndexRow indexRow{};
        if (!tables::index_row(itemTable, itemArray, index, indexRow)
            || !ornament_extraction_requested(request, indexRow.definitionHash)) {
            continue;
        }
        tables::items::Row item{};
        item.definitionHash = indexRow.definitionHash;
        item.definitionIndex = static_cast<std::uint16_t>(index);
        if (!reader::read_tag(source, scratch, indexRow.targetTag, definition)
            || !tables::items::read_definition(definition, item)) {
            continue;
        }
        extract_ornament_graph(request,
                               source,
                               scratch,
                               indexRow,
                               item,
                               definition,
                               globals,
                               root,
                               itemTable);
    }
}

/** Exports the selected native item and every investment table reachable from both roots. */
void extract_ornament_graph(const OrnamentExtractionRequest& request,
                            const reader::Source& source,
                            reader::Scratch& scratch,
                            const tables::IndexRow& indexRow,
                            const tables::items::Row& item,
                            std::span<const std::byte> definition,
                            std::span<const std::byte> globals,
                            std::span<const std::byte> root,
                            std::span<const std::byte> itemTable) noexcept {
    if (!ornament_extraction_requested(request, item.definitionHash)) {
        return;
    }
    OrnamentExtractionRequest itemRequest = request;
    std::array<wchar_t, 32> itemDirectory{};
    const int directoryLength = std::swprintf(
        itemDirectory.data(), itemDirectory.size(), L"\\0x%08X", item.definitionHash);
    if (directoryLength <= 0
        || !core::path::append(itemRequest.outputDirectory, itemDirectory.data())
        || (CreateDirectoryW(itemRequest.outputDirectory.chars.data(), nullptr) == FALSE
            && GetLastError() != ERROR_ALREADY_EXISTS)) {
        return;
    }
    bool success = dump_blob(itemRequest, L"definition.bin", definition)
                   && dump_blob(itemRequest, L"investment_globals.bin", globals)
                   && dump_blob(itemRequest, L"investment_root.bin", root)
                   && dump_blob(itemRequest, L"item_table.bin", itemTable);

    std::array<char, 768> itemJson{};
    const int itemLength = std::snprintf(
        itemJson.data(),
        itemJson.size(),
        "{\n  \"definition_hash\": \"0x%08X\",\n  \"definition_index\": %u,\n"
        "  \"definition_tag\": \"0x%08X\",\n  \"bucket_id\": %u,\n"
        "  \"gear_art_index\": %u,\n  \"art_arrangement_index\": %u,\n"
        "  \"socket_entry_list_index\": %u,\n  \"socket_count\": %u\n}\n",
        item.definitionHash,
        static_cast<unsigned>(item.definitionIndex),
        indexRow.targetTag,
        static_cast<unsigned>(item.bucketId),
        static_cast<unsigned>(item.gearArtIndex),
        static_cast<unsigned>(item.artArrangementIndex),
        static_cast<unsigned>(item.socketEntryListIndex),
        static_cast<unsigned>(item.socketCount));
    success = itemLength > 0
              && write_text(output_path(itemRequest, L"item.json"),
                            std::string(itemJson.data(), static_cast<std::size_t>(itemLength)))
              && success;

    std::string manifest = "tree,slot,tag,class,size,status\n";
    dump_tree(itemRequest, source, scratch, globals, "globals", true, manifest);
    dump_tree(itemRequest, source, scratch, root, "root", false, manifest);
    success = write_text(output_path(itemRequest, L"tables.csv"), manifest) && success;
    core::log::write(core::log::Channel::client,
                     success ? core::log::Level::info : core::log::Level::error,
                     success ? "ev=ornament_extract result=ok"
                             : "ev=ornament_extract result=partial");
}

} // namespace sunrise::client::content::items::packages
