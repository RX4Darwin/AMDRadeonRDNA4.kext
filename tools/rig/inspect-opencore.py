#!/usr/bin/env python3
"""Print a privacy-conscious OpenCore boot-path audit from config.plist."""
from __future__ import annotations

import argparse
import plistlib
import sys
from pathlib import Path
from typing import Any


def text(value: Any) -> str:
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "(unset)"
    return str(value)


def section(title: str) -> None:
    print(f"\n## {title}\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarise OpenCore boot-relevant settings without printing SMBIOS identity."
    )
    parser.add_argument("config", type=Path)
    args = parser.parse_args()
    try:
        with args.config.expanduser().open("rb") as stream:
            cfg = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException, ValueError) as exc:
        print(f"error: could not read plist: {exc}", file=sys.stderr)
        return 2
    if not isinstance(cfg, dict):
        print("error: expected plist root dictionary", file=sys.stderr)
        return 2

    print("# OpenCore boot-path audit")
    print("\nGenerated from the supplied config. SMBIOS serial, MLB, UUID and ROM values are intentionally omitted.")

    section("Boot arguments and platform")
    nvram = cfg.get("NVRAM", {}).get("Add", {})
    for guid, values in nvram.items():
        if not isinstance(values, dict):
            continue
        if "boot-args" in values:
            print(f"- NVRAM GUID {guid}: boot-args = `{text(values['boot-args'])}`")
    generic = cfg.get("PlatformInfo", {}).get("Generic", {})
    for key in ("SystemProductName", "SpoofVendor", "AdviseWindows", "ProcessorType"):
        if key in generic:
            print(f"- PlatformInfo.Generic.{key}: {text(generic[key])}")
    if "SecureBootModel" in nvram:
        print(f"- NVRAM SecureBootModel: {text(nvram['SecureBootModel'])}")

    section("ACPI")
    acpi = cfg.get("ACPI", {})
    for group in ("Add", "Delete", "Patch"):
        entries = acpi.get(group, [])
        print(f"- ACPI.{group}: {len(entries) if isinstance(entries, list) else 'invalid'} entries")
        if isinstance(entries, list):
            for i, entry in enumerate(entries):
                if not isinstance(entry, dict):
                    continue
                name = entry.get("Path") or entry.get("Comment") or entry.get("TableSignature") or "(unnamed)"
                enabled = entry.get("Enabled", "(missing)")
                print(f"  - [{i}] {text(name)} | Enabled={text(enabled)}")

    section("Kernel patches")
    patches = cfg.get("Kernel", {}).get("Patch", [])
    print(f"- Kernel.Patch: {len(patches) if isinstance(patches, list) else 'invalid'} entries")
    if isinstance(patches, list):
        for i, patch in enumerate(patches):
            if not isinstance(patch, dict):
                continue
            print(f"- [{i}] {text(patch.get('Comment', '(no comment)'))}")
            for key in ("Enabled", "Identifier", "Arch", "MinKernel", "MaxKernel", "Count", "Skip", "ReplaceMask"):
                if key in patch:
                    print(f"  - {key}: {text(patch[key])}")
            for key in ("Find", "Replace", "Mask"):
                if key in patch:
                    value = patch[key]
                    print(f"  - {key}: {text(value)} (length={len(value) if hasattr(value, '__len__') else 'n/a'})")

    section("Kernel kext entries")
    additions = cfg.get("Kernel", {}).get("Add", [])
    print(f"- Kernel.Add: {len(additions) if isinstance(additions, list) else 'invalid'} entries")
    if isinstance(additions, list):
        for i, entry in enumerate(additions):
            if not isinstance(entry, dict):
                continue
            print(f"- [{i}] {text(entry.get('BundlePath', '(no BundlePath)'))} | Enabled={text(entry.get('Enabled', '(missing)'))} | MinKernel={text(entry.get('MinKernel', ''))} | MaxKernel={text(entry.get('MaxKernel', ''))}")

    section("Kernel quirks")
    quirks = cfg.get("Kernel", {}).get("Quirks", {})
    for key in sorted(quirks):
        print(f"- {key}: {text(quirks[key])}")

    section("UEFI and PCI-related settings")
    for group in ("Quirks", "APFS"):
        values = cfg.get("UEFI", {}).get(group, {})
        for key in sorted(values):
            if any(term in key.lower() for term in ("pci", "bar", "memory", "connect", "driver", "protocol")) or group == "APFS":
                print(f"- UEFI.{group}.{key}: {text(values[key])}")
    for key in ("Above4GDecoding", "Resizable BAR", "ResizeAppleGpuBars", "RebuildAppleMemoryMap", "SyncRuntimePermissions"):
        print(f"- Miscellaneous {key}: inspect manually if present in firmware setup; not all are config.plist keys")

    section("Device properties")
    props = cfg.get("DeviceProperties", {}).get("Add", {})
    print(f"- DeviceProperties.Add: {len(props) if isinstance(props, dict) else 'invalid'} device paths")
    if isinstance(props, dict):
        for path, values in props.items():
            names = sorted(values) if isinstance(values, dict) else []
            print(f"- {path}: {', '.join(names) if names else '(no property names)'}")

    print("\n---\nThis is a structural summary, not an OpenCore validation result. Validate separately with the matching ocvalidate version.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
