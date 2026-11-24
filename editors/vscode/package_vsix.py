#!/usr/bin/env python3
"""Package ``editors/vscode/`` into an installable ``.vsix`` archive (pure Python 3 stdlib).

Usage::

    python3 editors/vscode/package_vsix.py [output_path]
    code --install-extension deskcode-vscode-1.17.0.vsix
"""
from __future__ import annotations

import json
import os
import sys
import zipfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple
from xml.sax.saxutils import escape, quoteattr

CONTENT_TYPES_BY_EXT: Dict[str, str] = {
    "cjs": "application/javascript",
    "css": "text/css",
    "gif": "image/gif",
    "html": "text/html",
    "jpg": "image/jpeg",
    "jpeg": "image/jpeg",
    "js": "application/javascript",
    "json": "application/json",
    "md": "text/markdown",
    "mjs": "application/javascript",
    "png": "image/png",
    "svg": "image/svg+xml",
    "txt": "text/plain",
    "vsixmanifest": "text/xml",
    "xml": "text/xml",
}

EXCLUDED_DIRS = frozenset({"tests", "__pycache__", "node_modules", ".git", ".vscode-test"})
EXCLUDED_FILES = frozenset({"package_vsix.py", ".DS_Store"})


def iter_extension_files(vscode_dir: Path) -> List[Tuple[Path, str]]:
    """Return ``(abs_path, rel_posix_path)`` pairs to embed under ``extension/``."""
    entries: List[Tuple[Path, str]] = []
    for root, dirs, files in os.walk(vscode_dir):
        dirs[:] = sorted(
            d for d in dirs if d not in EXCLUDED_DIRS and not d.startswith(".")
        )
        root_path = Path(root)
        for filename in sorted(files):
            if (
                filename in EXCLUDED_FILES
                or filename.startswith(".")
                or filename.endswith((".vsix", ".pyc", ".pyo"))
            ):
                continue
            full_path = root_path / filename
            rel_posix = full_path.relative_to(vscode_dir).as_posix()
            entries.append((full_path, rel_posix))
    entries.sort(key=lambda item: item[1])
    return entries


def build_content_types_xml(rel_paths: Iterable[str]) -> str:
    seen_exts = {"vsixmanifest"}
    for rel in rel_paths:
        ext = Path(rel).suffix.lstrip(".").lower()
        if ext:
            seen_exts.add(ext)

    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">',
    ]
    for ext in sorted(seen_exts):
        mime = CONTENT_TYPES_BY_EXT.get(ext, "application/octet-stream")
        lines.append(f'  <Default Extension={quoteattr(ext)} ContentType={quoteattr(mime)}/>')
    lines.append("</Types>")
    return "\n".join(lines) + "\n"


def build_vsixmanifest_xml(pkg: Dict[str, object], rel_paths: Iterable[str]) -> str:
    rel_set = set(rel_paths)
    ext_id = str(pkg.get("name") or "deskcode-vscode")
    publisher = str(pkg.get("publisher") or "milorgs")
    version = str(pkg.get("version") or "1.17.0")
    display_name = str(pkg.get("displayName") or ext_id)
    description = str(pkg.get("description") or "")
    engines = pkg.get("engines") if isinstance(pkg.get("engines"), dict) else {}
    vscode_engine = str(engines.get("vscode") or "^1.85.0")

    raw_keywords = pkg.get("keywords")
    keywords = [str(k) for k in raw_keywords] if isinstance(raw_keywords, list) else []
    raw_categories = pkg.get("categories")
    categories = [str(c) for c in raw_categories] if isinstance(raw_categories, list) else ["Other"]

    repo = pkg.get("repository")
    repo_url = ""
    if isinstance(repo, dict):
        repo_url = str(repo.get("url") or "")
    elif isinstance(repo, str):
        repo_url = repo
    homepage = str(pkg.get("homepage") or repo_url)

    properties = [
        ("Microsoft.VisualStudio.Code.Engine", vscode_engine),
        ("Microsoft.VisualStudio.Code.ExtensionDependencies", ""),
        ("Microsoft.VisualStudio.Code.ExtensionPack", ""),
        ("Microsoft.VisualStudio.Code.ExtensionKind", "workspace"),
        ("Microsoft.VisualStudio.Services.GitHubFlavoredMarkdown", "true"),
    ]
    if repo_url:
        properties.append(("Microsoft.VisualStudio.Services.Links.Source", repo_url))
        properties.append(("Microsoft.VisualStudio.Services.Links.GitHub", repo_url))
    if homepage:
        properties.append(("Microsoft.VisualStudio.Services.Links.Getstarted", homepage))

    assets = [
        ("Microsoft.VisualStudio.Code.Manifest", "extension/package.json"),
    ]
    if "README.md" in rel_set:
        assets.append(("Microsoft.VisualStudio.Services.Content.Details", "extension/README.md"))
    if "LICENSE" in rel_set:
        assets.append(("Microsoft.VisualStudio.Services.Content.License", "extension/LICENSE"))

    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">',
        "  <Metadata>",
        f'    <Identity Language="en-US" Id={quoteattr(ext_id)} Version={quoteattr(version)} Publisher={quoteattr(publisher)} />',
        f"    <DisplayName>{escape(display_name)}</DisplayName>",
        f'    <Description xml:space="preserve">{escape(description)}</Description>',
        f"    <Tags>{escape(','.join(keywords))}</Tags>",
        f"    <Categories>{escape(','.join(categories))}</Categories>",
        "    <GalleryFlags>Public</GalleryFlags>",
        "    <Properties>",
    ]
    for prop_id, prop_val in properties:
        lines.append(f"      <Property Id={quoteattr(prop_id)} Value={quoteattr(prop_val)} />")
    lines.extend([
        "    </Properties>",
        "  </Metadata>",
        "  <Installation>",
        '    <InstallationTarget Id="Microsoft.VisualStudio.Code" />',
        "  </Installation>",
        "  <Dependencies />",
        "  <Assets>",
    ])
    for asset_type, asset_path in assets:
        lines.append(
            f'    <Asset Type={quoteattr(asset_type)} Path={quoteattr(asset_path)} Addressable="true" />'
        )
    lines.extend([
        "  </Assets>",
        "</PackageManifest>",
    ])
    return "\n".join(lines) + "\n"


def _write_zip_bytes(zf: zipfile.ZipFile, arcname: str, data: bytes) -> None:
    info = zipfile.ZipInfo(arcname)
    info.date_time = (2026, 1, 1, 0, 0, 0)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    zf.writestr(info, data)


def build_vsix(output_path: Optional[Path] = None, vscode_dir: Optional[Path] = None) -> Path:
    root = (vscode_dir or Path(__file__).resolve().parent).resolve()
    package_json_path = root / "package.json"
    if not package_json_path.is_file():
        raise FileNotFoundError(f"Missing package.json in {root}")

    pkg = json.loads(package_json_path.read_text(encoding="utf-8"))
    default_filename = f"{pkg['name']}-{pkg['version']}.vsix"

    if output_path is None:
        dest = Path.cwd() / default_filename
    else:
        candidate = Path(output_path)
        dest = (candidate / default_filename) if candidate.is_dir() else candidate

    dest = dest.resolve()
    dest.parent.mkdir(parents=True, exist_ok=True)

    files = iter_extension_files(root)
    rel_paths = [rel for _, rel in files]

    content_types_xml = build_content_types_xml(rel_paths).encode("utf-8")
    manifest_xml = build_vsixmanifest_xml(pkg, rel_paths).encode("utf-8")

    with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        _write_zip_bytes(zf, "[Content_Types].xml", content_types_xml)
        _write_zip_bytes(zf, "extension.vsixmanifest", manifest_xml)
        for full_path, rel_posix in files:
            if full_path.resolve() == dest:
                continue
            _write_zip_bytes(zf, f"extension/{rel_posix}", full_path.read_bytes())

    return dest


def main(argv: Optional[List[str]] = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    if len(args) > 1 or (args and args[0] in ("-h", "--help")):
        print("Usage: python3 editors/vscode/package_vsix.py [output_path]", file=sys.stderr)
        return 0 if args and args[0] in ("-h", "--help") else 2

    target = Path(args[0]) if args else None
    created = build_vsix(output_path=target)
    print(created)
    return 0


if __name__ == "__main__":
    sys.exit(main())
