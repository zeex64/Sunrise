#!/usr/bin/env python3
"""Author and validate plaintext Destiny 2 pre-Beyond-Light Tiger packages.

This deliberately implements the simple authoring route: version 38, Windows platform 2,
pre-BL metadata directories, uncompressed and unencrypted blocks, and patch-local payloads.
It does not forge Bungie's RSA-PSS header signature. A real 256-byte signature may be supplied,
or the output may be used with a loader that explicitly permits unsigned custom packages.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import time
from dataclasses import dataclass
from pathlib import Path


VERSION = 38
PLATFORM = 2
HEADER_SIZE = 0x170
HEADER_SIGNATURE_OFFSET = 0x800
HEADER_SIGNATURE_SIZE = 0x100
# Version-38 packages describe the portion of the file following the reserved
# 0x800-byte header/signature area at header offset 0x160.
FILE_DATA_START = 0x800
MISC_OFFSET = 0x1000
MISC_VERSION = 3
MISC_NAMED_TABLE_OFFSET = 0x80
MISC_RECORD_MINIMUM_ALLOCATION = 8
MISC_ARRAY_MARKER = 0x80809FBD00000000
MISC_ALLOCATION_MARKER = 0x0000000080809EEC
MISC_STRING_MARKER = 0x8080006500000000
METADATA_OFFSET = 0x1800
METADATA_PREFIX_SIZE = 0x60
ENTRY_RECORD_SIZE = 0x10
ENTRY_BLOCK_GAP = 0x20
BLOCK_RECORD_SIZE = 0x30
BLOCK_SIZE = 0x40000
DATA_ALIGNMENT = 0x1000
FILE_ALIGNMENT = 0x1000
LOCALE_TOKEN_ZERO = 0x281141FD
# Every installed Windows version-38 package carries this fixed header word. Leaving it zero
# causes the native registrar to reject the package before header-signature verification.
HEADER_FORMAT_MARKER = 2
CUSTOM_PACKAGE_MIN = 0xAA0
CUSTOM_PACKAGE_MAX = 0xCFF
MAX_ENTRIES = 8192
MAX_PATCH = 0xFF
CUSTOM_ORNAMENT_CLASS = 0x53554E4F
CUSTOM_TEXTURE_CLASS = 0x53554E54
CUSTOM_RESOURCE_CLASS = 0x53554E52
ITEM_DEFINITION_CLASS = 0x80807BEA
ORNAMENT_MAGIC = 0x4F4E5553
ORNAMENT_VERSION = 1
ORNAMENT_RECORD_SIZE = 96
ORNAMENT_NAME_CAPACITY = 64
ORNAMENT_RESOURCE_CAPACITY = 16
ORNAMENT_RESOURCE_ROLE_CAPACITY = 32
ORNAMENT_RESOURCE_RECORD_SIZE = 36
ORNAMENT_DEFINITION_HASH_OFFSET = 0xA0
ITEM_SOCKET_BLOCK_RELATIVE_OFFSET = 0x68
ITEM_TABLE_ROOT_TAG_OFFSET = 0x308
GLOBALS_ROOT_TAG_OFFSET = 0x10
TABLE_ROW_COUNT_OFFSET = 0x08
TABLE_ARRAY_COUNT_OFFSET = 0x20
TABLE_FIRST_ROW_OFFSET = 0x30
ITEM_TABLE_ROW_SIZE = 0x18
ITEM_TABLE_ROW_TARGET_OFFSET = 0x10
SOCKET_ARRAY_FIRST_ENTRY_OFFSET = 0x10
SOCKET_ENTRY_SIZE = 0x50
SOCKET_OPTION_ARRAY_OFFSET = 0x40
SOCKET_OPTION_RELATIVE_OFFSET = 0x48
SOCKET_OPTION_SIZE = 0x20
ARRAY_MARKER = 0x80809FBD
INVESTMENT_GLOBALS_CLASS = 0x80805BB1
INVESTMENT_ROOT_CLASS = 0x80807D84
ITEM_TABLE_CLASS = 0x80807BE4
NATIVE_TYPE_INFO = 0x100B
GLOBALS_TYPE_INFO = 0x200B
NO_ENTRY = 0xFFFF
ORNAMENT_HAS_TEXTURE = 1 << 0
PACKAGE_RE = re.compile(
    r"^(?P<prefix>[a-z0-9_]+)_(?P<package>[0-9a-f]{4})(?:_[a-z]{2,3})?_(?P<patch>[0-9]+)\.pkg$"
)

# The public value at header +0x08 is the package identity advertised through ContentConfig.
# Keeping it fixed while rebuilding different bytes lets the Client reuse private validation
# state from an older build of the same package. "auto" derives it from the complete authored
# header, directories, metadata and payload so every material package change has a new identity.
GROUP_ID_AUTO = "auto"
GROUP_ID_DOMAIN = b"SunriseTigerPackageIdentity\0\x01"


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def integer(value: object, field: str) -> int:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        return int(value, 0)
    raise ValueError(f"{field} must be an integer or an integer string")


def put(blob: bytearray, offset: int, layout: str, *values: int) -> None:
    struct.pack_into("<" + layout, blob, offset, *values)


@dataclass(frozen=True)
class Entry:
    reference: int
    type_info: int
    payload: bytes
    source: str


@dataclass(frozen=True)
class OrnamentLink:
    row: dict
    definition_entry: int


@dataclass(frozen=True)
class Placement:
    start_block: int
    start_offset: int
    size: int

    def encode(self) -> int:
        if self.start_block >= (1 << 14):
            raise ValueError("entry start block exceeds the 14-bit package field")
        if self.start_offset % 16 != 0 or self.start_offset >= BLOCK_SIZE:
            raise ValueError("entry start offset is not representable")
        if self.size >= (1 << 36):
            raise ValueError("entry size exceeds the 36-bit package field")
        return self.start_block | ((self.start_offset >> 4) << 14) | (self.size << 28)


def ornament_record(
    definition_hash: int,
    target_item_hash: int,
    template_item_hash: int,
    definition_entry: int,
    texture_entry: int,
    socket_type: int,
    socket_lane: int,
    name: str,
    resources: list[tuple[int, str]],
) -> bytes:
    encoded_name = name.encode("utf-8")
    if not encoded_name or len(encoded_name) >= ORNAMENT_NAME_CAPACITY:
        raise ValueError(f"ornament name must encode to 1..{ORNAMENT_NAME_CAPACITY - 1} UTF-8 bytes")
    if not 0 <= definition_entry < NO_ENTRY:
        raise ValueError("ornament definition entry is not representable")
    if texture_entry != NO_ENTRY and not 0 <= texture_entry < NO_ENTRY:
        raise ValueError("ornament texture entry is not representable")
    if not 0 <= socket_type <= 0xFFFF or not 0 <= socket_lane <= 0xFF:
        raise ValueError("ornament socket type or lane is not representable")
    flags = ORNAMENT_HAS_TEXTURE if texture_entry != NO_ENTRY else 0
    if len(resources) > ORNAMENT_RESOURCE_CAPACITY:
        raise ValueError(f"ornament may declare at most {ORNAMENT_RESOURCE_CAPACITY} resources")
    name_field = encoded_name + bytes(ORNAMENT_NAME_CAPACITY - len(encoded_name))
    record = bytearray(struct.pack(
        "<IHHIIHHHBBH64sIH",
        ORNAMENT_MAGIC,
        ORNAMENT_VERSION,
        ORNAMENT_RECORD_SIZE,
        definition_hash,
        target_item_hash,
        definition_entry,
        texture_entry,
        socket_type,
        socket_lane,
        flags,
        0,
        name_field,
        template_item_hash,
        len(resources),
    ))
    for entry_index, role in resources:
        encoded_role = role.encode("utf-8")
        if not encoded_role or len(encoded_role) >= ORNAMENT_RESOURCE_ROLE_CAPACITY:
            raise ValueError(
                f"resource role must encode to 1..{ORNAMENT_RESOURCE_ROLE_CAPACITY - 1} UTF-8 bytes"
            )
        if not 0 <= entry_index < NO_ENTRY:
            raise ValueError("ornament resource entry is not representable")
        role_field = encoded_role + bytes(ORNAMENT_RESOURCE_ROLE_CAPACITY - len(encoded_role))
        record.extend(struct.pack("<HH32s", entry_index, 0, role_field))
    return bytes(record)


def load_ornaments(path: Path, rows: list[object]) -> tuple[list[Entry], list[OrnamentLink]]:
    entries: list[Entry] = []
    links: list[OrnamentLink] = []
    for ornament_index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"ornaments[{ornament_index}] must be an object")
        name = row.get("name")
        definition_source = row.get("definition_template")
        if not isinstance(name, str) or not name:
            raise ValueError(f"ornaments[{ornament_index}].name must be a nonempty string")
        if not isinstance(definition_source, str) or not definition_source:
            raise ValueError(
                f"ornaments[{ornament_index}].definition_template must be a nonempty path"
            )
        definition_hash = integer(
            row.get("definition_hash"), f"ornaments[{ornament_index}].definition_hash"
        ) & 0xFFFFFFFF
        target_item_hash = integer(
            row.get("target_item_hash"), f"ornaments[{ornament_index}].target_item_hash"
        ) & 0xFFFFFFFF
        template_item_hash = integer(
            row.get("template_item_hash"), f"ornaments[{ornament_index}].template_item_hash"
        ) & 0xFFFFFFFF
        socket_type = integer(
            row.get("socket_type"), f"ornaments[{ornament_index}].socket_type"
        )
        socket_lane = integer(
            row.get("socket_lane"), f"ornaments[{ornament_index}].socket_lane"
        )
        template_path = (path.parent / definition_source).resolve()
        definition = bytearray(template_path.read_bytes())
        if len(definition) < ORNAMENT_DEFINITION_HASH_OFFSET + 4:
            raise ValueError(f"ornaments[{ornament_index}] definition template is too short")
        put(definition, ORNAMENT_DEFINITION_HASH_OFFSET, "I", definition_hash)

        descriptor_entry = len(entries)
        definition_entry = descriptor_entry + 1
        resource_rows = row.get("resources", [])
        if not isinstance(resource_rows, list):
            raise ValueError(f"ornaments[{ornament_index}].resources must be an array")
        normalized_resources: list[dict] = []
        texture_source = row.get("texture")
        if texture_source is not None:
            normalized_resources.append(
                {
                    "role": "diffuse_rgba",
                    "path": texture_source,
                    "reference": row.get("texture_reference", CUSTOM_TEXTURE_CLASS),
                    "type_info": row.get("texture_type_info", 0),
                }
            )
        for resource_index, resource in enumerate(resource_rows):
            if not isinstance(resource, dict):
                raise ValueError(
                    f"ornaments[{ornament_index}].resources[{resource_index}] must be an object"
                )
            normalized_resources.append(resource)
        if len(normalized_resources) > ORNAMENT_RESOURCE_CAPACITY:
            raise ValueError(
                f"ornaments[{ornament_index}] exceeds the {ORNAMENT_RESOURCE_CAPACITY}-resource limit"
            )
        resource_entries: list[Entry] = []
        resource_links: list[tuple[int, str]] = []
        for resource_index, resource in enumerate(normalized_resources):
            role = resource.get("role")
            resource_source = resource.get("path")
            if not isinstance(role, str) or not role or not isinstance(resource_source, str) or not resource_source:
                raise ValueError(
                    f"ornaments[{ornament_index}].resources[{resource_index}] needs role and path"
                )
            entry_index = definition_entry + 1 + len(resource_entries)
            resource_links.append((entry_index, role))
            resource_entries.append(
                Entry(
                    integer(
                        resource.get("reference", CUSTOM_RESOURCE_CLASS),
                        f"ornaments[{ornament_index}].resources[{resource_index}].reference",
                    )
                    & 0xFFFFFFFF,
                    integer(
                        resource.get("type_info", 0),
                        f"ornaments[{ornament_index}].resources[{resource_index}].type_info",
                    )
                    & 0xFFFFFFFF,
                    (path.parent / resource_source).resolve().read_bytes(),
                    resource_source,
                )
            )
        texture_entry = resource_links[0][0] if texture_source is not None else NO_ENTRY
        descriptor = ornament_record(
            definition_hash,
            target_item_hash,
            template_item_hash,
            definition_entry,
            texture_entry,
            socket_type,
            socket_lane,
            name,
            resource_links,
        )
        entries.append(Entry(CUSTOM_ORNAMENT_CLASS, 0, descriptor, f"ornament:{name}"))
        entries.append(
            Entry(
                integer(
                    row.get("definition_reference", ITEM_DEFINITION_CLASS),
                    f"ornaments[{ornament_index}].definition_reference",
                )
                & 0xFFFFFFFF,
                integer(
                    row.get("definition_type_info", 0),
                    f"ornaments[{ornament_index}].definition_type_info",
                )
                & 0xFFFFFFFF,
                bytes(definition),
                definition_source,
            )
        )
        entries.extend(resource_entries)
        links.append(OrnamentLink(row, definition_entry))
    return entries, links


def entry_tag(package_id: int, entry_index: int) -> int:
    if not 0 <= entry_index < MAX_ENTRIES:
        raise ValueError("package entry index is outside the 13-bit tag field")
    return (0x80800000 + (package_id << 13) + entry_index) & 0xFFFFFFFF


def item_rows(blob: bytes) -> tuple[int, dict[int, tuple[int, bytes]]]:
    if len(blob) < TABLE_FIRST_ROW_OFFSET:
        raise ValueError("investment item table is too short")
    count = struct.unpack_from("<Q", blob, TABLE_ROW_COUNT_OFFSET)[0]
    repeated = struct.unpack_from("<Q", blob, TABLE_ARRAY_COUNT_OFFSET)[0]
    if count == 0 or count != repeated or TABLE_FIRST_ROW_OFFSET + count * ITEM_TABLE_ROW_SIZE != len(blob):
        raise ValueError("investment item table has an unsupported dense layout")
    rows: dict[int, tuple[int, bytes]] = {}
    for index in range(count):
        offset = TABLE_FIRST_ROW_OFFSET + index * ITEM_TABLE_ROW_SIZE
        row = blob[offset : offset + ITEM_TABLE_ROW_SIZE]
        definition_hash = struct.unpack_from("<I", row, 0)[0]
        if definition_hash in rows:
            raise ValueError("investment item table contains a duplicate definition hash")
        rows[definition_hash] = (index, row)
    return count, rows


def expand_target_definition(
    source: bytes,
    socket_lane: int,
    socket_type: int,
    template_indices: list[int],
    custom_indices: list[int],
) -> bytes:
    definition = bytearray(source)
    if len(definition) < ITEM_SOCKET_BLOCK_RELATIVE_OFFSET + 8:
        raise ValueError("target item definition is too short for its socket block")
    block_relative = struct.unpack_from("<q", definition, ITEM_SOCKET_BLOCK_RELATIVE_OFFSET)[0]
    socket_block = ITEM_SOCKET_BLOCK_RELATIVE_OFFSET + block_relative
    if socket_block < 0 or socket_block + 16 > len(definition):
        raise ValueError("target item socket block is invalid")
    socket_count, header_relative = struct.unpack_from("<Qq", definition, socket_block)
    socket_header = socket_block + 8 + header_relative
    lane_offset = socket_header + SOCKET_ARRAY_FIRST_ENTRY_OFFSET + socket_lane * SOCKET_ENTRY_SIZE
    if socket_lane >= socket_count or lane_offset + SOCKET_ENTRY_SIZE > len(definition):
        raise ValueError("ornament socket lane is outside the target definition")
    if struct.unpack_from("<H", definition, lane_offset)[0] != socket_type:
        raise ValueError("ornament socket type does not match the target definition")

    option_count = struct.unpack_from("<Q", definition, lane_offset + SOCKET_OPTION_ARRAY_OFFSET)[0]
    option_relative = struct.unpack_from("<q", definition, lane_offset + SOCKET_OPTION_RELATIVE_OFFSET)[0]
    option_header = lane_offset + SOCKET_OPTION_RELATIVE_OFFSET + option_relative
    option_bytes = option_count * SOCKET_OPTION_SIZE
    if (
        option_count == 0
        or option_header < 4
        or option_header + SOCKET_ARRAY_FIRST_ENTRY_OFFSET + option_bytes > len(definition)
        or struct.unpack_from("<I", definition, option_header - 4)[0] != ARRAY_MARKER
        or struct.unpack_from("<Q", definition, option_header)[0] != option_count
    ):
        raise ValueError("target ornament option array is invalid")
    option_class = struct.unpack_from("<I", definition, option_header + 8)[0]
    old_options = [
        bytes(
            definition[
                option_header + SOCKET_ARRAY_FIRST_ENTRY_OFFSET + index * SOCKET_OPTION_SIZE :
                option_header + SOCKET_ARRAY_FIRST_ENTRY_OFFSET + (index + 1) * SOCKET_OPTION_SIZE
            ]
        )
        for index in range(option_count)
    ]
    additions: list[bytes] = []
    for template_index, custom_index in zip(template_indices, custom_indices):
        template = next(
            (record for record in old_options if struct.unpack_from("<H", record, 0)[0] == template_index),
            None,
        )
        if template is None:
            raise ValueError("template ornament is not present in the target socket option list")
        added = bytearray(template)
        put(added, 0, "H", custom_index)
        additions.append(bytes(added))

    new_count = option_count + len(additions)
    new_header = align(len(definition) + 4, 16)
    definition.extend(bytes(new_header - len(definition)))
    put(definition, new_header - 4, "I", ARRAY_MARKER)
    definition.extend(bytes(SOCKET_ARRAY_FIRST_ENTRY_OFFSET + new_count * SOCKET_OPTION_SIZE))
    put(definition, new_header, "Q", new_count)
    put(definition, new_header + 8, "I", option_class)
    cursor = new_header + SOCKET_ARRAY_FIRST_ENTRY_OFFSET
    for record in old_options + additions:
        definition[cursor : cursor + SOCKET_OPTION_SIZE] = record
        cursor += SOCKET_OPTION_SIZE
    put(definition, lane_offset + SOCKET_OPTION_ARRAY_OFFSET, "Q", new_count)
    put(
        definition,
        lane_offset + SOCKET_OPTION_RELATIVE_OFFSET,
        "q",
        new_header - (lane_offset + SOCKET_OPTION_RELATIVE_OFFSET),
    )
    put(definition, 0, "Q", len(definition))
    return bytes(definition)


def append_investment_overlay(
    path: Path, document: dict, entries: list[Entry], links: list[OrnamentLink]
) -> None:
    overlay = document.get("investment_overlay")
    if overlay is None:
        return
    if not isinstance(overlay, dict):
        raise ValueError("investment_overlay must be an object")
    package_id = integer(document.get("package_id"), "package_id")
    required = ("globals_template", "root_template", "item_table_template")
    for field in required:
        if not isinstance(overlay.get(field), str) or not overlay[field]:
            raise ValueError(f"investment_overlay.{field} must be a nonempty path")
    globals_blob = bytearray((path.parent / overlay["globals_template"]).resolve().read_bytes())
    root_blob = bytearray((path.parent / overlay["root_template"]).resolve().read_bytes())
    table_blob = bytearray((path.parent / overlay["item_table_template"]).resolve().read_bytes())
    native_count, native_rows = item_rows(table_blob)
    if native_count + len(links) >= NO_ENTRY:
        raise ValueError("custom ornament indices exceed the native 16-bit item table")

    grouped: dict[int, list[tuple[OrnamentLink, int, int]]] = {}
    for position, link in enumerate(links):
        row = link.row
        target_hash = integer(row.get("target_item_hash"), "target_item_hash") & 0xFFFFFFFF
        template_hash = integer(row.get("template_item_hash"), "template_item_hash") & 0xFFFFFFFF
        if target_hash not in native_rows or template_hash not in native_rows:
            raise ValueError("ornament target or template hash is absent from the native item table")
        custom_index = native_count + position
        grouped.setdefault(target_hash, []).append(
            (link, native_rows[template_hash][0], custom_index)
        )

    replacement_entries: dict[int, int] = {}
    for target_hash, ornaments in grouped.items():
        template_name = ornaments[0][0].row.get("target_definition_template")
        if not isinstance(template_name, str) or not template_name:
            raise ValueError("each overlaid ornament needs target_definition_template")
        for ornament, _, _ in ornaments[1:]:
            if ornament.row.get("target_definition_template") != template_name:
                raise ValueError("ornaments sharing a target must share one target definition template")
        source = (path.parent / template_name).resolve().read_bytes()
        expanded = expand_target_definition(
            source,
            integer(ornaments[0][0].row.get("socket_lane"), "socket_lane"),
            integer(ornaments[0][0].row.get("socket_type"), "socket_type"),
            [template_index for _, template_index, _ in ornaments],
            [custom_index for _, _, custom_index in ornaments],
        )
        replacement_entries[target_hash] = len(entries)
        entries.append(Entry(ITEM_DEFINITION_CLASS, NATIVE_TYPE_INFO, expanded, template_name))

    table_entry = len(entries)
    for target_hash, target_entry in replacement_entries.items():
        target_index = native_rows[target_hash][0]
        put(
            table_blob,
            TABLE_FIRST_ROW_OFFSET + target_index * ITEM_TABLE_ROW_SIZE + ITEM_TABLE_ROW_TARGET_OFFSET,
            "I",
            entry_tag(package_id, target_entry),
        )
    for position, link in enumerate(links):
        definition_hash = integer(link.row.get("definition_hash"), "definition_hash") & 0xFFFFFFFF
        template_hash = integer(link.row.get("template_item_hash"), "template_item_hash") & 0xFFFFFFFF
        row = bytearray(native_rows[template_hash][1])
        put(row, 0, "I", definition_hash)
        put(row, ITEM_TABLE_ROW_TARGET_OFFSET, "I", entry_tag(package_id, link.definition_entry))
        table_blob.extend(row)
    put(table_blob, 0, "Q", len(table_blob))
    put(table_blob, TABLE_ROW_COUNT_OFFSET, "Q", native_count + len(links))
    put(table_blob, TABLE_ARRAY_COUNT_OFFSET, "Q", native_count + len(links))
    entries.append(Entry(ITEM_TABLE_CLASS, NATIVE_TYPE_INFO, bytes(table_blob), overlay["item_table_template"]))

    root_entry = len(entries)
    if len(root_blob) < ITEM_TABLE_ROOT_TAG_OFFSET + 4:
        raise ValueError("investment root template is too short")
    put(root_blob, ITEM_TABLE_ROOT_TAG_OFFSET, "I", entry_tag(package_id, table_entry))
    entries.append(Entry(INVESTMENT_ROOT_CLASS, NATIVE_TYPE_INFO, bytes(root_blob), overlay["root_template"]))

    globals_entry = len(entries)
    if len(globals_blob) < GLOBALS_ROOT_TAG_OFFSET + 4:
        raise ValueError("investment globals template is too short")
    put(globals_blob, GLOBALS_ROOT_TAG_OFFSET, "I", entry_tag(package_id, root_entry))
    entries.append(Entry(INVESTMENT_GLOBALS_CLASS, GLOBALS_TYPE_INFO, bytes(globals_blob), overlay["globals_template"]))
    publish_named_tag = overlay.get("publish_named_tag", True)
    if not isinstance(publish_named_tag, bool):
        raise ValueError("investment_overlay.publish_named_tag must be a boolean")
    if not publish_named_tag:
        return
    named = document.setdefault("named_tags", [])
    if not isinstance(named, list):
        raise ValueError("named_tags must be an array")
    named_row = {
        "name": "investment_globals",
        "entry": globals_entry,
        "class_id": INVESTMENT_GLOBALS_CLASS,
    }
    named_tag_target = overlay.get("named_tag_target")
    if named_tag_target is not None:
        named_row["tag"] = named_tag_target
    named.append(named_row)


def load_manifest(path: Path) -> tuple[dict, list[Entry]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("manifest root must be an object")
    ornaments = document.get("ornaments")
    if ornaments is not None:
        if not isinstance(ornaments, list) or not ornaments:
            raise ValueError("ornaments must contain at least one row")
        if document.get("entries") is not None:
            raise ValueError("manifest may declare either entries or ornaments, not both")
        entries, links = load_ornaments(path, ornaments)
        append_investment_overlay(path, document, entries, links)
        if len(entries) > MAX_ENTRIES:
            raise ValueError(f"ornament entries exceed the {MAX_ENTRIES}-entry package limit")
        return document, entries
    rows = document.get("entries")
    if not isinstance(rows, list) or not rows or len(rows) > MAX_ENTRIES:
        raise ValueError(f"entries must contain 1..{MAX_ENTRIES} rows")
    entries: list[Entry] = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"entries[{index}] must be an object")
        source = row.get("path")
        if not isinstance(source, str) or not source:
            raise ValueError(f"entries[{index}].path must be a nonempty string")
        source_path = (path.parent / source).resolve()
        payload = source_path.read_bytes()
        entries.append(
            Entry(
                integer(row.get("reference", 0xFFFFFFFF), f"entries[{index}].reference")
                & 0xFFFFFFFF,
                integer(row.get("type_info", 0), f"entries[{index}].type_info")
                & 0xFFFFFFFF,
                payload,
                source,
            )
        )
    return document, entries


def build_misc(document: dict, package_id: int) -> bytes:
    rows = document.get("named_tags", [])
    if not rows:
        # Current packages without named tags omit the extended-header directory. The header still
        # carries SHA-1(empty), matching shipped packages whose offset and size are both zero.
        return b""
    if not isinstance(rows, list) or len(rows) > MAX_ENTRIES:
        raise ValueError("named_tags must be an array of at most 8192 rows")
    record_size = 16
    allocated = MISC_RECORD_MINIMUM_ALLOCATION
    while allocated < len(rows):
        allocated *= 2
    records_offset = MISC_NAMED_TABLE_OFFSET
    strings_directory_offset = records_offset + allocated * record_size
    names_offset = strings_directory_offset + 16
    names = bytearray()
    encoded_rows: list[tuple[int, int, bytes]] = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"named_tags[{index}] must be an object")
        name = row.get("name")
        if not isinstance(name, str) or not name or "\0" in name:
            raise ValueError(f"named_tags[{index}].name must be a nonempty string")
        encoded = name.encode("utf-8") + b"\0"
        explicit_tag = row.get("tag")
        if explicit_tag is None:
            tag = entry_tag(package_id, integer(row.get("entry"), f"named_tags[{index}].entry"))
        else:
            tag = integer(explicit_tag, f"named_tags[{index}].tag") & 0xFFFFFFFF
        class_id = integer(row.get("class_id"), f"named_tags[{index}].class_id") & 0xFFFFFFFF
        encoded_rows.append((tag, class_id, encoded))
        names.extend(encoded)

    logical_size = names_offset + len(names)
    misc = bytearray(align(logical_size, 8))
    put(misc, 0x00, "Q", logical_size)
    put(misc, 0x08, "Q", MISC_VERSION)
    # Native version-38/pre-BL packages place the named-tag relative directory at +0x10.
    put(misc, 0x10, "QQQ", len(rows), records_offset - 0x28, 0)
    # The native dynamic-array bookkeeping immediately precedes the allocated record storage.
    put(misc, records_offset - 0x20, "Q", 0)
    put(misc, records_offset - 0x18, "Q", MISC_ARRAY_MARKER)
    put(misc, records_offset - 0x10, "Q", allocated)
    put(misc, records_offset - 0x08, "Q", MISC_ALLOCATION_MARKER)
    # Native version-3 directories mark unused records with all-ones tag/class fields and a null
    # relative-name pointer. Zero records are parsed as real tag-zero rows; filling the pointer
    # with all ones also fails the native directory validator.
    unused_record = struct.pack("<IIQ", 0xFFFFFFFF, 0xFFFFFFFF, 0)
    misc[records_offset : records_offset + allocated * record_size] = unused_record * allocated
    put(misc, strings_directory_offset, "QQ", MISC_STRING_MARKER, len(names))

    name_cursor = names_offset
    for index, (tag, class_id, encoded) in enumerate(encoded_rows):
        record = records_offset + index * record_size
        put(misc, record, "IIQ", tag, class_id, name_cursor - (record + 8))
        misc[name_cursor : name_cursor + len(encoded)] = encoded
        name_cursor += len(encoded)
    return bytes(misc)


def build_blocks(entries: list[Entry]) -> tuple[list[bytes], list[Placement]]:
    blocks = [bytearray()]
    placements: list[Placement] = []
    for entry in entries:
        current = blocks[-1]
        padded = align(len(current), 16)
        if padded >= BLOCK_SIZE:
            blocks.append(bytearray())
            current = blocks[-1]
            padded = 0
        current.extend(b"\0" * (padded - len(current)))
        placement = Placement(len(blocks) - 1, len(current), len(entry.payload))
        placements.append(placement)
        remaining = memoryview(entry.payload)
        while remaining:
            current = blocks[-1]
            available = BLOCK_SIZE - len(current)
            if available == 0:
                blocks.append(bytearray())
                continue
            take = min(available, len(remaining))
            current.extend(remaining[:take])
            remaining = remaining[take:]
    if not blocks[-1]:
        blocks[-1].append(0)
    return [bytes(block) for block in blocks], placements


def build_metadata(
    entries: list[Entry], placements: list[Placement], blocks: list[bytes], data_offset: int, patch: int
) -> bytes:
    entry_count = len(entries)
    block_count = len(blocks)
    block_table_offset = METADATA_PREFIX_SIZE + entry_count * ENTRY_RECORD_SIZE + ENTRY_BLOCK_GAP
    metadata_size = block_table_offset + block_count * BLOCK_RECORD_SIZE
    metadata = bytearray(metadata_size)

    put(metadata, 0x00, "Q", metadata_size)
    put(metadata, 0x08, "Q", 1)
    put(metadata, 0x10, "Q", entry_count)
    put(metadata, 0x18, "Q", METADATA_PREFIX_SIZE - (0x10 + 0x18))
    put(metadata, 0x20, "Q", block_count)
    put(metadata, 0x28, "Q", block_table_offset - (0x20 + 0x18))
    # The third metadata table is optional for this minimal authoring route.
    put(metadata, 0x30, "Q", 0)
    put(metadata, 0x38, "Q", 0)
    put(metadata, 0x40, "Q", 0)
    put(metadata, 0x48, "Q", 0x80809FBD00000000)
    # The native directory reader obtains the allocated entry-row count here.
    put(metadata, 0x50, "I", entry_count)
    put(metadata, 0x58, "Q", 0x0000000080809EF3)

    cursor = METADATA_PREFIX_SIZE
    for entry, placement in zip(entries, placements):
        put(metadata, cursor, "IIQ", entry.reference, entry.type_info, placement.encode())
        cursor += ENTRY_RECORD_SIZE

    cursor = block_table_offset
    physical = data_offset
    for block in blocks:
        put(metadata, cursor, "IIHH", physical, len(block), patch, 0)
        metadata[cursor + 0x0C : cursor + 0x20] = hashlib.sha1(block).digest()
        # +0x20..+0x2f is the zero GCM tag for an unencrypted block.
        physical += len(block)
        cursor += BLOCK_RECORD_SIZE
    return bytes(metadata)


def expected_filename(package_id: int, patch: int, family: str) -> str:
    normalized = family.lower().strip("_")
    if not normalized or not re.fullmatch(r"[a-z0-9_]+", normalized):
        raise ValueError("family may contain only lowercase letters, digits, and underscores")
    return f"w64_{normalized}_{package_id:04x}_{patch}.pkg"


def validate_output_name(path: Path, package_id: int, patch: int) -> None:
    match = PACKAGE_RE.fullmatch(path.name.lower())
    if match is None:
        raise ValueError("output filename must look like w64_<family>_<4-hex-id>_<patch>.pkg")
    if int(match.group("package"), 16) != package_id or int(match.group("patch")) != patch:
        raise ValueError("output filename package id or patch does not match the manifest")


def derive_group_id(
    header: bytes, misc: bytes, metadata: bytes, blocks: list[bytes]
) -> int:
    """Derive a stable public package identity from all authored package content."""
    digest = hashlib.sha256()
    digest.update(GROUP_ID_DOMAIN)
    digest.update(header)
    digest.update(misc)
    digest.update(metadata)
    for block in blocks:
        digest.update(block)
    value = int.from_bytes(digest.digest()[:8], "little")
    return value or 1


def pack(manifest_path: Path, output_path: Path) -> dict:
    document, entries = load_manifest(manifest_path)
    package_id = integer(document.get("package_id"), "package_id")
    patch = integer(document.get("patch", 0), "patch")
    if not CUSTOM_PACKAGE_MIN <= package_id <= CUSTOM_PACKAGE_MAX:
        raise ValueError(
            f"custom package id must use the untracked window {CUSTOM_PACKAGE_MIN:#x}..{CUSTOM_PACKAGE_MAX:#x}"
        )
    if not 0 <= patch <= MAX_PATCH:
        raise ValueError(f"patch must fit 0..{MAX_PATCH}")
    validate_output_name(output_path, package_id, patch)

    blocks, placements = build_blocks(entries)
    misc = build_misc(document, package_id)
    # Shipped packages place metadata at 0x1000 when no extended-header directory exists and at
    # 0x1800 when the directory occupies 0x1000. Keep the same canonical boundary.
    metadata_offset = METADATA_OFFSET if misc else MISC_OFFSET
    provisional_metadata_size = (
        METADATA_PREFIX_SIZE
        + len(entries) * ENTRY_RECORD_SIZE
        + ENTRY_BLOCK_GAP
        + len(blocks) * BLOCK_RECORD_SIZE
    )
    data_offset = align(metadata_offset + provisional_metadata_size, DATA_ALIGNMENT)
    metadata = build_metadata(entries, placements, blocks, data_offset, patch)
    payload_end = data_offset + sum(len(block) for block in blocks)
    file_size = align(payload_end, FILE_ALIGNMENT)

    header = bytearray(HEADER_SIZE)
    put(header, 0x00, "H", VERSION)
    put(header, 0x02, "H", PLATFORM)
    put(header, 0x04, "H", package_id)
    put(header, 0x06, "B", 1)
    put(header, 0x07, "B", 0)
    group_id_value = document.get("group_id", GROUP_ID_AUTO)
    automatic_group_id = group_id_value == GROUP_ID_AUTO
    if not automatic_group_id:
        group_id = integer(group_id_value, "group_id") & 0xFFFFFFFFFFFFFFFF
    put(header, 0x10, "Q", integer(document.get("build_time", int(time.time())), "build_time"))
    # Byte +0x1A must be 1 to select the pre-Beyond-Light directory layout.
    put(header, 0x18, "I", integer(document.get("content_build", 0x00014B68), "content_build"))
    put(header, 0x1C, "I", integer(document.get("content_revision", 0), "content_revision"))
    put(header, 0x20, "H", patch)
    put(header, 0x22, "B", integer(document.get("language", 0), "language") & 0xFF)
    # These two words form a native header-validation pair. Keep them manifest-driven because
    # their meaning varies by package family; the custom ornament manifest uses the pair carried
    # by the matching investment_globals_client packages for this content build.
    put(header, 0xA4, "I", integer(document.get("header_word_a4", 0), "header_word_a4"))
    put(header, 0xA8, "I", integer(document.get("header_word_a8", 0), "header_word_a8"))
    put(header, 0xAC, "I", HEADER_FORMAT_MARKER)
    put(header, 0xB0, "I", HEADER_SIGNATURE_OFFSET)
    put(header, 0xB4, "I", len(entries))
    put(header, 0xB8, "I", 0)
    put(header, 0xD0, "I", len(blocks))
    put(header, 0xD4, "I", 0)
    put(header, 0xF0, "I", MISC_OFFSET if misc else 0)
    put(header, 0xF4, "I", len(misc))
    header[0xF8:0x10C] = hashlib.sha1(misc).digest()
    put(header, 0x110, "I", metadata_offset)
    put(header, 0x114, "I", len(metadata))
    header[0x118:0x12C] = hashlib.sha1(metadata).digest()
    put(header, 0x160, "I", file_size - FILE_DATA_START)
    put(header, 0x164, "I", file_size)
    put(header, 0x168, "I", LOCALE_TOKEN_ZERO)
    put(header, 0x16C, "B", 0)

    # Offset +0x08 remains zero while deriving the automatic identity, avoiding a circular hash.
    if automatic_group_id:
        group_id = derive_group_id(bytes(header), misc, metadata, blocks)
    put(header, 0x08, "Q", group_id)

    signature = bytes(HEADER_SIGNATURE_SIZE)
    signature_name = document.get("header_signature")
    if signature_name is not None:
        if not isinstance(signature_name, str) or not signature_name:
            raise ValueError("header_signature must be a nonempty path string")
        signature = (manifest_path.parent / signature_name).read_bytes()
        if len(signature) != HEADER_SIGNATURE_SIZE:
            raise ValueError(f"header signature must be exactly {HEADER_SIGNATURE_SIZE} bytes")

    output = bytearray(file_size)
    output[:HEADER_SIZE] = header
    output[HEADER_SIGNATURE_OFFSET : HEADER_SIGNATURE_OFFSET + HEADER_SIGNATURE_SIZE] = signature
    output[MISC_OFFSET : MISC_OFFSET + len(misc)] = misc
    output[metadata_offset : metadata_offset + len(metadata)] = metadata
    cursor = data_offset
    for block in blocks:
        output[cursor : cursor + len(block)] = block
        cursor += len(block)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(output)
    return {
        "path": str(output_path),
        "packageId": f"0x{package_id:04X}",
        "patch": patch,
        "groupId": f"0x{group_id:016X}",
        "entries": len(entries),
        "blocks": len(blocks),
        "fileSize": file_size,
        "tagBase": f"0x{(0x80800000 + (package_id << 13)) & 0xFFFFFFFF:08X}",
        "retailSigned": any(signature),
    }


def unpack_header(data: bytes) -> dict:
    if len(data) < HEADER_SIZE:
        raise ValueError("file is shorter than the package header")
    u16 = lambda offset: struct.unpack_from("<H", data, offset)[0]
    u32 = lambda offset: struct.unpack_from("<I", data, offset)[0]
    u64 = lambda offset: struct.unpack_from("<Q", data, offset)[0]
    return {
        "version": u16(0x00),
        "platform": u16(0x02),
        "package_id": u16(0x04),
        "group_id": u64(0x08),
        "patch": u16(0x20),
        "header_word_a4": u32(0xA4),
        "header_word_a8": u32(0xA8),
        "format_marker": u32(0xAC),
        "signature_offset": u32(0xB0),
        "entry_count": u32(0xB4),
        "block_count": u32(0xD0),
        "misc_offset": u32(0xF0),
        "misc_size": u32(0xF4),
        "metadata_offset": u32(0x110),
        "metadata_size": u32(0x114),
        "data_size": u32(0x160),
        "file_size": u32(0x164),
        "locale_token": u32(0x168),
        "locale_id": data[0x16C],
    }


def verify(path: Path) -> dict:
    data = path.read_bytes()
    header = unpack_header(data)
    errors: list[str] = []
    warnings: list[str] = []
    if header["version"] != VERSION:
        errors.append(f"unsupported version {header['version']}")
    if header["platform"] != PLATFORM:
        errors.append(f"unsupported platform {header['platform']}")
    if header["format_marker"] != HEADER_FORMAT_MARKER:
        errors.append(f"header format marker is not {HEADER_FORMAT_MARKER}")
    if not 1 <= header["entry_count"] <= MAX_ENTRIES:
        errors.append("entry count is outside 1..8192")
    if header["block_count"] == 0:
        errors.append("block count is zero")
    if header["file_size"] != len(data):
        errors.append("header file size does not match the file")
    if header["data_size"] != header["file_size"] - FILE_DATA_START:
        errors.append("header data size does not match file size minus 0x800")
    if header["patch"] > MAX_PATCH:
        errors.append("patch is outside 0..255")
    if header["locale_id"] != 0 or header["locale_token"] != LOCALE_TOKEN_ZERO:
        errors.append("locale id/token pair is not the installed locale-0 pair")

    match = PACKAGE_RE.fullmatch(path.name.lower())
    if match is None:
        errors.append("filename is not a recognized patchable package name")
    else:
        if int(match.group("package"), 16) != header["package_id"]:
            errors.append("filename package id differs from the header")
        if int(match.group("patch")) != header["patch"]:
            errors.append("filename patch differs from the header")

    for label, offset_key, size_key, digest_offset in (
        ("misc", "misc_offset", "misc_size", 0xF8),
        ("metadata", "metadata_offset", "metadata_size", 0x118),
    ):
        offset, size = header[offset_key], header[size_key]
        if (size == 0) != (offset == 0):
            errors.append(f"{label} offset and size must both be zero or both be nonzero")
        elif offset + size > len(data):
            errors.append(f"{label} region extends past the file")
        elif hashlib.sha1(data[offset : offset + size]).digest() != data[digest_offset : digest_offset + 20]:
            errors.append(f"{label} SHA-1 does not match")

    meta = header["metadata_offset"]
    entries = meta + METADATA_PREFIX_SIZE
    if meta + METADATA_PREFIX_SIZE > len(data):
        errors.append("metadata prefix is truncated")
    else:
        allocated = struct.unpack_from("<I", data, entries - 0x10)[0]
        if allocated < header["entry_count"]:
            errors.append("allocated entry region is smaller than entry count")
        blocks = entries + allocated * ENTRY_RECORD_SIZE + ENTRY_BLOCK_GAP
        if blocks + header["block_count"] * BLOCK_RECORD_SIZE > meta + header["metadata_size"]:
            errors.append("block table extends past metadata")
        else:
            for index in range(header["block_count"]):
                row = blocks + index * BLOCK_RECORD_SIZE
                offset, size, patch, flags = struct.unpack_from("<IIHH", data, row)
                if offset + size > len(data):
                    errors.append(f"block {index} extends past the file")
                    continue
                if patch != header["patch"]:
                    errors.append(f"block {index} references patch {patch}, not this patch")
                if flags != 0:
                    errors.append(f"block {index} is not plaintext/uncompressed")
                if hashlib.sha1(data[offset : offset + size]).digest() != data[row + 0x0C : row + 0x20]:
                    errors.append(f"block {index} SHA-1 does not match")

    signature_offset = header["signature_offset"]
    signature = data[signature_offset : signature_offset + HEADER_SIGNATURE_SIZE]
    retail_signed = len(signature) == HEADER_SIGNATURE_SIZE and any(signature)
    if not retail_signed:
        warnings.append("header has no RSA-PSS signature; the loader must explicitly allow unsigned custom packages")
    return {**header, "valid": not errors, "retailSigned": retail_signed, "errors": errors, "warnings": warnings}


def extract_entry(path: Path, entry_index: int) -> tuple[bytes, int, int]:
    """Extract one plaintext entry from a package authored by this tool."""
    data = path.read_bytes()
    header = unpack_header(data)
    if not 0 <= entry_index < header["entry_count"]:
        raise ValueError("entry index is outside the package entry table")
    meta = header["metadata_offset"]
    if meta + METADATA_PREFIX_SIZE > len(data):
        raise ValueError("metadata prefix is truncated")
    allocated = struct.unpack_from("<I", data, meta + 0x50)[0]
    if allocated < header["entry_count"]:
        raise ValueError("allocated entry region is smaller than entry count")
    entry_offset = meta + METADATA_PREFIX_SIZE + entry_index * ENTRY_RECORD_SIZE
    reference, type_info, encoded = struct.unpack_from("<IIQ", data, entry_offset)
    start_block = encoded & 0x3FFF
    start_offset = ((encoded >> 14) & 0x3FFF) << 4
    payload_size = encoded >> 28
    if payload_size == 0:
        raise ValueError("entry payload is empty")
    block_table = meta + METADATA_PREFIX_SIZE + allocated * ENTRY_RECORD_SIZE + ENTRY_BLOCK_GAP
    output = bytearray()
    block_index = start_block
    while len(output) < payload_size:
        if block_index >= header["block_count"]:
            raise ValueError("entry extends past the block table")
        record = block_table + block_index * BLOCK_RECORD_SIZE
        offset, size, patch, flags = struct.unpack_from("<IIHH", data, record)
        if patch != header["patch"] or flags != 0 or offset + size > len(data):
            raise ValueError("extract supports only plaintext blocks stored in this patch")
        skip = start_offset if block_index == start_block else 0
        if skip > size:
            raise ValueError("entry start offset exceeds its first block")
        take = min(size - skip, payload_size - len(output))
        if take == 0:
            raise ValueError("entry block contributes no payload bytes")
        output.extend(data[offset + skip : offset + skip + take])
        block_index += 1
    return bytes(output), reference, type_info


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack_parser = commands.add_parser("pack", help="write a package from a JSON manifest")
    pack_parser.add_argument("manifest", type=Path)
    pack_parser.add_argument("output", type=Path)
    verify_parser = commands.add_parser("verify", help="validate a package written by this tool")
    verify_parser.add_argument("package", type=Path)
    inspect_parser = commands.add_parser("inspect", help="print public package header fields")
    inspect_parser.add_argument("package", type=Path)
    extract_parser = commands.add_parser("extract", help="extract one plaintext entry")
    extract_parser.add_argument("package", type=Path)
    extract_parser.add_argument("entry", type=lambda value: int(value, 0))
    extract_parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.command == "pack":
        result = pack(args.manifest.resolve(), args.output.resolve())
    elif args.command == "verify":
        result = verify(args.package.resolve())
    elif args.command == "inspect":
        result = unpack_header(args.package.read_bytes())
    else:
        payload, reference, type_info = extract_entry(args.package.resolve(), args.entry)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(payload)
        result = {
            "path": str(args.output.resolve()),
            "entry": args.entry,
            "reference": f"0x{reference:08X}",
            "typeInfo": f"0x{type_info:08X}",
            "size": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        }
    print(json.dumps(result, indent=2))
    if args.command == "verify" and not result["valid"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
