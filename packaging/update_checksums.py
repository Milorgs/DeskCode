#!/usr/bin/env python3
"""Update version and SHA256 hashes across all DeskCode package manager manifests.

Usage::

    python3 packaging/update_checksums.py <version> <SHA256SUMS.txt> [--packaging-dir <dir>]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional

REPO_URL = "https://github.com/Milorgs/DeskCode"

REQUIRED_ARCHIVES = (
    "desk-agent-linux-x64.tar.gz",
    "desk-agent-linux-arm64.tar.gz",
    "desk-agent-windows-x64.zip",
    "desk-agent-windows-arm64.zip",
    "desk-agent-macos-universal.tar.gz",
)

VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")
SUM_LINE_RE = re.compile(r"^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$")


def normalize_version(raw: str) -> str:
    version = raw.strip()
    if version.startswith("v"):
        version = version[1:]
    if not VERSION_RE.match(version):
        raise ValueError(f"Invalid semantic version: {raw!r}")
    return version


def parse_sha256sums(sums_path: Path) -> Dict[str, str]:
    if not sums_path.is_file():
        raise FileNotFoundError(f"SHA256SUMS file not found: {sums_path}")

    hashes: Dict[str, str] = {}
    for lineno, raw_line in enumerate(
        sums_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = SUM_LINE_RE.match(line)
        if not match:
            raise ValueError(f"Malformed SHA256SUMS line {lineno}: {raw_line!r}")
        digest = match.group(1).lower()
        archive_name = Path(match.group(2).replace("\\", "/")).name
        hashes[archive_name] = digest

    missing = [name for name in REQUIRED_ARCHIVES if name not in hashes]
    if missing:
        raise ValueError(
            f"Missing required archive checksums in {sums_path}: {', '.join(missing)}"
        )
    return hashes


def _sub_once(pattern: str, repl: str, text: str, label: str, flags: int = 0) -> str:
    new_text, count = re.subn(pattern, repl, text, flags=flags)
    if count != 1:
        raise RuntimeError(f"Expected exactly 1 match for {label}, got {count}")
    return new_text


def update_homebrew(path: Path, version: str, hashes: Dict[str, str]) -> None:
    text = path.read_text(encoding="utf-8")
    text = _sub_once(
        r'^(\s*version\s+)"[^"]+"',
        rf'\g<1>"{version}"',
        text,
        "homebrew version",
        flags=re.MULTILINE,
    )
    for archive in (
        "desk-agent-macos-universal.tar.gz",
        "desk-agent-linux-arm64.tar.gz",
        "desk-agent-linux-x64.tar.gz",
    ):
        pattern = (
            rf'(url\s+"[^"]*/{re.escape(archive)}"\s*\n\s*sha256\s+)"[0-9a-fA-F]{{64}}"'
        )
        text = _sub_once(
            pattern,
            rf'\g<1>"{hashes[archive]}"',
            text,
            f"homebrew sha256 for {archive}",
        )
    path.write_text(text, encoding="utf-8", newline="\n")


def update_scoop(path: Path, version: str, hashes: Dict[str, str]) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    data["version"] = version
    arch = data.setdefault("architecture", {})
    arch["64bit"] = {
        "url": f"{REPO_URL}/releases/download/v{version}/desk-agent-windows-x64.zip",
        "hash": hashes["desk-agent-windows-x64.zip"],
    }
    arch["arm64"] = {
        "url": f"{REPO_URL}/releases/download/v{version}/desk-agent-windows-arm64.zip",
        "hash": hashes["desk-agent-windows-arm64.zip"],
    }
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")


def update_aur_pkgbuild(path: Path, version: str, hashes: Dict[str, str]) -> None:
    text = path.read_text(encoding="utf-8")
    text = _sub_once(
        r"^pkgver=.*$", f"pkgver={version}", text, "PKGBUILD pkgver", flags=re.MULTILINE
    )
    text = _sub_once(
        r"^pkgrel=.*$", "pkgrel=1", text, "PKGBUILD pkgrel", flags=re.MULTILINE
    )
    text = _sub_once(
        r"^sha256sums_x86_64=\('[0-9a-fA-F]{64}'\)$",
        f"sha256sums_x86_64=('{hashes['desk-agent-linux-x64.tar.gz']}')",
        text,
        "PKGBUILD sha256sums_x86_64",
        flags=re.MULTILINE,
    )
    text = _sub_once(
        r"^sha256sums_aarch64=\('[0-9a-fA-F]{64}'\)$",
        f"sha256sums_aarch64=('{hashes['desk-agent-linux-arm64.tar.gz']}')",
        text,
        "PKGBUILD sha256sums_aarch64",
        flags=re.MULTILINE,
    )
    path.write_text(text, encoding="utf-8", newline="\n")


def update_aur_srcinfo(path: Path, version: str, hashes: Dict[str, str]) -> None:
    text = path.read_text(encoding="utf-8")
    text = _sub_once(
        r"^\tpkgver = .*$",
        f"\tpkgver = {version}",
        text,
        ".SRCINFO pkgver",
        flags=re.MULTILINE,
    )
    text = _sub_once(
        r"^\tpkgrel = .*$",
        "\tpkgrel = 1",
        text,
        ".SRCINFO pkgrel",
        flags=re.MULTILINE,
    )
    text = _sub_once(
        r"^\tsource_x86_64 = .*$",
        (
            f"\tsource_x86_64 = deskcode-{version}-x86_64.tar.gz::"
            f"{REPO_URL}/releases/download/v{version}/desk-agent-linux-x64.tar.gz"
        ),
        text,
        ".SRCINFO source_x86_64",
        flags=re.MULTILINE,
    )
    text = _sub_once(
        r"^\tsha256sums_x86_64 = [0-9a-fA-F]{64}$",
        f"\tsha256sums_x86_64 = {hashes['desk-agent-linux-x64.tar.gz']}",
        text,
        ".SRCINFO sha256sums_x86_64",
        flags=re.MULTILINE,
    )
    text = _sub_once(
        r"^\tsource_aarch64 = .*$",
        (
            f"\tsource_aarch64 = deskcode-{version}-aarch64.tar.gz::"
            f"{REPO_URL}/releases/download/v{version}/desk-agent-linux-arm64.tar.gz"
        ),
        text,
        ".SRCINFO source_aarch64",
        flags=re.MULTILINE,
    )
    text = _sub_once(
        r"^\tsha256sums_aarch64 = [0-9a-fA-F]{64}$",
        f"\tsha256sums_aarch64 = {hashes['desk-agent-linux-arm64.tar.gz']}",
        text,
        ".SRCINFO sha256sums_aarch64",
        flags=re.MULTILINE,
    )
    path.write_text(text, encoding="utf-8", newline="\n")


def update_winget(path: Path, version: str, hashes: Dict[str, str]) -> None:
    text = path.read_text(encoding="utf-8")
    text = _sub_once(
        r"^PackageVersion:\s+.*$",
        f"PackageVersion: {version}",
        text,
        "WinGet PackageVersion",
        flags=re.MULTILINE,
    )
    for arch_name, archive in (
        ("x64", "desk-agent-windows-x64.zip"),
        ("arm64", "desk-agent-windows-arm64.zip"),
    ):
        pattern = (
            rf"(- Architecture:\s+{arch_name}\s*\n\s*InstallerUrl:\s+)\S+"
            rf"(\s*\n\s*InstallerSha256:\s+)[0-9a-fA-F]{{64}}"
        )
        url = f"{REPO_URL}/releases/download/v{version}/{archive}"
        text = _sub_once(
            pattern,
            rf"\g<1>{url}\g<2>{hashes[archive]}",
            text,
            f"WinGet {arch_name} installer",
        )
    path.write_text(text, encoding="utf-8", newline="\n")


def update_packaging(packaging_dir: Path, version: str, sums_path: Path) -> List[Path]:
    norm_ver = normalize_version(version)
    hashes = parse_sha256sums(sums_path)

    root = packaging_dir.resolve()
    targets = [
        root / "homebrew" / "deskcode.rb",
        root / "scoop" / "deskcode.json",
        root / "aur" / "PKGBUILD",
        root / "aur" / ".SRCINFO",
        root / "winget" / "Milorgs.DeskCode.yaml",
    ]
    for target in targets:
        if not target.is_file():
            raise FileNotFoundError(f"Manifest file not found: {target}")

    update_homebrew(targets[0], norm_ver, hashes)
    update_scoop(targets[1], norm_ver, hashes)
    update_aur_pkgbuild(targets[2], norm_ver, hashes)
    update_aur_srcinfo(targets[3], norm_ver, hashes)
    update_winget(targets[4], norm_ver, hashes)
    return targets


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Update DeskCode version and SHA256 hashes across package manager manifests."
    )
    parser.add_argument("version", help="Release version (e.g. 1.17.0 or v1.17.0)")
    parser.add_argument("sha256sums", help="Path to SHA256SUMS.txt covering all 5 release archives")
    parser.add_argument(
        "--packaging-dir",
        default=str(Path(__file__).resolve().parent),
        help="Path to packaging/ directory (defaults to script parent directory)",
    )
    args = parser.parse_args(argv)

    try:
        updated = update_packaging(
            Path(args.packaging_dir),
            args.version,
            Path(args.sha256sums),
        )
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for path in updated:
        print(f"updated {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
