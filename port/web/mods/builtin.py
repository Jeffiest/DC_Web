#!/usr/bin/env python3
"""The mods built into the web page: every mod for Chronicle in MoonMod's mods/ at the commit builtin.json names,
each zipped as mods/builtin/<id>.zip beside the page with mods/builtin/index.json listing them. The page installs
them into the browser's save/mods/ (site/shell.js, installBuiltinMods), so they are there without adding them, and
offers them for the PC builds too, with all of them in one zip (darkcloud-mods.zip, also attached to each build
release by .github/workflows/builds.yml). A mod that is a part of a merged pack there (moonmod.pack) is built in as
the pack only.

    python port/web/mods/builtin.py --out port/build/web/mods/builtin [--moonmod <checkout>] [--cache <dir>]

--moonmod reads the commit from a MoonMod checkout that has it; otherwise MoonMod is cloned into --cache (or a
temporary folder) and fetched to it. A folder already made from the same commit is left as it is.
"""

from __future__ import annotations

import argparse
import io
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from catalog import MOD_ID, VERSION, ZIP_TIME  # noqa: E402

PIN = HERE / "builtin.json"
BUNDLE = "darkcloud-mods.zip"
GAME = "chronicle"


def git(*args: str, cwd: Path, binary: bool = False):
    result = subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=not binary)
    return result.stdout


def has_commit(checkout: Path, commit: str) -> bool:
    return subprocess.run(["git", "cat-file", "-e", f"{commit}^{{commit}}"], cwd=checkout,
                          capture_output=True).returncode == 0


def source(pin: dict, moonmod: Path | None, cache: Path) -> Path:
    """A git repository holding the pinned commit."""
    commit = pin["commit"]
    if moonmod is not None:
        if not has_commit(moonmod, commit):
            git("fetch", "-q", "origin", commit, cwd=moonmod)
        return moonmod
    if not (cache / ".git").is_dir():
        cache.mkdir(parents=True, exist_ok=True)
        git("init", "-q", cwd=cache)
        git("remote", "add", "origin", f"https://github.com/{pin['repository']}.git", cwd=cache)
    if not has_commit(cache, commit):
        git("fetch", "-q", "--depth", "1", "origin", commit, cwd=cache)
    return cache


def mods_at(repo: Path, commit: str, path: str) -> dict[str, dict[str, bytes]]:
    """Each mod folder under path at commit, as {folder: {relative path: bytes}}."""
    archive = git("archive", "--format=tar", commit, path, cwd=repo, binary=True)
    folders: dict[str, dict[str, bytes]] = {}
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar:
            if not member.isfile():
                continue
            parts = member.name.split("/")[len(path.strip("/").split("/")):]
            if len(parts) < 2:
                continue
            folders.setdefault(parts[0], {})["/".join(parts[1:])] = tar.extractfile(member).read()
    return folders


def zip_files(mods: dict[str, dict[str, bytes]], out: Path) -> None:
    """As catalog.py's zip_mod: <id>/... for each mod, in name order, every entry at ZIP_TIME, so the bytes never
    change."""
    entries = {f"{mod_id}/{name}": data for mod_id, files in mods.items() for name, data in files.items()}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, entries[name])


def build(pin: dict, repo: Path, out: Path) -> list[dict]:
    commit = pin["commit"]
    staging = out.with_name(out.name + ".new")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    listed = []
    included = {}
    folders = mods_at(repo, commit, pin["path"])
    # A merged pack's parts (mod.json "parts", MoonMod's docs/modding/PACKS.md) are built in as the pack only: the
    # mods it was made from stay out of the list.
    packed = {}
    for folder, files in folders.items():
        if "mod.json" in files:
            manifest = json.loads(files["mod.json"].decode("utf-8"))
            if GAME in manifest.get("games", []):
                for part in manifest.get("parts", []):
                    packed[part["id"]] = folder
    for folder, files in sorted(folders.items()):
        if "mod.json" not in files:
            continue
        if folder in packed:
            print(f"skipping {folder}: built in as part of {packed[folder]}")
            continue
        manifest = json.loads(files["mod.json"].decode("utf-8"))
        mod_id = manifest.get("id")
        if mod_id != folder or not MOD_ID.fullmatch(str(mod_id)):
            sys.exit(f"{pin['path']}/{folder}/mod.json: its id {mod_id!r} isn't its folder's name")
        if not VERSION.fullmatch(str(manifest.get("version", ""))):
            sys.exit(f"{mod_id}: version {manifest.get('version')!r} is not MAJOR.MINOR.PATCH")
        if GAME not in manifest.get("games", []):
            print(f"skipping {mod_id}: not for {GAME}")
            continue
        name = f"{mod_id}.zip"
        zip_files({mod_id: files}, staging / name)
        included[mod_id] = files
        listed.append({
            "id": mod_id,
            "version": manifest["version"],
            "name": manifest.get("name", mod_id),
            "file": name,
            "size": (staging / name).stat().st_size,
        })
    zip_files(included, staging / BUNDLE)
    index = {"repository": pin["repository"], "commit": commit, "mods": listed,
             "bundle": {"file": BUNDLE, "size": (staging / BUNDLE).stat().st_size}}
    (staging / "index.json").write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                                        newline="\n")
    shutil.rmtree(out, ignore_errors=True)
    staging.rename(out)
    return listed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, type=Path, help="the folder to make (mods/builtin in the build)")
    parser.add_argument("--moonmod", type=Path, help="a MoonMod checkout to read the pinned commit from")
    parser.add_argument("--cache", type=Path, help="where to keep MoonMod's clone between builds")
    args = parser.parse_args()

    pin = json.loads(PIN.read_text(encoding="utf-8"))
    if not re.fullmatch(r"[0-9a-f]{40}", pin.get("commit", "")):
        sys.exit(f"{PIN}: commit must be a full commit hash")
    out = args.out.resolve()
    try:
        made = json.loads((out / "index.json").read_text(encoding="utf-8"))
        if made.get("commit") == pin["commit"] and made.get("repository") == pin["repository"] and \
                (out / BUNDLE).is_file():
            print(f"built-in mods from {pin['repository']}@{pin['commit'][:8]} are up to date")
            return
    except (OSError, ValueError):
        pass
    with tempfile.TemporaryDirectory() as work:
        cache = args.cache.resolve() if args.cache else Path(work) / "moonmod"
        repo = source(pin, args.moonmod.resolve() if args.moonmod else None, cache)
        listed = build(pin, repo, out)
    print(f"built-in mods from {pin['repository']}@{pin['commit'][:8]}: "
          + ", ".join(f"{m['id']} {m['version']}" for m in listed))


if __name__ == "__main__":
    main()
