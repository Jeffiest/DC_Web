"""Copies MoonMod's animation editor page (tools/anim/editor in the Moonmod repository) into the site as anim/,
changed only where the browser differs from the desktop: the data comes from data.js instead of server.py, the
model is a blob: URL, and there is no assistant bridge. Run it again after the editor changes:

    python port/web/anim/sync_editor.py <Moonmod checkout>

Each change must match exactly once, so an editor that moved on fails here rather than half-ported.
"""

import pathlib
import sys

SITE = pathlib.Path(__file__).resolve().parents[1] / "site" / "anim"


def port(text, changes, name):
    for old, new in changes:
        count = text.count(old)
        if count != 1:
            sys.exit(f"{name}: expected one {old[:60]!r}, found {count}")
        text = text.replace(old, new)
    return text


EDITOR = [
    ('''// MoonMod animation editor: the page. server.py serves the rigs,''',
     '''// MoonMod animation editor, as DC_Web serves it (port/web/anim/sync_editor.py copied it from MoonMod's
// tools/anim/editor): data.js answers what server.py answers on the desktop, from the player's own game data.
//
// MoonMod animation editor: the page. server.py serves the rigs,'''),
    ('''import { TransformControls } from "three/addons/controls/TransformControls.js";
''', '''import { TransformControls } from "three/addons/controls/TransformControls.js";
import * as local from "./data.js";
'''),
    ('''async function api(path, body) {
  const res = await fetch(path, body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
  const data = await res.json();
  if (!res.ok) throw new Error(data.error || res.statusText);
  return data;
}''', '''async function api(path, body) {
  return local.api(path, body, (text) => status(text));
}'''),
    ('''  const gltf = await new GLTFLoader().loadAsync(location.origin + info.model);''',
     '''  const gltf = await new GLTFLoader().loadAsync(info.model);'''),
    ('''window.addEventListener("pagehide", () => navigator.sendBeacon("/api/bridge/bye?page=" + PAGE));
''', ''),
    ('''  .then(() => loadReferenceGroups().catch((e) => status("Reference: " + e.message, "bad")))
  .finally(bridgeLoop);''', '''  .then(() => loadReferenceGroups().catch((e) => status("Reference: " + e.message, "bad")));

// The browser's own: a motion downloaded rather than saved into a mod, and one opened from this computer.
function motionBody() {
  const body = {
    id: state.rig.id, path: $("path").value.trim(), speed: Number($("speed").value),
    keys: state.keys.map((k) => ({ frame: k.frame, ease: k.ease, values: k.values })),
  };
  if (state.keys[state.keys.length - 1].frame !== state.length) body.keys.push({ frame: state.length, ease: null, values: valuesAt(state.length) });
  return body;
}
$("download").onclick = () => local.download(motionBody())
  .then((r) => status("Downloaded " + r.name, "good")).catch((e) => status("Not downloaded: " + e.message, "bad"));
$("localFile").onchange = async (e) => {
  const file = e.target.files[0];
  e.target.value = "";
  if (!file) return;
  try {
    const r = await local.addLocalFile(file);
    if (r.character !== state.rig.character) status(`${file.name} is ${r.character}'s motion; this is ${state.rig.character}`, "bad");
    if (!state.rig.files.includes(r.path)) state.rig.files.push(r.path);
    fillSelect($("files"), state.rig.files.map((f) => [f, f.replace(/^mods\\//, "").replace("/resources/anims/", ": ")]));
    $("files").value = r.path;
    await openFile(r.path);
  } catch (err) {
    status(err.message, "bad");
  }
};'''),
]

PAGE = [
    ('''<title>MoonMod Animation Editor</title>''', '''<title>Dark Cloud Animation Editor</title>'''),
    ('''    <h3>Character or monster</h3>''', '''    <div class="note"><a href="../" style="color: var(--accent)">Back to the game</a></div>
    <h3>Character or monster</h3>'''),
    ('''    <div class="row"><button id="newMotion">New from this pose</button></div>''',
     '''    <label>A motion file on this computer</label>
    <input type="file" id="localFile" accept=".json">
    <div class="row"><button id="newMotion">New from this pose</button></div>'''),
    ('''    <label>File (under mods/ or examples/)</label>''',
     '''    <label>File (mods/&lt;a stored mod&gt;/resources/anims/…)</label>'''),
    ('''    <div class="row"><label><input type="checkbox" id="sync" checked> copy into the play save's mod too</label></div>
    <div class="row"><button class="primary" id="save">Save</button> <span class="note"><kbd>Ctrl</kbd>+<kbd>S</kbd></span></div>''',
     '''    <input type="checkbox" id="sync" hidden>
    <div class="row"><button class="primary" id="save">Save</button> <span class="note"><kbd>Ctrl</kbd>+<kbd>S</kbd></span>
      <button id="download">Download</button></div>'''),
    ('''<script type="module" src="/editor.js"></script>''', '''<script src="../zip.js"></script>
<script type="module" src="editor.js"></script>'''),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    editor = pathlib.Path(sys.argv[1]) / "tools" / "anim" / "editor"
    SITE.mkdir(parents=True, exist_ok=True)
    for name, changes in (("editor.js", EDITOR), ("index.html", PAGE)):
        text = (editor / name).read_text(encoding="utf-8")
        (SITE / name).write_text(port(text, changes, name), encoding="utf-8", newline="\n")
    names = (editor / "monster_names.json").read_text(encoding="utf-8")
    (SITE / "monster_names.json").write_text(names, encoding="utf-8", newline="\n")
    print("ported the editor into", SITE)


if __name__ == "__main__":
    main()
