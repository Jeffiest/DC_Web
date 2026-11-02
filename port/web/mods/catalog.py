#!/usr/bin/env python3
"""The page's list of mods for the PC (site/mods/catalog.json): each mod version with the build it was confirmed
with, which the page's PC downloads offer together.

A build is one of main's commits, kept as the release build-<commit> by .github/workflows/builds.yml. A mod version
is a folder in a MoonMod checkout at one commit, kept as the release mod-<id>-<version> by publish below
(.github/workflows/mods.yml runs it when the list changes). A version's zip never changes once published: a mod
that changed needs a new version.

    python port/web/mods/catalog.py add --moonmod E:/Moonmod --mod moonmod.vigil --build <main commit>
    python port/web/mods/catalog.py check
    python port/web/mods/catalog.py publish [--dry-run]
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CATALOG = ROOT / "port" / "web" / "site" / "mods" / "catalog.json"
MOD_ID = re.compile(r"[a-z0-9][a-z0-9._-]{2,63}")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+([+-][A-Za-z0-9.-]+)?")
COMMIT = re.compile(r"[0-9a-f]{40}")
REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
# A fixed time for every zip entry, so a version's zip is the same bytes whenever it is made.
ZIP_TIME = (2000, 1, 1, 0, 0, 0)


def git(*args: str, cwd: Path) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def load() -> dict:
    return json.loads(CATALOG.read_text(encoding="utf-8"))


def save(catalog: dict) -> None:
    catalog["mods"].sort(key=lambda entry: (entry["id"], [int(part) for part in entry["version"].split("+")[0]
                                                           .split("-")[0].split(".")]))
    CATALOG.write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def mod_tag(entry: dict) -> str:
    return f"mod-{entry['id']}-{entry['version']}"


def build_tag(entry: dict) -> str:
    return f"build-{entry['build'][:8]}"


def problems(catalog: dict) -> list[str]:
    found = []
    if not REPOSITORY.fullmatch(str(catalog.get("repository", ""))):
        found.append("repository must be owner/name")
    seen = set()
    for index, entry in enumerate(catalog.get("mods", [])):
        where = f"mods[{index}] ({entry.get('id')} {entry.get('version')})"
        if not MOD_ID.fullmatch(str(entry.get("id", ""))):
            found.append(f"{where}: id is not a mod id")
        if not VERSION.fullmatch(str(entry.get("version", ""))):
            found.append(f"{where}: version is not MAJOR.MINOR.PATCH")
        if not entry.get("name"):
            found.append(f"{where}: no name")
        if not COMMIT.fullmatch(str(entry.get("build", ""))):
            found.append(f"{where}: build must be a full commit hash")
        source = entry.get("source", {})
        if not REPOSITORY.fullmatch(str(source.get("repository", ""))) or \
                not COMMIT.fullmatch(str(source.get("commit", ""))) or not source.get("path"):
            found.append(f"{where}: source needs repository, a full commit and path")
        key = (entry.get("id"), entry.get("version"))
        if key in seen:
            found.append(f"{where}: listed twice")
        seen.add(key)
    return found


def add(args: argparse.Namespace) -> None:
    checkout = Path(args.moonmod).resolve()
    path = f"mods/{args.mod}"
    commit = git("rev-parse", "HEAD", cwd=checkout)
    if git("status", "--porcelain", "--", path, cwd=checkout):
        sys.exit(f"{path} has changes that aren't committed; the list names a commit, so commit them first")
    manifest = json.loads(git("show", f"HEAD:{path}/mod.json", cwd=checkout))
    if manifest.get("id") != args.mod:
        sys.exit(f"{path}/mod.json says its id is {manifest.get('id')!r}")
    remote = git("remote", "get-url", "origin", cwd=checkout)
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(\.git)?$", remote)
    if not match:
        sys.exit(f"the checkout's origin isn't a GitHub repository: {remote}")
    if not git("branch", "-r", "--contains", commit, cwd=checkout):
        print(f"warning: {commit[:8]} isn't pushed yet; publish needs it on GitHub", file=sys.stderr)
    build = git("rev-parse", f"{args.build}^{{commit}}", cwd=ROOT)
    if subprocess.run(["git", "merge-base", "--is-ancestor", build, "origin/main"], cwd=ROOT).returncode != 0:
        print(f"warning: {build[:8]} isn't on origin/main, so it has no build release", file=sys.stderr)

    catalog = load()
    entry = {
        "id": manifest["id"],
        "version": manifest["version"],
        "name": manifest.get("name", manifest["id"]),
        "author": manifest.get("author", ""),
        "description": manifest.get("description", ""),
        "build": build,
        "source": {"repository": match.group(1), "commit": commit, "path": path},
        "confirmed": datetime.date.today().isoformat(),
    }
    if args.note:
        entry["note"] = args.note
    previous = [m for m in catalog["mods"] if (m["id"], m["version"]) == (entry["id"], entry["version"])]
    if previous and previous[0]["source"]["commit"] != commit and not args.replace:
        sys.exit(f"{entry['id']} {entry['version']} is listed from {previous[0]['source']['commit'][:8]}; a "
                 "published version doesn't change, so give the mod a new version (or --replace before it's published)")
    catalog["mods"] = [m for m in catalog["mods"] if m not in previous] + [entry]
    found = problems(catalog)
    if found:
        sys.exit("\n".join(found))
    save(catalog)
    print(f"{entry['id']} {entry['version']} from {match.group(1)}@{commit[:8]}, confirmed with build {build[:8]}")


def check(_: argparse.Namespace) -> None:
    found = problems(load())
    if found:
        sys.exit("\n".join(found))
    print("ok")


def zip_mod(folder: Path, mod_id: str, out: Path) -> None:
    """The mod's folder as <id>/..., in name order, every entry at ZIP_TIME."""
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for file in sorted(p for p in folder.rglob("*") if p.is_file()):
            info = zipfile.ZipInfo(f"{mod_id}/{file.relative_to(folder).as_posix()}", ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, file.read_bytes())


def publish(args: argparse.Namespace) -> None:
    catalog = load()
    found = problems(catalog)
    if found:
        sys.exit("\n".join(found))
    repository = catalog["repository"]
    listed = subprocess.run(["gh", "release", "list", "-R", repository, "-L", "1000", "--json", "tagName"],
                            check=True, capture_output=True, text=True).stdout
    released = {release["tagName"] for release in json.loads(listed)}
    waiting = [entry for entry in catalog["mods"] if mod_tag(entry) not in released]
    for entry in catalog["mods"]:
        if build_tag(entry) not in released:
            print(f"note: {entry['id']} {entry['version']} names {build_tag(entry)}, which isn't released yet")
    if not waiting:
        print("every listed mod version is published")
        return
    with tempfile.TemporaryDirectory() as work:
        clones: dict[str, Path] = {}
        for entry in waiting:
            source = entry["source"]
            clone = clones.get(source["repository"])
            if clone is None:
                clone = Path(work) / f"source{len(clones)}"
                subprocess.run(["git", "clone", "-q", "--filter=blob:none", "--no-checkout",
                                f"https://github.com/{source['repository']}.git", str(clone)], check=True)
                clones[source["repository"]] = clone
            git("checkout", "-q", "--force", source["commit"], cwd=clone)
            folder = clone / source["path"]
            manifest = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
            if (manifest.get("id"), manifest.get("version")) != (entry["id"], entry["version"]):
                sys.exit(f"{source['path']} at {source['commit'][:8]} is {manifest.get('id')} "
                         f"{manifest.get('version')}, not {entry['id']} {entry['version']}")
            name = f"{entry['id']}-{entry['version']}.zip"
            out = Path(work) / name
            zip_mod(folder, entry["id"], out)
            notes = (f"{entry['name']} {entry['version']}"
                     + (f" by {entry['author']}" if entry["author"] else "") + ".\n\n"
                     + (entry["description"] + "\n\n" if entry["description"] else "")
                     + f"Confirmed with [build-{entry['build'][:8]}](https://github.com/{repository}/releases/tag/"
                       f"{build_tag(entry)}) on {entry['confirmed']}. From {source['repository']}@"
                       f"{source['commit'][:8]} ({source['path']}).\n\n"
                     + f"Unzip it into the game's `save/mods/` folder: the zip holds the mod's own folder, "
                       f"`{entry['id']}/`. In the browser, add the zip on the page's Mods card.\n")
            if args.dry_run:
                print(f"would publish {mod_tag(entry)}: {name}, {out.stat().st_size} bytes")
                continue
            notes_file = Path(work) / "notes.md"
            notes_file.write_text(notes, encoding="utf-8")
            subprocess.run(["gh", "release", "create", mod_tag(entry), str(out), "-R", repository,
                            "--title", f"{entry['name']} {entry['version']}", "--notes-file", str(notes_file),
                            "--latest=false"], check=True)
            print(f"published {mod_tag(entry)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    adding = commands.add_parser("add", help="list a mod version, confirmed with a build")
    adding.add_argument("--moonmod", required=True, help="a MoonMod checkout at the commit to list")
    adding.add_argument("--mod", required=True, help="the mod's id (its folder under mods/)")
    adding.add_argument("--build", required=True, help="the DC_Web commit on main it was confirmed with")
    adding.add_argument("--note", help="a line for the page, such as what to know before playing")
    adding.add_argument("--replace", action="store_true", help="replace an unpublished entry of the same version")
    adding.set_defaults(run=add)
    commands.add_parser("check", help="check the list").set_defaults(run=check)
    publishing = commands.add_parser("publish", help="release each listed mod version that isn't released")
    publishing.add_argument("--dry-run", action="store_true", help="make the zips and say what would be released")
    publishing.set_defaults(run=publish)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
