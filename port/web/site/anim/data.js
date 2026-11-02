// The animation editor's data in the browser: what tools/anim/editor/server.py answers on the desktop (MoonMod's
// repository), read from the player's own game data in this browser's storage, where the main page put it
// (shell.js: chronicle/disc.iso, chronicle/disc/, chronicle/data/ or chronicle/data.zip). Motions are saved into the
// mods stored beside the saves (chronicle/save/mods/), which the game loads, or downloaded.
//
// A rig's model is the glTF Kazarov's converter makes. A data folder or data.zip that dcdata extracted already holds
// it; for a disc, convert-worker.js runs the converter (dc_anim_convert, port/web/anim/convert.cpp) on the pack
// once, and the result is kept under chronicle/anim/.

const SECTOR = 2048;
const TYPES = { 0: "rotation", 1: "scale", 2: "translation" };
const EASES = [null, "in", "out", "inout"];

// The playable characters' dungeon packs, as moonanim.PACKS has them.
const PLAYERS = [["toan", "Toan", "c01d"], ["xiao", "Xiao", "c04b"], ["goro", "Goro", "c06b"], ["ruby", "Ruby", "c05a"],
  ["ungaga", "Ungaga", "c10b"], ["osmond", "Osmond", "c18a"]];

const fold = (path) => path.replace(/\\/g, "/").replace(/^\/+/, "").toLowerCase();
const u16 = (v, at) => v.getUint16(at, true);
const u32 = (v, at) => v.getUint32(at, true);
// Python's round(x, n): toFixed rounds the exact binary value, as Python does (x * 1e5 rounds it first).
const r6 = (x) => Number(x.toFixed(6));
const r5 = (x) => Number(x.toFixed(5));

async function bytesOf(blob) {
  return new Uint8Array(await blob.arrayBuffer());
}

// ---------------------------------------------------------------------------------------------------------------
// The game data: {kind, read(path) -> Blob or null, list(directory) -> [path]}, paths folded as dcdata folds them.

async function home() {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle("chronicle", { create: true });
}

async function child(directory, name, kind) {
  try {
    return kind === "file" ? await directory.getFileHandle(name) : await directory.getDirectoryHandle(name);
  } catch {
    return null;
  }
}

// A handle's entry by name, case folded.
async function childFolded(directory, name, kind) {
  for await (const [entry, handle] of directory.entries()) {
    if (handle.kind === kind && entry.toLowerCase() === name) return handle;
  }
  return null;
}

// DATA.HD2 as dcdata::ParseIndex reads it: 32-byte records, then their names. The first of each folded path wins.
function parseIndex(hd2) {
  const view = new DataView(hd2.buffer, hd2.byteOffset, hd2.byteLength);
  const count = u32(view, 0) >>> 5;
  const decoder = new TextDecoder("latin1");
  const files = new Map();
  for (let i = 0; i < count; i++) {
    const at = i * 32;
    const nameAt = u32(view, at);
    let end = nameAt;
    while (end < hd2.length && hd2[end] !== 0) end++;
    const name = decoder.decode(hd2.subarray(nameAt, end));
    const size = u32(view, at + 20);
    if (!name && i > 0 && size === 0) break;
    const path = fold(name);
    if (!files.has(path)) files.set(path, { offset: u32(view, at + 24) * SECTOR, size });
  }
  return files;
}

// DATA.DAT and DATA.HD2 in an ISO 9660 image's root directory.
async function isoRoot(image) {
  for (let sector = 16; ; sector++) {
    const d = new DataView(await image.slice(sector * SECTOR, (sector + 1) * SECTOR).arrayBuffer());
    if (d.byteLength < SECTOR || String.fromCharCode(...new Uint8Array(d.buffer, 1, 5)) !== "CD001") {
      throw new Error("the disc image has no ISO 9660 volume");
    }
    if (d.getUint8(0) !== 1) continue;
    const extent = u32(d, 156 + 2), size = u32(d, 156 + 10);
    const dir = new Uint8Array(await image.slice(extent * SECTOR, extent * SECTOR + size).arrayBuffer());
    const found = {};
    for (let at = 0; at < dir.length;) {
      const length = dir[at];
      if (length === 0) { at = (Math.floor(at / SECTOR) + 1) * SECTOR; continue; }
      const v = new DataView(dir.buffer, at, length);
      const name = String.fromCharCode(...dir.subarray(at + 33, at + 33 + dir[at + 32])).split(";")[0].replace(/\.$/, "");
      found[name.toUpperCase()] = image.slice(u32(v, 2) * SECTOR, u32(v, 2) * SECTOR + u32(v, 10));
      at += length;
    }
    if (!found["DATA.DAT"] || !found["DATA.HD2"]) throw new Error("the disc image has no DATA.DAT and DATA.HD2");
    return { dat: found["DATA.DAT"], hd2: found["DATA.HD2"] };
  }
}

function discSource(dat, files) {
  return {
    kind: "disc",
    async read(path) {
      const f = files.get(fold(path));
      return f ? dat.slice(f.offset, f.offset + f.size) : null;
    },
    async list(directory) {
      const prefix = fold(directory).replace(/\/?$/, "/");
      return [...files.keys()].filter((p) => p.startsWith(prefix) && !p.slice(prefix.length).includes("/"));
    },
  };
}

function folderSource(root) {
  async function walk(path, kind) {
    const parts = fold(path).split("/").filter(Boolean);
    let directory = root;
    for (const [i, part] of parts.entries()) {
      directory = await childFolded(directory, part, i === parts.length - 1 ? kind : "directory");
      if (!directory) return null;
    }
    return directory;
  }
  return {
    kind: "data",
    async read(path) {
      const handle = await walk(path, "file");
      return handle ? handle.getFile() : null;
    },
    async list(directory) {
      const handle = await walk(directory, "directory");
      const out = [];
      if (handle) {
        for await (const [name, entry] of handle.entries()) {
          if (entry.kind === "file") out.push(fold(directory).replace(/\/?$/, "/") + name.toLowerCase());
        }
      }
      return out;
    },
  };
}

async function zipSource(blob) {
  const entries = new Map();
  for (const [name, entry] of await window.dcZip.open(blob)) {
    // dcdata's data.zip holds data/... at its top
    entries.set(fold(name).replace(/^data\//, ""), entry);
  }
  return {
    kind: "data.zip",
    async read(path) {
      const e = entries.get(fold(path));
      return e ? e.read() : null;
    },
    async list(directory) {
      const prefix = fold(directory).replace(/\/?$/, "/");
      return [...entries.keys()].filter((p) => p.startsWith(prefix) && !p.slice(prefix.length).includes("/"));
    },
  };
}

// The game data this browser holds, in the order shell.js prefers it: a disc first.
async function openSource() {
  const dir = await home();
  const iso = await child(dir, "disc.iso", "file");
  if (iso) {
    const { dat, hd2 } = await isoRoot(await iso.getFile());
    return discSource(dat, parseIndex(await bytesOf(hd2)));
  }
  const disc = await child(dir, "disc", "directory");
  if (disc) {
    const dat = await childFolded(disc, "data.dat", "file");
    const hd2 = await childFolded(disc, "data.hd2", "file");
    if (dat && hd2) return discSource(await dat.getFile(), parseIndex(await bytesOf(await hd2.getFile())));
  }
  const data = await child(dir, "data", "directory");
  if (data) return folderSource(data);
  const zip = await child(dir, "data.zip", "file");
  if (zip) return zipSource(await zip.getFile());
  throw new Error("this browser holds no game data yet: add your disc on the main page first");
}

// ---------------------------------------------------------------------------------------------------------------
// Packs and motions (moonanim.py)

function packMembers(data) {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const decoder = new TextDecoder("latin1");
  const out = new Map();
  let off = 0;
  while (off + 76 <= data.length) {
    let end = off;
    while (end < off + 64 && data[end] !== 0) end++;
    const name = decoder.decode(data.subarray(off, end));
    if (!name) break;
    const o = view.getInt32(off + 64, true), size = view.getInt32(off + 68, true), next = view.getInt32(off + 72, true);
    out.set(name, data.subarray(off + o, off + o + size));
    if (next < 76) break;
    off += next;
  }
  return out;
}

function parseMot(data) {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const channels = [];
  for (let off = 0; ;) {
    const frame = u32(view, off), target = u32(view, off + 4), type = view.getInt32(off + 8, true);
    const count = u32(view, off + 16), more = u32(view, off + 20);
    off += 32;
    const keys = [];
    for (let i = 0; i < count; i++) {
      const f = u32(view, off);
      keys.push([f, [16, 20, 24, 28].map((d) => view.getFloat32(off + d, true))]);
      off += 32;
    }
    channels.push({ frame, target, type, keys });
    if (more === 0) return channels;
  }
}

function qnorm(q) {
  const n = Math.hypot(...q) || 1;
  return q.map((c) => c / n);
}

function qslerp(a, b, t) {
  let dot = a.reduce((s, x, i) => s + x * b[i], 0);
  if (dot < 0) { b = b.map((c) => -c); dot = -dot; }
  if (dot > 0.9995) return qnorm(a.map((x, i) => x + (b[i] - x) * t));
  const th = Math.acos(Math.min(1, dot)), s = Math.sin(th);
  const wa = Math.sin((1 - t) * th) / s, wb = Math.sin(t * th) / s;
  return a.map((x, i) => wa * x + wb * b[i]);
}

const gameToGltf = ([w, x, y, z]) => qnorm([-x, -y, -z, w]);
const gltfToGame = ([x, y, z, w]) => [w, -x, -y, -z];
const lerp = (a, b, t) => a.map((x, i) => x + (b[i] - x) * t);

// The pose at a (fractional) native frame: values in the rig's channel order, rotations as glTF quaternions.
function sample(rig, frame) {
  return rig.channels.map((c) => {
    const keys = c.keys;
    let prev = keys[0], next = null;
    for (const k of keys) {
      if (k[0] <= frame) prev = k;
      else { next = k; break; }
    }
    const kind = TYPES[c.type];
    if (next === null || prev[0] === frame || prev[0] > frame) {
      return kind === "rotation" ? gameToGltf(prev[1]) : prev[1].slice(0, 3);
    }
    const t = (frame - prev[0]) / (next[0] - prev[0]);
    return kind === "rotation" ? qslerp(gameToGltf(prev[1]), gameToGltf(next[1]), t)
      : lerp(prev[1].slice(0, 3), next[1].slice(0, 3), t);
  });
}

// A cfg comment as game_names spells it (server.py's _plain).
const plain = (comment) => comment.replace(/\s+/gu, "").replace(/^\p{Nd}+|\p{Nd}+$/gu, "")
  .replace(/−/g, "ー").replace(/－/g, "ー");

function motionTable(cfgText, gameJson) {
  const motions = [], names = [];
  const english = new Map();
  for (const [key, value] of Object.entries(gameJson?.game_names ?? {})) english.set(plain(String(value?.value ?? "")), key);
  for (const raw of cfgText.split(/\r?\n/)) {
    const line = raw.trim();
    const parts = line.replace(/,/g, " ").split(/\s+/);
    if (parts.length >= 4 && parts[0] === "KEY") {
      const start = Number.parseInt(parts[1], 10), end = Number.parseInt(parts[2], 10), speed = Number.parseFloat(parts[3]);
      if (/^-?\d+$/.test(parts[1]) && /^-?\d+$/.test(parts[2]) && !Number.isNaN(speed)) motions.push([start, end, speed]);
    }
    if (line.startsWith("KEY") && !line.startsWith("KEY_")) {
      const comment = line.includes("//") ? line.split("//").slice(1).join("//").trim() : "";
      names.push(english.get(plain(comment)) ?? (comment || "(unnamed)"));
    }
  }
  return { motions, names };
}

// ---------------------------------------------------------------------------------------------------------------
// Models

let converter = null; // the worker, made on first use

function convertPack(path, bytes) {
  converter ??= new Worker("convert-worker.js");
  return new Promise((resolve, reject) => {
    const id = Math.random().toString(36).slice(2);
    const done = ({ data }) => {
      if (data.id !== id) return;
      converter.removeEventListener("message", done);
      if (data.error) reject(new Error(data.error));
      else resolve(data.files);
    };
    converter.addEventListener("message", done);
    converter.postMessage({ id, path, bytes }, [bytes.buffer]);
  });
}

// The converted files of a pack kept under chronicle/anim/: {read(path) -> Blob or null}, or null when not there.
async function cachedModel(path) {
  const parts = ["anim", ...fold(path).split("/")];
  let dir = await home();
  for (const part of parts.slice(0, -1)) {
    dir = await child(dir, part, "directory");
    if (!dir) return null;
  }
  const done = await child(dir, parts.at(-1).replace(/\.chr$/, ".done"), "file");
  return done ? folderSource(await (await home()).getDirectoryHandle("anim")) : null;
}

async function storeConverted(path, files) {
  const root = await (await home()).getDirectoryHandle("anim", { create: true });
  const write = async (rel, bytes) => {
    const parts = fold(rel).split("/");
    let dir = root;
    for (const part of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(part, { create: true });
    const w = await (await dir.getFileHandle(parts.at(-1), { create: true })).createWritable();
    await w.write(bytes);
    await w.close();
  };
  for (const [rel, bytes] of files) await write(rel, bytes);
  await write(fold(path).replace(/\.chr$/, ".done"), new Uint8Array(0)); // written last: the conversion is whole
  return folderSource(root);
}

// The glTF beside a pack, with where its buffers and images come from.
async function modelFor(source, chrPath, status) {
  const gltfPath = chrPath.replace(/\.chr$/, ".gltf");
  let from = (await source.read(gltfPath)) ? source : await cachedModel(chrPath);
  if (!from) {
    status?.("Converting " + chrPath + " (once)…");
    const chr = await source.read(chrPath);
    from = await storeConverted(chrPath, await convertPack(fold(chrPath), await bytesOf(chr)));
  }
  const gltf = JSON.parse(await (await from.read(gltfPath)).text());
  const gameBlob = await from.read(chrPath.replace(/\.chr$/, ".game.json"));
  const game = gameBlob ? JSON.parse(await gameBlob.text()) : null;
  return { gltf, from, gltfPath, game };
}

const join = (base, rel) => {
  const out = base.split("/").slice(0, -1);
  for (const part of decodeURIComponent(rel).split("/")) {
    if (part === "..") out.pop();
    else if (part && part !== ".") out.push(part);
  }
  return out.join("/");
};

// The model as server.py's /model/ serves it, a blob: URL with its buffers and images as blob: URLs too.
async function modelUrl(model) {
  const g = structuredClone(model.gltf);
  delete g.animations; // the native motions come from the pack
  g.scenes = g.scenes.filter((s) => s.nodes.includes(0));
  if (g.scenes.length === 0) g.scenes = model.gltf.scenes.slice(0, 1);
  g.scene = 0;
  for (const item of [...(g.buffers ?? []), ...(g.images ?? [])]) {
    if (item.uri && !item.uri.startsWith("data:")) {
      const blob = await model.from.read(join(model.gltfPath, item.uri));
      if (!blob) throw new Error("the model's " + item.uri + " is missing");
      item.uri = URL.createObjectURL(blob);
    }
  }
  return URL.createObjectURL(new Blob([JSON.stringify(g)], { type: "model/gltf+json" }));
}

// ---------------------------------------------------------------------------------------------------------------
// Mods: chronicle/save/mods/<id>/, the editor's "mods/<id>/..." paths

async function modsRoot(create) {
  const dir = await home();
  const save = create ? await dir.getDirectoryHandle("save", { create: true }) : await child(dir, "save", "directory");
  if (!save) return null;
  return create ? save.getDirectoryHandle("mods", { create: true }) : child(save, "mods", "directory");
}

// A "mods/<id>/....json" path as [id, parts after it], or null.
function modPath(rel) {
  const parts = rel.replace(/\\/g, "/").replace(/^\/+/, "").split("/");
  if (parts[0] !== "mods" || parts.length < 3 || !parts.at(-1).endsWith(".json")) return null;
  if (parts.some((p) => p === "" || p === "." || p === "..")) return null;
  return [parts[1], parts.slice(2)];
}

async function modFile(rel, create) {
  const where = modPath(rel);
  if (!where) return null;
  const mods = await modsRoot(false);
  let dir = mods && await child(mods, where[0], "directory");
  if (!dir) return null;
  for (const part of where[1].slice(0, -1)) {
    dir = create ? await dir.getDirectoryHandle(part, { create: true }) : await child(dir, part, "directory");
    if (!dir) return null;
  }
  return create ? dir.getFileHandle(where[1].at(-1), { create: true }) : child(dir, where[1].at(-1), "file");
}

async function storedMods() {
  const mods = await modsRoot(false);
  const out = [];
  if (mods) for await (const [id, h] of mods.entries()) if (h.kind === "directory") out.push(id);
  return out.sort();
}

// The motion files the stored mods hold for `character` (server.py's motion_files).
async function motionFiles(character) {
  const mods = await modsRoot(false);
  const out = [];
  if (!mods) return out;
  for (const id of await storedMods()) {
    const mod = await child(mods, id, "directory");
    const resources = mod && await child(mod, "resources", "directory");
    const anims = resources && await child(resources, "anims", "directory");
    if (!anims) continue;
    for await (const [name, h] of anims.entries()) {
      if (h.kind !== "file" || !name.endsWith(".json")) continue;
      const head = await (await h.getFile()).slice(0, 160).text();
      const m = head.match(/"character"\s*:\s*"([^"]+)"/);
      if (head.includes('"moonmod-motion"') && m && m[1] === character) out.push(`mods/${id}/resources/anims/${name}`);
    }
  }
  return out.sort();
}

// ---------------------------------------------------------------------------------------------------------------
// Baking (moonanim.bake, motion_document) and reading back (server.py's document_frames)

const ease = (t, kind) => kind === "in" ? t * t : kind === "out" ? 1 - (1 - t) * (1 - t)
  : kind === "inout" ? t * t * (3 - 2 * t) : t;

function blend(rig, a, b, t) {
  return a.map((va, i) => TYPES[rig.channels[i].type] === "rotation" ? qslerp(va, b[i], t) : lerp(va, b[i], t));
}

function bake(rig, keyposes) {
  const frames = [];
  for (let k = 0; k + 1 < keyposes.length; k++) {
    const [f0, p0] = keyposes[k], [f1, p1, e] = keyposes[k + 1];
    for (let f = f0; f < f1; f++) frames.push([f, blend(rig, p0, p1, ease((f - f0) / (f1 - f0), e))]);
  }
  frames.push([keyposes.at(-1)[0], keyposes.at(-1)[1]]);
  return frames;
}

function motionDocument(rig, character, frames, speed) {
  const tracks = rig.channels.map((c, i) => {
    const kind = TYPES[c.type];
    const keys = frames.map(([f, pose]) => kind === "rotation" ? [f, ...gltfToGame(pose[i]).map(r6)] : [f, ...pose[i].map(r5)]);
    return { bone: c.bone, type: kind, keys };
  });
  return { format: "moonmod-motion", version: 1, character, length: frames.at(-1)[0], speed, tracks };
}

function documentFrames(rig, doc) {
  const tracks = new Map((doc.tracks ?? []).map((t) => [t.bone + "|" + t.type, t.keys]));
  const rest = sample(rig, rig.motions.length ? rig.motions[0][0] : 0);
  const at = new Set();
  for (const keys of tracks.values()) for (const k of keys) at.add(k[0]);
  const framesAt = [...(at.size ? at : [0])].sort((a, b) => a - b);
  const value = (keys, f, kind) => {
    let prev = keys[0];
    for (const k of keys) {
      if (k[0] <= f) { prev = k; continue; }
      const u = (f - prev[0]) / ((k[0] - prev[0]) || 1);
      return kind === "rotation" ? qslerp(gameToGltf(prev.slice(1, 5)), gameToGltf(k.slice(1, 5)), u)
        : lerp(prev.slice(1, 4), k.slice(1, 4), u);
    }
    return kind === "rotation" ? gameToGltf(prev.slice(1, 5)) : prev.slice(1, 4);
  };
  const frames = framesAt.map((f) => ({
    frame: f,
    values: rig.channels.map((c, i) => {
      const kind = TYPES[c.type];
      const keys = tracks.get(c.bone + "|" + kind);
      return (keys ? value(keys, f, kind) : rest[i]).map(r6);
    }),
  }));
  return { speed: doc.speed ?? 0.5, length: doc.length ?? framesAt.at(-1), frames };
}

// ---------------------------------------------------------------------------------------------------------------
// The editor's /api calls

let source = null;
let entries = null; // id -> {id, name, group, character, chr}
const rigs = new Map();
const opened = new Map(); // "upload/<name>" -> a document opened from this computer
let monsterNames = null;

async function catalog() {
  if (entries) return entries;
  source = await openSource();
  monsterNames ??= await (await fetch("monster_names.json")).json();
  entries = new Map();
  for (const [id, name, pack] of PLAYERS) {
    const chr = `dun/mainchara/${pack}.chr`;
    if (await source.read(chr)) entries.set(id, { id, name, group: "Characters", character: id, chr });
  }
  for (const chr of (await source.list("dun/monstor")).filter((p) => p.endsWith(".chr")).sort()) {
    const pack = chr.split("/").at(-1).slice(0, -4);
    if (pack.endsWith("_") || pack.endsWith("_s")) continue; // a backup beside the real pack, or a beam's shadow
    const name = monsterNames[pack];
    entries.set(pack, { id: pack, name: name ? `${name} (${pack})` : pack, group: name ? "Monsters" : "Other models",
      character: pack, chr });
  }
  return entries;
}

async function rig(id, status) {
  if (rigs.has(id)) return rigs.get(id);
  const e = (await catalog()).get(id);
  if (!e) throw new Error("no rig " + id);
  const members = packMembers(await bytesOf(await source.read(e.chr)));
  const motName = [...members.keys()].find((n) => n.endsWith(".mot") && !n.endsWith("s.mot"));
  const cfgName = members.has("base.cfg") ? "base.cfg" : "info.cfg";
  const model = await modelFor(source, e.chr, status);
  const names = model.gltf.nodes.map((n) => n.name);
  const channels = parseMot(members.get(motName)).filter((c) => c.type in TYPES);
  for (const c of channels) c.bone = names[c.frame];
  const table = motionTable(new TextDecoder("shift_jis").decode(members.get(cfgName) ?? new Uint8Array()), model.game);
  const r = { entry: e, model, channels, motions: table.motions, motionNames: table.names, url: null };
  rigs.set(id, r);
  return r;
}

async function rigInfo(id, status) {
  const r = await rig(id, status);
  r.url ??= await modelUrl(r.model);
  const e = r.entry;
  return {
    id, name: e.name, group: e.group, character: e.character, playable: e.group === "Characters", model: r.url,
    channels: r.channels.map((c) => ({ node: c.frame, bone: c.bone, kind: TYPES[c.type] })),
    motions: r.motions.map(([start, end, speed], index) => ({ index, name: r.motionNames[index] ?? "", start, end, speed })),
    files: [...await motionFiles(e.character), ...[...opened.keys()]],
  };
}

function nativeFrames(r, motion) {
  const [start, end, speed] = r.motions[motion];
  const frames = [];
  for (let i = 0; i <= Math.trunc(end - start); i++) frames.push({ frame: i, values: sample(r, start + i).map((v) => v.map(r6)) });
  return { speed: speed || 0.5, length: Math.trunc(end - start), frames };
}

// The editor's keys as a moonmod-motion document (server.py's save, without the writing).
function bakeBody(r, body) {
  const speed = Number(body.speed);
  if (!(speed >= 0.05 && speed <= 4)) throw new Error("speed is 0.05 to 4");
  const keys = [...body.keys].sort((a, b) => a.frame - b.frame);
  if (!keys.length || keys[0].frame !== 0) throw new Error("the motion needs a key at frame 0");
  const poses = keys.map((k) => {
    if (k.values.length !== r.channels.length) {
      throw new Error(`frame ${k.frame}: ${k.values.length} channel values for ${r.channels.length} channels`);
    }
    return [Math.trunc(k.frame), k.values, EASES.includes(k.ease) ? k.ease : null];
  });
  const frames = poses.length > 1 ? bake(r, poses) : [[0, poses[0][1]]];
  return { doc: motionDocument(r, r.entry.character, frames, speed), frames: frames.length };
}

async function save(body) {
  const r = await rig(body.id);
  const { doc, frames } = bakeBody(r, body);
  const text = JSON.stringify(doc);
  const where = modPath(body.path ?? "");
  if (!where) throw new Error("save as mods/<mod id>/resources/anims/<name>.json");
  const mods = await storedMods();
  if (!mods.includes(where[0])) {
    throw new Error(`this browser holds no mod ${where[0]} (${mods.join(", ") || "none yet"}): add it on the main page, ` +
      "or use Download");
  }
  const w = await (await modFile(body.path, true)).createWritable();
  await w.write(text);
  await w.close();
  return { path: body.path, frames, length: doc.length, synced: [], bytes: new Blob([text]).size };
}

function download(body) {
  return rig(body.id).then((r) => {
    const { doc } = bakeBody(r, body);
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([JSON.stringify(doc)], { type: "application/json" }));
    a.download = (body.path || "motion.json").split("/").at(-1);
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    return { name: a.download };
  });
}

async function load(id, path) {
  const r = await rig(id);
  let doc = opened.get(path);
  if (!doc) {
    const handle = await modFile(path, false);
    if (!handle) throw new Error("no such motion file");
    doc = JSON.parse(await (await handle.getFile()).text());
  }
  return documentFrames(r, doc);
}

// A motion file from this computer, listed with the mods' files as upload/<name> while the page is open.
export async function addLocalFile(file) {
  const doc = JSON.parse(await file.text());
  if (doc.format !== "moonmod-motion") throw new Error(file.name + " is not a moonmod-motion file");
  const path = "upload/" + file.name;
  opened.set(path, doc);
  return { path, character: doc.character };
}

export { download };

// server.py's routes, answered here. `status` shows progress (a conversion takes a few seconds).
export async function api(path, body, status) {
  const url = new URL(path, location.href);
  const q = Object.fromEntries(url.searchParams);
  switch (url.pathname.replace(/^.*\/api\//, "/api/")) {
    case "/api/rigs":
      return [...(await catalog()).values()].map(({ id, name, group }) => ({ id, name, group }));
    case "/api/rig":
      return rigInfo(q.id, status);
    case "/api/native":
      return nativeFrames(await rig(q.id), Number(q.motion));
    case "/api/load":
      return load(q.id, q.path);
    case "/api/save":
      return save(body);
    case "/api/ref/groups":
      return { available: false }; // the Reference panel needs files only the desktop has
    default:
      throw new Error("not available in the browser: " + path);
  }
}
