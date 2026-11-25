#!/usr/bin/env python3
"""Unit tests for ``packaging/`` manifests and ``update_checksums.py``."""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, List

PACKAGING_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PACKAGING_DIR))

import update_checksums  # noqa: E402


SAMPLE_HASHES = {
    "desk-agent-linux-x64.tar.gz": "11" * 32,
    "desk-agent-linux-arm64.tar.gz": "22" * 32,
    "desk-agent-windows-x64.zip": "33" * 32,
    "desk-agent-windows-arm64.zip": "44" * 32,
    "desk-agent-macos-universal.tar.gz": "55" * 32,
}


def parse_srcinfo(text: str) -> Dict[str, List[str]]:
    fields: Dict[str, List[str]] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        assert "=" in line, f"Malformed .SRCINFO line: {raw!r}"
        key, val = [part.strip() for part in line.split("=", 1)]
        fields.setdefault(key, []).append(val)
    return fields


def parse_winget_singleton_yaml(text: str) -> Dict[str, Any]:
    """Minimal strict parser for WinGet singleton YAML manifests (pure stdlib)."""
    root: Dict[str, Any] = {}
    current_list_key: str | None = None
    current_item: Dict[str, str] | None = None

    for lineno, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        stripped = raw.strip()
        if indent == 0:
            current_item = None
            key, _, val = stripped.partition(":")
            val = val.strip()
            if val == "":
                root[key] = []
                current_list_key = key
            else:
                root[key] = val
                current_list_key = None
        elif indent == 2 and stripped.startswith("- "):
            assert current_list_key is not None, f"Unexpected list item at line {lineno}"
            body = stripped[2:].strip()
            target_list = root[current_list_key]
            if ":" in body:
                k, _, v = body.partition(":")
                current_item = {k.strip(): v.strip()}
                target_list.append(current_item)
            else:
                current_item = None
                target_list.append(body)
        elif indent == 4 and current_item is not None and ":" in stripped:
            k, _, v = stripped.partition(":")
            current_item[k.strip()] = v.strip()
        else:
            raise AssertionError(f"Unsupported YAML indentation at line {lineno}: {raw!r}")

    return root


class PackagingManifestTests(unittest.TestCase):
    def _validate_all_manifests(
        self, root: Path, expected_version: str, expected_hashes: Dict[str, str]
    ) -> None:
        # 1. Scoop JSON validation
        scoop_path = root / "scoop" / "deskcode.json"
        scoop = json.loads(scoop_path.read_text(encoding="utf-8"))
        self.assertEqual(scoop["version"], expected_version)
        self.assertEqual(scoop["bin"], "desk.exe")
        self.assertEqual(scoop["checkver"], "github")
        self.assertIn("autoupdate", scoop)
        self.assertEqual(
            scoop["architecture"]["64bit"]["url"],
            f"https://github.com/Milorgs/DeskCode/releases/download/v{expected_version}/desk-agent-windows-x64.zip",
        )
        self.assertEqual(
            scoop["architecture"]["64bit"]["hash"],
            expected_hashes["desk-agent-windows-x64.zip"],
        )
        self.assertEqual(
            scoop["architecture"]["arm64"]["url"],
            f"https://github.com/Milorgs/DeskCode/releases/download/v{expected_version}/desk-agent-windows-arm64.zip",
        )
        self.assertEqual(
            scoop["architecture"]["arm64"]["hash"],
            expected_hashes["desk-agent-windows-arm64.zip"],
        )

        # 2. Homebrew Formula validation (+ ruby -c syntax check if available)
        brew_path = root / "homebrew" / "deskcode.rb"
        brew_text = brew_path.read_text(encoding="utf-8")
        self.assertIn("class Deskcode < Formula", brew_text)
        self.assertIn(f'version "{expected_version}"', brew_text)
        self.assertIn("on_macos do", brew_text)
        self.assertIn("on_linux do", brew_text)
        self.assertIn("on_arm do", brew_text)
        self.assertIn("on_intel do", brew_text)
        self.assertIn('bin.install "desk"', brew_text)
        self.assertIn('system "#{bin}/desk", "--version"', brew_text)

        for archive_name in (
            "desk-agent-macos-universal.tar.gz",
            "desk-agent-linux-arm64.tar.gz",
            "desk-agent-linux-x64.tar.gz",
        ):
            pattern = (
                rf'url\s+"[^"]*/{re.escape(archive_name)}"\s*\n\s*'
                rf'sha256\s+"{expected_hashes[archive_name]}"'
            )
            self.assertRegex(brew_text, pattern)

        ruby_bin = shutil.which("ruby")
        if ruby_bin:
            proc = subprocess.run(
                [ruby_bin, "-c", str(brew_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)

        # 3. WinGet singleton YAML validation
        winget_path = root / "winget" / "Milorgs.DeskCode.yaml"
        winget_text = winget_path.read_text(encoding="utf-8")
        winget = parse_winget_singleton_yaml(winget_text)
        self.assertEqual(winget["ManifestVersion"], "1.6.0")
        self.assertEqual(winget["ManifestType"], "singleton")
        self.assertEqual(winget["PackageIdentifier"], "Milorgs.DeskCode")
        self.assertEqual(winget["PackageVersion"], expected_version)
        self.assertEqual(winget["InstallerType"], "zip")
        self.assertEqual(winget["NestedInstallerType"], "portable")
        self.assertEqual(
            winget["NestedInstallerFiles"],
            [{"RelativeFilePath": "desk.exe", "PortableCommandAlias": "desk"}],
        )
        installers_by_arch = {item["Architecture"]: item for item in winget["Installers"]}
        self.assertEqual(set(installers_by_arch.keys()), {"x64", "arm64"})
        self.assertEqual(
            installers_by_arch["x64"]["InstallerUrl"],
            f"https://github.com/Milorgs/DeskCode/releases/download/v{expected_version}/desk-agent-windows-x64.zip",
        )
        self.assertEqual(
            installers_by_arch["x64"]["InstallerSha256"].lower(),
            expected_hashes["desk-agent-windows-x64.zip"],
        )
        self.assertEqual(
            installers_by_arch["arm64"]["InstallerUrl"],
            f"https://github.com/Milorgs/DeskCode/releases/download/v{expected_version}/desk-agent-windows-arm64.zip",
        )
        self.assertEqual(
            installers_by_arch["arm64"]["InstallerSha256"].lower(),
            expected_hashes["desk-agent-windows-arm64.zip"],
        )

        # 4. Arch Linux PKGBUILD & .SRCINFO validation and consistency
        pkgbuild_path = root / "aur" / "PKGBUILD"
        srcinfo_path = root / "aur" / ".SRCINFO"
        pkgbuild_text = pkgbuild_path.read_text(encoding="utf-8")
        srcinfo = parse_srcinfo(srcinfo_path.read_text(encoding="utf-8"))

        self.assertIn("pkgname=deskcode-bin\n", pkgbuild_text)
        self.assertIn(f"pkgver={expected_version}\n", pkgbuild_text)
        self.assertIn("arch=('x86_64' 'aarch64')", pkgbuild_text)
        self.assertIn(
            f"sha256sums_x86_64=('{expected_hashes['desk-agent-linux-x64.tar.gz']}')",
            pkgbuild_text,
        )
        self.assertIn(
            f"sha256sums_aarch64=('{expected_hashes['desk-agent-linux-arm64.tar.gz']}')",
            pkgbuild_text,
        )

        self.assertEqual(srcinfo["pkgbase"], ["deskcode-bin"])
        self.assertEqual(srcinfo["pkgname"], ["deskcode-bin"])
        self.assertEqual(srcinfo["pkgver"], [expected_version])
        self.assertEqual(srcinfo["arch"], ["x86_64", "aarch64"])
        self.assertEqual(
            srcinfo["sha256sums_x86_64"],
            [expected_hashes["desk-agent-linux-x64.tar.gz"]],
        )
        self.assertEqual(
            srcinfo["sha256sums_aarch64"],
            [expected_hashes["desk-agent-linux-arm64.tar.gz"]],
        )
        self.assertTrue(
            srcinfo["source_x86_64"][0].endswith(
                f"/releases/download/v{expected_version}/desk-agent-linux-x64.tar.gz"
            )
        )
        self.assertTrue(
            srcinfo["source_aarch64"][0].endswith(
                f"/releases/download/v{expected_version}/desk-agent-linux-arm64.tar.gz"
            )
        )

        bash_bin = shutil.which("bash")
        if bash_bin:
            proc = subprocess.run(
                [bash_bin, "-n", str(pkgbuild_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_committed_manifests_are_valid(self) -> None:
        zero_hashes = {k: "0" * 64 for k in SAMPLE_HASHES}
        self._validate_all_manifests(PACKAGING_DIR, "1.18.5", zero_hashes)

    def test_update_checksums_cli_and_determinism(self) -> None:
        with tempfile.TemporaryDirectory(prefix="deskcode-pkg-test-") as tmp:
            tmp_root = Path(tmp)
            copied = tmp_root / "packaging"
            shutil.copytree(
                PACKAGING_DIR,
                copied,
                ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
            )

            sums_file = tmp_root / "SHA256SUMS.txt"
            lines = [
                f"{SAMPLE_HASHES['desk-agent-linux-x64.tar.gz']}  dist/desk-agent-linux-x64.tar.gz",
                f"{SAMPLE_HASHES['desk-agent-linux-arm64.tar.gz']}  desk-agent-linux-arm64.tar.gz",
                f"{SAMPLE_HASHES['desk-agent-windows-x64.zip'].upper()} *desk-agent-windows-x64.zip",
                f"{SAMPLE_HASHES['desk-agent-windows-arm64.zip']} *dist\\desk-agent-windows-arm64.zip",
                f"{SAMPLE_HASHES['desk-agent-macos-universal.tar.gz']}  desk-agent-macos-universal.tar.gz",
            ]
            sums_file.write_text("\n".join(lines) + "\n", encoding="utf-8")

            env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
            cmd = [
                sys.executable,
                "-B",
                str(copied / "update_checksums.py"),
                "v1.18.5",
                str(sums_file),
            ]
            res1 = subprocess.run(cmd, capture_output=True, text=True, env=env, check=False)
            self.assertEqual(res1.returncode, 0, res1.stderr)
            self._validate_all_manifests(copied, "1.18.5", SAMPLE_HASHES)

            manifest_rel_paths = [
                "homebrew/deskcode.rb",
                "scoop/deskcode.json",
                "aur/PKGBUILD",
                "aur/.SRCINFO",
                "winget/Milorgs.DeskCode.yaml",
            ]
            snap1 = {
                rel: (copied / rel).read_bytes() for rel in manifest_rel_paths
            }

            # Re-running with same inputs must produce byte-identical files
            res2 = subprocess.run(cmd, capture_output=True, text=True, env=env, check=False)
            self.assertEqual(res2.returncode, 0, res2.stderr)
            snap2 = {
                rel: (copied / rel).read_bytes() for rel in manifest_rel_paths
            }
            self.assertEqual(snap1, snap2)

    def test_update_checksums_rejects_incomplete_or_invalid_inputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="deskcode-pkg-err-") as tmp:
            tmp_root = Path(tmp)
            incomplete = tmp_root / "SHA256SUMS.txt"
            incomplete.write_text(
                f"{'a' * 64}  desk-agent-linux-x64.tar.gz\n", encoding="utf-8"
            )
            with self.assertRaises(ValueError):
                update_checksums.parse_sha256sums(incomplete)
            with self.assertRaises(ValueError):
                update_checksums.normalize_version("not-a-semver")


if __name__ == "__main__":
    unittest.main(verbosity=2)
