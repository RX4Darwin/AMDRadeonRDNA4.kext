#!/usr/bin/env python3
"""Create a review-safe copy of an OpenCore config.plist.

Only SMBIOS identity fields are changed. The source file is never modified.
Requires Python 3.9+ and the standard library only.
"""
from __future__ import annotations

import argparse
import plistlib
import sys
from pathlib import Path
from typing import Any

REDACT = {
    "systemserialnumber": "REDACTED-SERIAL",
    "mlb": "REDACTED-MLB",
    "systemuuid": "REDACTED-UUID",
    "rom": None,
}


def redact(value: Any, prefix: str = "") -> tuple[Any, list[str]]:
    """Return a recursively redacted copy and the paths changed."""
    changed: list[str] = []
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            path = f"{prefix}.{key}" if prefix else str(key)
            replacement = REDACT.get(str(key).lower(), ...)
            if replacement is not ...:
                if str(key).lower() == "rom":
                    # OpenCore stores ROM as six bytes. Preserve its plist type/length.
                    result[key] = bytes(len(item)) if isinstance(item, (bytes, bytearray)) else "REDACTED-ROM"
                else:
                    result[key] = replacement
                changed.append(path)
            else:
                result[key], nested = redact(item, path)
                changed.extend(nested)
        return result, changed
    if isinstance(value, list):
        result = []
        for index, item in enumerate(value):
            child, nested = redact(item, f"{prefix}[{index}]")
            result.append(child)
            changed.extend(nested)
        return result, changed
    return value, changed


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Write a redacted copy of an OpenCore config.plist for review."
    )
    parser.add_argument("config", type=Path, help="source EFI/OC/config.plist")
    parser.add_argument("-o", "--output", type=Path, help="output path (default: config.redacted.plist)")
    parser.add_argument("--force", action="store_true", help="replace an existing output file")
    args = parser.parse_args()

    source = args.config.expanduser().resolve()
    output = (args.output.expanduser().resolve() if args.output else
              source.with_name(source.stem + ".redacted" + source.suffix))

    if not source.is_file():
        parser.error(f"source file does not exist: {source}")
    if source == output:
        parser.error("output must be a different file; the original is never overwritten")
    if output.exists() and not args.force:
        parser.error(f"output already exists (use --force to replace it): {output}")

    try:
        with source.open("rb") as stream:
            original = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException, ValueError) as exc:
        print(f"error: could not read plist: {exc}", file=sys.stderr)
        return 2

    if not isinstance(original, dict):
        print("error: expected the OpenCore plist root to be a dictionary", file=sys.stderr)
        return 2

    cleaned, changed = redact(original)
    if not changed:
        print("warning: no recognised SMBIOS identity keys were found", file=sys.stderr)

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as stream:
        plistlib.dump(cleaned, stream, fmt=plistlib.FMT_XML, sort_keys=False)

    print(f"Redacted copy written: {output}")
    print(f"Identity fields replaced: {len(changed)}")
    for path in changed:
        print(f"  {path}")
    print("The source config was not changed. Review the output before sharing it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
