# Sunrise Tiger package packer

This tool writes deterministic, plaintext Destiny 2 pre-Beyond-Light Tiger package containers.
It targets package version 38 and Windows platform 2, using the custom untracked package-ID window
`0xAA0..0xCFF` documented for Sunrise.

## Current milestone

The writer currently supports:

- a pre-BL package header and metadata directory;
- arbitrary entry `reference`, `type_info`, and payload bytes;
- 16-byte entry placement and automatic `0x40000` block splitting;
- uncompressed, unencrypted patch-local blocks;
- SHA-1 for the misc region, metadata region, and every block;
- filename/header consistency checks and structural round-trip validation.

It does **not** forge Bungie's RSA-PSS header signature. Supply a real 256-byte signature using the
manifest's `header_signature` field, or use it only with a Sunrise loader that explicitly permits
unsigned custom packages.

## Usage

```bash
python3 tiger_pkg_packer.py pack example/minimal.json w64_custom_ornaments_0aa0_0.pkg
python3 tiger_pkg_packer.py verify w64_custom_ornaments_0aa0_0.pkg
python3 tiger_pkg_packer.py inspect w64_custom_ornaments_0aa0_0.pkg
python3 tiger_pkg_packer.py extract w64_custom_ornaments_0aa0_0.pkg 0 extracted_entry.bin
```

The package TagHash for entry `i` is calculated by addition:

```text
0x80800000 + (package_id << 13) + i
```

For package `0xAA0`, entry zero is `0x81D40000`.

The next stage is an ornament authoring layer that emits client-known geometry, material, texture,
appearance, and inventory-definition tag families into these generic entry records.

## Data-driven ornaments

An ornament package can declare an `ornaments` array instead of raw `entries`. Each row supplies
its own new definition hash, target weapon, native socket lane/type, serialized definition template,
and optional texture. The packer emits a versioned `SUNO` descriptor plus the referenced entries.
Sunrise discovers these descriptors by class, so adding another ornament does not require changing
or rebuilding the DLL.

```json
{
  "package_id": "0xAA0",
  "patch": 0,
  "ornaments": [
    {
      "name": "Sunrise Diamond",
      "definition_hash": "0x10203040",
      "target_item_hash": "0x6F22FCEC",
      "template_item_hash": "0x908F68D6",
      "socket_type": 453,
      "socket_lane": 6,
      "definition_template": "positive_infinity.bin",
      "texture": "Diffuse.rgba",
      "resources": [
        {"role": "model_glb", "path": "model.glb"},
        {"role": "branch_1_positions", "path": "branch_1/positions.buf"},
        {"role": "branch_1_attributes", "path": "branch_1/attributes.buf"},
        {"role": "branch_1_indices", "path": "branch_1/indices.buf"}
      ]
    }
  ]
}
```

Every descriptor contains entry indices rather than package-specific absolute tags. This allows the
same authoring format to be used in any package inside the custom package-ID window.
Named `resources` are open-ended package payloads. Sunrise records their package tags by role, so a
new model layout or auxiliary material file is package data rather than an ornament-specific DLL
constant.

`ornament_bundle.py` collects an existing definition and any extracted model, buffer, material, and
texture files into one editable directory. It writes the relative `package.json` consumed by the
packer; no source asset path is baked into the DLL or final package.
