#!/usr/bin/env python3
"""Generate the checked-in Unicode 17 terminal-width interval table.

The build does not run this script.  It is a maintainer tool: download the
pinned Unicode data with --download, verify every SHA-256, and regenerate
src/platform/unicode_width_table.h deterministically.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import urllib.request


UNICODE_VERSION = "17.0.0"
BASE_URL = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd"
SOURCES = {
    "EastAsianWidth.txt": (
        f"{BASE_URL}/EastAsianWidth.txt",
        "ea7ce50f3444a050333448dffef1cadd9325af55cbb764b4a2280faf52170a33",
    ),
    "DerivedGeneralCategory.txt": (
        f"{BASE_URL}/extracted/DerivedGeneralCategory.txt",
        "d62e5bab70ca74f099343f71224fa051cb1fdd61a1ab45c0488c44cfc0b6102e",
    ),
    "DerivedCoreProperties.txt": (
        f"{BASE_URL}/DerivedCoreProperties.txt",
        "24c7fed1195c482faaefd5c1e7eb821c5ee1fb6de07ecdbaa64b56a99da22c08",
    ),
    "PropList.txt": (
        f"{BASE_URL}/PropList.txt",
        "130dcddcaadaf071008bdfce1e7743e04fdfbc910886f017d9f9ac931d8c64dd",
    ),
}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def obtain_sources(directory: pathlib.Path, download: bool) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in SOURCES.items():
        path = directory / name
        if not path.exists():
            if not download:
                raise SystemExit(f"missing {path}; rerun with --download")
            print(f"Downloading {url}")
            with urllib.request.urlopen(url) as response, path.open("wb") as out:
                while block := response.read(1024 * 1024):
                    out.write(block)
        actual = sha256(path)
        if actual != expected:
            raise SystemExit(
                f"SHA-256 mismatch for {path}: expected {expected}, got {actual}"
            )


def parse_records(path: pathlib.Path):
    with path.open(encoding="utf-8") as source:
        for raw_line in source:
            data, _, comment = raw_line.partition("#")
            if ";" not in data:
                continue
            code_range, property_name, *_ = (
                field.strip() for field in data.split(";")
            )
            if not code_range or not property_name:
                continue
            if ".." in code_range:
                first_text, last_text = code_range.split("..", 1)
            else:
                first_text = last_text = code_range
            yield (
                int(first_text, 16),
                int(last_text, 16),
                property_name,
                comment.strip(),
            )


def points_for(path: pathlib.Path, properties: set[str]) -> set[int]:
    result: set[int] = set()
    for first, last, property_name, _ in parse_records(path):
        if property_name in properties:
            result.update(range(first, last + 1))
    return result


def merge_ranges(points: set[int]) -> list[tuple[int, int]]:
    if not points:
        return []
    ordered = sorted(points)
    result: list[tuple[int, int]] = []
    first = previous = ordered[0]
    for value in ordered[1:]:
        if value == previous + 1:
            previous = value
            continue
        result.append((first, previous))
        first = previous = value
    result.append((first, previous))
    return result


def build_tables(directory: pathlib.Path):
    general = directory / "DerivedGeneralCategory.txt"
    core = directory / "DerivedCoreProperties.txt"
    prop_list = directory / "PropList.txt"
    east_asian = directory / "EastAsianWidth.txt"

    # This follows the scalar-width portion of the current wcwidth algorithm.
    # Ambiguous East Asian characters stay narrow.  Sequence-dependent VS15,
    # VS16, Fitzpatrick, flag and ZWJ behavior is handled by the terminal cell
    # stream; this table describes the standalone scalar width.
    zero = points_for(general, {"Me", "Mn", "Mc", "Cf", "Zl", "Zp"})
    general_zero = set(zero)
    default_ignorable = points_for(core, {"Default_Ignorable_Code_Point"})
    prepended = points_for(prop_list, {"Prepended_Concatenation_Mark"})
    hangul_jamo = set(range(0x1160, 0x1200)) | set(range(0xD7B0, 0xD800))
    emoji_modifiers = set(range(0x1F3FB, 0x1F400))

    zero.add(0)
    zero.update(hangul_jamo)
    zero.update(default_ignorable)
    zero.discard(0x115F)  # Hangul choseong filler composes into a wide cell.
    zero.discard(0x00AD)  # Soft hyphen is visible in terminal emulation.
    zero.difference_update(prepended)
    zero.difference_update(emoji_modifiers)  # Standalone modifiers are wide.

    wide = points_for(east_asian, {"W", "F"})
    wide.difference_update(general_zero)
    wide.difference_update(hangul_jamo)
    wide.difference_update(default_ignorable - {0x115F})
    wide.update(emoji_modifiers)
    wide.update(range(0x1F1E6, 0x1F200))  # Standalone regional indicators.
    wide.difference_update(zero)

    return merge_ranges(zero), merge_ranges(wide)


def render(zero: list[tuple[int, int]], wide: list[tuple[int, int]]) -> str:
    def table(name: str, ranges: list[tuple[int, int]]) -> str:
        lines = [
            f"inline constexpr std::array<UnicodeInterval, {len(ranges)}> {name}{{{{"
        ]
        for first, last in ranges:
            lines.append(f"    {{0x{first:06X}, 0x{last:06X}}},")
        lines.append("}};")
        return "\n".join(lines)

    return f"""// Generated by scripts/generate-unicode-width.py. Do not edit.
// Unicode {UNICODE_VERSION}; source file SHA-256 values are pinned in that script.
// Unicode data is distributed under Unicode License v3.
#pragma once

#include <array>

namespace mosh::win32::utf8::detail {{

struct UnicodeInterval {{
  char32_t first;
  char32_t last;
}};

{table("kUnicodeZeroWidth", zero)}

{table("kUnicodeWide", wide)}

}} // namespace mosh::win32::utf8::detail
"""


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--data-dir", type=pathlib.Path, default=root / "build" / "unicode-data"
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=root / "src" / "platform" / "unicode_width_table.h",
    )
    parser.add_argument("--download", action="store_true")
    parser.add_argument(
        "--check", action="store_true", help="fail if the checked-in table differs"
    )
    arguments = parser.parse_args()

    obtain_sources(arguments.data_dir, arguments.download)
    zero, wide = build_tables(arguments.data_dir)
    generated = render(zero, wide)
    if arguments.check:
        current = arguments.output.read_text(encoding="utf-8")
        if current != generated:
            raise SystemExit(f"generated Unicode table differs: {arguments.output}")
        print(f"Unicode table is current: {len(zero)} zero, {len(wide)} wide ranges")
        return 0

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(generated, encoding="utf-8", newline="\n")
    print(f"Wrote {arguments.output}: {len(zero)} zero, {len(wide)} wide ranges")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
