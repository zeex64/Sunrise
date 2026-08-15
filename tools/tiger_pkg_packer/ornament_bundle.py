#!/usr/bin/env python3
"""Collect an editable ornament source bundle and emit its package manifest."""

from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path


ROLE_RE = re.compile(r"[a-z0-9_]{1,31}")


def copy_asset(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise ValueError(f"source asset does not exist: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def resource_argument(value: str) -> tuple[str, Path]:
    role, separator, source = value.partition("=")
    if not separator or ROLE_RE.fullmatch(role) is None or not source:
        raise argparse.ArgumentTypeError("resource must be role=/path with a lowercase role")
    return role, Path(source).resolve()


def build_bundle(args: argparse.Namespace) -> Path:
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    definition_name = "definition_template.bin"
    copy_asset(args.definition.resolve(), output / definition_name)

    ornament: dict[str, object] = {
        "name": args.name,
        "definition_hash": args.definition_hash,
        "target_item_hash": args.target_item_hash,
        "template_item_hash": args.template_item_hash,
        "socket_type": args.socket_type,
        "socket_lane": args.socket_lane,
        "definition_template": definition_name,
    }
    if args.texture is not None:
        texture_name = "Diffuse.rgba"
        copy_asset(args.texture.resolve(), output / texture_name)
        ornament["texture"] = texture_name

    resources: list[dict[str, str]] = []
    for role, source in args.resource:
        suffix = "".join(source.suffixes)
        relative = Path("resources") / f"{role}{suffix}"
        copy_asset(source, output / relative)
        resources.append({"role": role, "path": relative.as_posix()})
    if resources:
        ornament["resources"] = resources

    manifest = {
        "package_id": args.package_id,
        "patch": args.patch,
        "group_id": "0x53554E5249534501",
        "content_build": "0x00014B68",
        "ornaments": [ornament],
    }
    manifest_path = output / "package.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--definition-hash", required=True)
    parser.add_argument("--target-item-hash", required=True)
    parser.add_argument("--template-item-hash", required=True)
    parser.add_argument("--socket-type", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--socket-lane", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--definition", required=True, type=Path)
    parser.add_argument("--texture", type=Path)
    parser.add_argument("--resource", action="append", default=[], type=resource_argument)
    parser.add_argument("--package-id", default="0xAA0")
    parser.add_argument("--patch", default=0, type=lambda value: int(value, 0))
    args = parser.parse_args()
    manifest_path = build_bundle(args)
    print(json.dumps({"manifest": str(manifest_path), "resources": len(args.resource)}, indent=2))


if __name__ == "__main__":
    main()
