import json
import struct
import tempfile
import unittest
from pathlib import Path

import tiger_pkg_packer as tiger


class TigerPackagePackerTests(unittest.TestCase):
    def test_minimal_package_round_trip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "entry.bin").write_bytes(b"custom package payload")
            manifest = {
                "package_id": "0xAA0",
                "patch": 0,
                "entries": [{"reference": "0xFFFFFFFF", "type_info": 0, "path": "entry.bin"}],
            }
            manifest_path = root / "package.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            output = root / "w64_custom_ornaments_0aa0_0.pkg"
            summary = tiger.pack(manifest_path, output)
            report = tiger.verify(output)
            self.assertEqual(summary["packageId"], "0x0AA0")
            self.assertTrue(report["valid"], report["errors"])
            self.assertEqual(report["format_marker"], tiger.HEADER_FORMAT_MARKER)
            self.assertEqual(report["misc_offset"], 0)
            self.assertEqual(report["misc_size"], 0)
            self.assertEqual(report["metadata_offset"], tiger.MISC_OFFSET)
            self.assertEqual(report["data_size"], report["file_size"] - tiger.FILE_DATA_START)
            self.assertFalse(report["retailSigned"])
            payload, reference, type_info = tiger.extract_entry(output, 0)
            self.assertEqual(payload, b"custom package payload")
            self.assertEqual(reference, 0xFFFFFFFF)
            self.assertEqual(type_info, 0)

    def test_omitted_named_tags_use_no_misc_directory(self):
        misc = tiger.build_misc({}, 0xAA0)
        self.assertEqual(misc, b"")

    def test_automatic_group_id_tracks_package_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = root / "entry.bin"
            payload.write_bytes(b"first payload")
            manifest_path = root / "package.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "package_id": "0xAA0",
                        "patch": 0,
                        "build_time": 1,
                        "entries": [{"path": "entry.bin"}],
                    }
                ),
                encoding="utf-8",
            )
            output = root / "w64_custom_ornaments_0aa0_0.pkg"
            first = tiger.pack(manifest_path, output)["groupId"]
            self.assertEqual(first, tiger.pack(manifest_path, output)["groupId"])
            payload.write_bytes(b"second payload")
            second = tiger.pack(manifest_path, output)["groupId"]
            self.assertNotEqual(first, second)

    def test_named_tag_misc_uses_native_prebl_directory(self):
        document = {
            "named_tags": [
                {"name": "investment_globals", "entry": 17, "class_id": tiger.INVESTMENT_GLOBALS_CLASS}
            ]
        }
        misc = tiger.build_misc(document, 0xAA0)
        logical_size, version = struct.unpack_from("<QQ", misc, 0)
        count, relative, reserved = struct.unpack_from("<QQQ", misc, 0x10)
        allocated = struct.unpack_from("<Q", misc, 0x70)[0]
        tag, class_id, name_relative = struct.unpack_from("<IIQ", misc, 0x80)
        self.assertEqual(logical_size, 0x123)
        self.assertEqual(version, tiger.MISC_VERSION)
        self.assertEqual((count, relative, reserved), (1, 0x58, 0))
        self.assertEqual(allocated, tiger.MISC_RECORD_MINIMUM_ALLOCATION)
        self.assertEqual(tag, tiger.entry_tag(0xAA0, 17))
        self.assertEqual(class_id, tiger.INVESTMENT_GLOBALS_CLASS)
        unused_record = struct.pack("<IIQ", 0xFFFFFFFF, 0xFFFFFFFF, 0)
        self.assertEqual(misc[0x90:0x100], unused_record * 7)
        name_offset = 0x80 + 8 + name_relative
        self.assertEqual(misc[name_offset : name_offset + 19], b"investment_globals\0")

        document["named_tags"][0]["tag"] = "0x8133719C"
        aliased = tiger.build_misc(document, 0xAA0)
        self.assertEqual(struct.unpack_from("<I", aliased, 0x80)[0], 0x8133719C)

    def test_rejects_tracked_package_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "entry.bin").write_bytes(b"x")
            manifest_path = root / "package.json"
            manifest_path.write_text(
                json.dumps({"package_id": "0x975", "entries": [{"path": "entry.bin"}]}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "untracked window"):
                tiger.pack(manifest_path, root / "w64_custom_ornaments_0975_0.pkg")

    def test_multiple_ornaments_are_authored_from_manifest_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            template = bytearray(978)
            struct.pack_into("<I", template, tiger.ORNAMENT_DEFINITION_HASH_OFFSET, 0x908F68D6)
            (root / "template.bin").write_bytes(template)
            (root / "first.rgba").write_bytes(b"first texture")
            (root / "second.rgba").write_bytes(b"second texture")
            manifest = {
                "package_id": "0xAA0",
                "patch": 0,
                "ornaments": [
                    {
                        "name": "First Ornament",
                        "definition_hash": "0x10203040",
                        "target_item_hash": "0x6F22FCEC",
                        "template_item_hash": "0x908F68D6",
                        "socket_type": 453,
                        "socket_lane": 6,
                        "definition_template": "template.bin",
                        "texture": "first.rgba",
                    },
                    {
                        "name": "Second Ornament",
                        "definition_hash": "0x50607080",
                        "target_item_hash": "0x11223344",
                        "template_item_hash": "0x55667788",
                        "socket_type": 99,
                        "socket_lane": 2,
                        "definition_template": "template.bin",
                        "texture": "second.rgba",
                    },
                ],
            }
            manifest_path = root / "ornaments.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            document, entries = tiger.load_manifest(manifest_path)
            self.assertEqual(document["package_id"], "0xAA0")
            self.assertEqual(len(entries), 6)
            self.assertEqual(entries[0].reference, tiger.CUSTOM_ORNAMENT_CLASS)
            self.assertEqual(entries[3].reference, tiger.CUSTOM_ORNAMENT_CLASS)
            first = struct.unpack("<IHHIIHHHBBH64sIH", entries[0].payload[:96])
            second = struct.unpack("<IHHIIHHHBBH64sIH", entries[3].payload[:96])
            self.assertEqual(first[3:9], (0x10203040, 0x6F22FCEC, 1, 2, 453, 6))
            self.assertEqual(second[3:9], (0x50607080, 0x11223344, 4, 5, 99, 2))
            self.assertEqual(first[-2], 0x908F68D6)
            self.assertEqual(second[-2], 0x55667788)
            self.assertEqual(first[-1], 1)
            self.assertEqual(struct.unpack("<HH32s", entries[0].payload[96:])[0], 2)
            self.assertEqual(
                struct.unpack_from("<I", entries[1].payload, tiger.ORNAMENT_DEFINITION_HASH_OFFSET)[0],
                0x10203040,
            )
            self.assertEqual(
                struct.unpack_from("<I", entries[4].payload, tiger.ORNAMENT_DEFINITION_HASH_OFFSET)[0],
                0x50607080,
            )
            output = root / "w64_custom_ornaments_0aa0_0.pkg"
            tiger.pack(manifest_path, output)
            report = tiger.verify(output)
            self.assertTrue(report["valid"], report["errors"])
            self.assertEqual(report["entry_count"], 6)
            for index, expected in enumerate(entries):
                payload, reference, type_info = tiger.extract_entry(output, index)
                self.assertEqual(payload, expected.payload)
                self.assertEqual(reference, expected.reference)
                self.assertEqual(type_info, expected.type_info)


if __name__ == "__main__":
    unittest.main()
