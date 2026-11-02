// MoonMod animation editor, as DC_Web serves it (port/web/anim/sync_editor.py copied it from MoonMod's
// tools/anim/editor): data.js answers what server.py answers on the desktop, from the player's own game data.
//
// MoonMod animation editor: the page. server.py serves the rigs, their native motions and the motion files, and bakes
// and writes what is saved. Here a motion is a list of keys {frame, ease, values}: values holds one glTF value per
// channel of the rig (rotations x, y, z, w; translations and scales x, y, z), in the rig's channel order. Between
// two keys the pose blends as moonanim.bake does: rotations slerp, the rest lerp, eased by the later key's ease.

import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import { TransformControls } from "three/addons/controls/TransformControls.js";
import * as local from "./data.js";

const $ = (id) => document.getElementById(id);
const TICKS = 60; // the game's logic ticks per second

const state = {
  rig: null,          // /api/rig answer
  model: null,        // the model's root
  nodes: [],          // Object3D by glTF node index
  channels: [],       // [{node, bone, kind}]
  byNode: new Map(),  // node index -> {rotation: channel index, translation: ..., scale: ...}
  keys: [],
  length: 30,
  speed: 0.5,
  frame: 0,
  playing: false,
  selectedKey: -1,
  selected: -1,       // selected node index
  mode: "rotate",
  clipboard: null,
  undo: [], redo: [],
  sourceName: "",
};

// ---------------------------------------------------------------------------------------------------------------
// Scene

const view = $("view");
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(window.devicePixelRatio);
renderer.outputColorSpace = THREE.SRGBColorSpace;
view.prepend(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x2a2d33);
const camera = new THREE.PerspectiveCamera(40, 1, 0.1, 2000);
camera.position.set(28, 18, 34);
const orbit = new OrbitControls(camera, renderer.domElement);
orbit.target.set(0, 7, 0);
orbit.update();
scene.add(new THREE.HemisphereLight(0xffffff, 0x50505a, 2.2));
const sun = new THREE.DirectionalLight(0xffffff, 1.6);
sun.position.set(20, 40, 25);
scene.add(sun);
const grid = new THREE.GridHelper(120, 60, 0x60646c, 0x3a3d44);
scene.add(grid);
const forward = new THREE.ArrowHelper(new THREE.Vector3(0, 0, 1), new THREE.Vector3(0, 0.05, 0), 8, 0xe0a63a, 1.2, 0.8);
scene.add(forward); // the way the character faces (+Z)

const stage = new THREE.Group(); // carries the model along the travel preview
scene.add(stage);

const gizmo = new TransformControls(camera, renderer.domElement);
gizmo.setSize(0.8);
gizmo.setSpace("local");
scene.add(gizmo);
gizmo.addEventListener("dragging-changed", (e) => {
  orbit.enabled = !e.value;
  if (e.value) pushUndo();
});
gizmo.addEventListener("objectChange", onGizmoChange);

const ikTarget = new THREE.Object3D();
scene.add(ikTarget);

// Bone dots and lines, drawn over the model.
const dotGeo = new THREE.SphereGeometry(0.32, 10, 8);
const dotMat = new THREE.MeshBasicMaterial({ color: 0x5fb3f9, depthTest: false, transparent: true, opacity: 0.85 });
const dotSelMat = new THREE.MeshBasicMaterial({ color: 0xe0a63a, depthTest: false });
const dotEndMat = new THREE.MeshBasicMaterial({ color: 0x9a9ca3, depthTest: false, transparent: true, opacity: 0.7 });
const skeleton = new THREE.Group();
scene.add(skeleton);
let dots = [];      // [{mesh, node}]
let boneLines = null;

function resize() {
  const r = view.getBoundingClientRect();
  renderer.setSize(r.width, r.height);
  camera.aspect = r.width / Math.max(1, r.height);
  camera.updateProjectionMatrix();
  drawTimeline();
}
window.addEventListener("resize", resize);

// ---------------------------------------------------------------------------------------------------------------
// Server

async function api(path, body) {
  return local.api(path, body, (text) => status(text));
}

function status(text, kind) {
  const el = $("status");
  el.textContent = text;
  el.className = kind || "";
}

// ---------------------------------------------------------------------------------------------------------------
// Rigs

async function loadRigList() {
  const rigs = await api("/api/rigs");
  const sel = $("rig");
  sel.innerHTML = "";
  const groups = {};
  for (const r of rigs) {
    if (!groups[r.group]) {
      groups[r.group] = document.createElement("optgroup");
      groups[r.group].label = r.group;
      sel.appendChild(groups[r.group]);
    }
    const o = document.createElement("option");
    o.value = r.id;
    o.textContent = r.name;
    groups[r.group].appendChild(o);
  }
  const want = new URLSearchParams(location.search).get("rig") || localStorage.getItem("moonanim.rig") || "toan";
  if ([...sel.options].some((o) => o.value === want)) sel.value = want;
  await openRig(sel.value);
}

async function openRig(id) {
  status("Loading " + id + "…");
  try { localStorage.setItem("moonanim.rig", id); } catch (e) { /* private window */ }
  const info = await api("/api/rig?id=" + encodeURIComponent(id));
  // a full URL, so the loader resolves the model's /raw/ paths against the origin rather than appending them
  const gltf = await new GLTFLoader().loadAsync(info.model);
  if (state.model) stage.remove(state.model);
  gizmo.detach();
  state.rig = info;
  state.model = gltf.scene;
  state.nodes = [];
  gltf.scene.traverse((o) => {
    const a = gltf.parser.associations.get(o);
    if (a && a.nodes !== undefined && state.nodes[a.nodes] === undefined) state.nodes[a.nodes] = o;
    if (o.isMesh) o.frustumCulled = false;
  });
  fixSkins(gltf);
  stage.add(gltf.scene);
  state.channels = info.channels;
  state.byNode = new Map();
  info.channels.forEach((c, i) => {
    if (!state.byNode.has(c.node)) state.byNode.set(c.node, {});
    state.byNode.get(c.node)[c.kind] = i;
  });
  $("rigNote").innerHTML = info.playable ? "" :
    '<span class="warn">Monster motions save and reopen here, but the engine can\'t play a mod motion on a monster yet.</span>';
  fillSelect($("native"), info.motions.map((m) => [m.index, `${m.index}: ${m.name}  (${m.end - m.start} frames)`]));
  fillSelect($("files"), info.files.map((f) => [f, f.replace(/^mods\//, "").replace("/resources/anims/", ": ")]));
  buildSkeleton();
  buildBoneList();
  state.selected = -1;
  if (info.motions.length) await openNative(0);
  else setMotion({ frames: [{ frame: 0, values: currentValues() }], length: 30, speed: 0.5 }, "new_move", null);
  state.rigHeight = posedBox(new THREE.Box3(), true).getSize(new THREE.Vector3()).y;
  frameCamera();
  status(`${info.name}: ${info.channels.length} animated channels, ${info.motions.length} native motions`, "good");
}

// The converter stores each joint's inverse bind as the inverse of its own bind-pose (BBP) matrix, local to its
// parent, where glTF wants the whole chain's. Dark Cloud skins as MotionProc2 (gameutil.cpp) does: a joint's bind
// matrix is the product of the bind matrices down from the skin's parent, the joints' from their BBP and every other
// node's from its own local transform; the vertices are in the space of the skin's parent. So each joint's inverse
// bind is rebuilt from that chain, and the mesh binds with no matrix of its own. Its texture is opaque, but its
// material asks for blending, which draws it unsorted against the rigid parts; it is drawn opaque.
function fixSkins(gltf) {
  const local = new Map();
  gltf.scene.updateMatrixWorld(true);
  gltf.scene.traverse((o) => { o.updateMatrix(); local.set(o, o.matrix.clone()); });
  const done = new Set();
  gltf.scene.traverse((mesh) => {
    if (!mesh.isSkinnedMesh) return;
    const nodeOf = (o) => gltf.parser.associations.get(o)?.nodes;
    // a node with several primitives is a group of meshes: the skin's node is then the group
    const owner = nodeOf(mesh) === undefined && mesh.parent ? mesh.parent : mesh;
    const skinRoot = owner.parent;
    const sk = mesh.skeleton;
    if (!done.has(sk)) {
      done.add(sk);
      const bindLocal = new Map(sk.bones.map((b, i) => [b, sk.boneInverses[i].clone().invert()]));
      const chain = (o) => (!o || o === skinRoot || o === gltf.scene) ? new THREE.Matrix4()
        : chain(o.parent).multiply(bindLocal.get(o) || local.get(o));
      sk.bones.forEach((b, i) => sk.boneInverses[i].copy(chain(b)).invert());
    }
    mesh.bindMode = "attached";
    mesh.bindMatrix.identity();
    mesh.material.transparent = false;
    mesh.material.depthWrite = true;
  });
}

function fillSelect(sel, items) {
  sel.innerHTML = "";
  for (const [value, text] of items) {
    const o = document.createElement("option");
    o.value = value;
    o.textContent = text;
    sel.appendChild(o);
  }
}

// Frames the posed model.
function frameCamera() {
  placeCamera(posedBox());
}

// The posed model's box: its bones, and its meshes as posed (a skinned mesh's own box is its bind pose). Some packs
// keep helper nodes well away from the body; `meshesOnly` leaves the bones out.
function posedBox(box = new THREE.Box3(), meshesOnly = false) {
  state.model.updateMatrixWorld(true);
  const v = new THREE.Vector3();
  state.model.traverse((o) => {
    if (o.isSkinnedMesh) {
      const p = o.geometry.attributes.position;
      for (let i = 0; i < p.count; i += 7) box.expandByPoint(o.applyBoneTransform(i, v.fromBufferAttribute(p, i)).applyMatrix4(o.matrixWorld));
    } else if (o.isMesh) {
      box.expandByObject(o);
    } else if (!meshesOnly) {
      box.expandByPoint(o.getWorldPosition(v));
    }
  });
  return box;
}

function placeCamera(box) {
  const size = box.getSize(new THREE.Vector3());
  const center = box.getCenter(new THREE.Vector3());
  const h = Math.max(size.y, size.x, size.z, 4);
  orbit.target.copy(center);
  camera.position.set(center.x + h * 1.1, center.y + h * 0.5, center.z + h * 1.4);
  camera.near = h / 200;
  camera.far = h * 200;
  camera.updateProjectionMatrix();
  orbit.update();
  const s = h / 40;
  for (const d of dots) d.mesh.scale.setScalar(s);
  gizmo.setSize(Math.min(1.2, 0.5 + h / 60));
}

// ---------------------------------------------------------------------------------------------------------------
// Bones: dots on every animated node and on the ends of chains (hands, feet), lines to their parents

function animatedAncestor(o) {
  for (let p = o.parent; p; p = p.parent) {
    const i = nodeIndex(p);
    if (i >= 0 && state.byNode.get(i)?.rotation !== undefined) return p;
  }
  return null;
}

function nodeIndex(o) { return state.nodes.indexOf(o); }

function pickable(i) {
  const o = state.nodes[i];
  if (!o || o.isMesh || o.isSkinnedMesh) return false;
  if (state.byNode.has(i)) return true;
  // an end point (a foot, a hand): an unanimated node with an animated parent and no animated node below it
  const parentAnimated = o.parent && state.byNode.get(nodeIndex(o.parent))?.rotation !== undefined;
  let below = false;
  o.traverse((c) => { if (c !== o && state.byNode.has(nodeIndex(c))) below = true; });
  return parentAnimated && !below;
}

function buildSkeleton() {
  skeleton.clear();
  dots = [];
  state.nodes.forEach((o, i) => {
    if (!pickable(i)) return;
    const animated = state.byNode.has(i);
    const mesh = new THREE.Mesh(dotGeo, animated ? dotMat : dotEndMat);
    mesh.renderOrder = 10;
    mesh.userData.node = i;
    skeleton.add(mesh);
    dots.push({ mesh, node: i });
  });
  const pairs = dots.map((d) => [d.node, animatedAncestor(state.nodes[d.node])]).filter((p) => p[1]);
  const geo = new THREE.BufferGeometry();
  geo.setAttribute("position", new THREE.BufferAttribute(new Float32Array(pairs.length * 6), 3));
  boneLines = new THREE.LineSegments(geo, new THREE.LineBasicMaterial({ color: 0x5fb3f9, depthTest: false, transparent: true, opacity: 0.6 }));
  boneLines.userData.pairs = pairs;
  boneLines.renderOrder = 9;
  skeleton.add(boneLines);
}

const tmpV = new THREE.Vector3();
function updateSkeleton() {
  state.model?.updateMatrixWorld(true);
  for (const d of dots) {
    state.nodes[d.node].getWorldPosition(d.mesh.position);
    d.mesh.material = d.node === state.selected ? dotSelMat : (state.byNode.has(d.node) ? dotMat : dotEndMat);
  }
  if (boneLines) {
    const pos = boneLines.geometry.attributes.position;
    boneLines.userData.pairs.forEach(([i, parent], k) => {
      state.nodes[i].getWorldPosition(tmpV);
      pos.setXYZ(k * 2, tmpV.x, tmpV.y, tmpV.z);
      parent.getWorldPosition(tmpV);
      pos.setXYZ(k * 2 + 1, tmpV.x, tmpV.y, tmpV.z);
    });
    pos.needsUpdate = true;
  }
  skeleton.visible = $("showSkel").checked;
}

function buildBoneList() {
  const box = $("bones");
  box.innerHTML = "";
  const walk = (o, depth) => {
    const i = nodeIndex(o);
    if (i >= 0 && pickable(i)) {
      const div = document.createElement("div");
      div.textContent = " ".repeat(depth * 2) + (o.name || "node " + i);
      div.dataset.node = i;
      if (!state.byNode.has(i)) div.className = "dim";
      div.onclick = () => select(i);
      box.appendChild(div);
      depth++;
    }
    for (const c of o.children) walk(c, depth);
  };
  walk(state.model, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Poses

function currentValues() {
  return state.channels.map((c) => readChannel(state.nodes[c.node], c.kind));
}

function readChannel(o, kind) {
  if (kind === "rotation") return [o.quaternion.x, o.quaternion.y, o.quaternion.z, o.quaternion.w];
  const v = kind === "translation" ? o.position : o.scale;
  return [v.x, v.y, v.z];
}

function applyValues(values) {
  state.channels.forEach((c, i) => {
    const o = state.nodes[c.node];
    const v = values[i];
    if (!o || !v) return;
    if (c.kind === "rotation") o.quaternion.set(v[0], v[1], v[2], v[3]).normalize();
    else if (c.kind === "translation") o.position.set(v[0], v[1], v[2]);
    else o.scale.set(v[0], v[1], v[2]);
  });
}

function ease(t, kind) {
  if (kind === "in") return t * t;
  if (kind === "out") return 1 - (1 - t) * (1 - t);
  if (kind === "inout") return t * t * (3 - 2 * t);
  return t;
}

const qa = new THREE.Quaternion(), qb = new THREE.Quaternion();
function blendValues(a, b, t) {
  return state.channels.map((c, i) => {
    if (c.kind === "rotation") {
      qa.set(...a[i]); qb.set(...b[i]);
      qa.slerp(qb, t);
      return [qa.x, qa.y, qa.z, qa.w];
    }
    return a[i].map((x, k) => x + (b[i][k] - x) * t);
  });
}

function valuesAt(frame) {
  const keys = state.keys;
  if (!keys.length) return null;
  if (frame <= keys[0].frame) return keys[0].values;
  for (let k = 1; k < keys.length; k++) {
    if (frame <= keys[k].frame) {
      const a = keys[k - 1], b = keys[k];
      if (frame === b.frame) return b.values;
      return blendValues(a.values, b.values, ease((frame - a.frame) / (b.frame - a.frame), b.ease));
    }
  }
  return keys[keys.length - 1].values;
}

function showFrame() {
  const v = valuesAt(state.frame);
  if (v) applyValues(v);
  placeTravel();
  placeReference();
  updateSkeleton();
  if (state.mode === "ik" && state.selected >= 0 && !gizmo.dragging) placeIkTarget();
  refreshBonePanel();
  $("frame").value = Math.round(state.frame * 100) / 100;
  const ticks = state.frame / state.speed;
  $("timeNote").textContent = `tick ${ticks.toFixed(0)} of ${(state.length / state.speed).toFixed(0)}  ·  ${(state.length / state.speed / TICKS).toFixed(2)} s at speed ${state.speed}`;
  if (ref.figure.visible) $("timeNote").textContent += `  ·  reference frame ${ref.frame.toFixed(1)}`;
  drawTimeline();
}

function keyIndexAt(frame) { return state.keys.findIndex((k) => k.frame === frame); }

// The key at the current (whole) frame, made from the current pose if there is none.
function keyHere() {
  const f = Math.round(state.frame);
  state.frame = f;
  let i = keyIndexAt(f);
  if (i < 0) {
    const values = valuesAt(f).map((v) => v.slice());
    state.keys.push({ frame: f, ease: null, values });
    state.keys.sort((a, b) => a.frame - b.frame);
    i = keyIndexAt(f);
  }
  state.selectedKey = i;
  return state.keys[i];
}

function storeNode(nodeIdx) {
  const key = keyHere();
  const ch = state.byNode.get(nodeIdx) || {};
  for (const kind of ["rotation", "translation", "scale"]) {
    if (ch[kind] !== undefined) key.values[ch[kind]] = readChannel(state.nodes[nodeIdx], kind);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Opening and saving

function setMotion(data, name, path) {
  state.keys = data.frames.map((f) => ({ frame: f.frame, ease: null, values: f.values }));
  state.length = Math.max(1, data.length || state.keys[state.keys.length - 1].frame);
  state.speed = data.speed || 0.5;
  state.frame = 0;
  state.selectedKey = 0;
  state.undo = []; state.redo = [];
  state.sourceName = name;
  $("length").value = state.length;
  $("speed").value = state.speed;
  if (path) $("path").value = path;
  else if (!$("path").value || !$("path").value.includes(state.rig.character)) {
    const mod = state.rig.id === "opar" ? "moonmod.opar" : "moonmod.vigil";
    $("path").value = `mods/${mod}/resources/anims/${state.rig.character}_${name.replace(/[^a-z0-9_]+/gi, "_").toLowerCase()}.json`;
  }
  updateTravelNote();
  showFrame();
}

async function openNative(index) {
  const m = state.rig.motions[index];
  const data = await api(`/api/native?id=${state.rig.id}&motion=${index}`);
  setMotion(data, m.name || "motion_" + index, null);
  status(`Opened the native motion ${index}: ${m.name} (a key on every frame; "Thin out keys" leaves the ones that matter)`, "good");
}

async function openFile(path) {
  const data = await api(`/api/load?id=${state.rig.id}&path=${encodeURIComponent(path)}`);
  setMotion(data, path.split("/").pop().replace(/\.json$/, ""), path);
  status("Opened " + path, "good");
}

async function save() {
  const body = {
    id: state.rig.id, path: $("path").value.trim(), speed: Number($("speed").value), sync: $("sync").checked,
    keys: state.keys.map((k) => ({ frame: k.frame, ease: k.ease, values: k.values })),
  };
  if (state.keys[state.keys.length - 1].frame !== state.length) {
    // the motion ends at its length: hold the last pose there
    body.keys.push({ frame: state.length, ease: null, values: valuesAt(state.length) });
  }
  try {
    const r = await api("/api/save", body);
    $("saveNote").innerHTML = `Saved <b>${r.path}</b>: ${r.length} frames, ${(r.bytes / 1024).toFixed(0)} KB` +
      (r.synced.length ? `<br>and copied to ${r.synced.join(", ")}` : "");
    status("Saved " + r.path, "good");
    const info = await api("/api/rig?id=" + encodeURIComponent(state.rig.id));
    state.rig.files = info.files;
    fillSelect($("files"), info.files.map((f) => [f, f.replace(/^mods\//, "").replace("/resources/anims/", ": ")]));
    $("files").value = r.path;
    return r;
  } catch (e) {
    status("Not saved: " + e.message, "bad");
    throw e;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Selecting and posing

function select(i) {
  state.selected = i;
  for (const div of $("bones").children) div.classList.toggle("sel", Number(div.dataset.node) === i);
  const sel = $("bones").querySelector(".sel");
  sel?.scrollIntoView({ block: "nearest" });
  attachGizmo();
  updateSkeleton();
  refreshBonePanel();
}

function attachGizmo() {
  gizmo.detach();
  const i = state.selected;
  if (i < 0) return setTool("");
  const ch = state.byNode.get(i) || {};
  const o = state.nodes[i];
  if (state.mode === "rotate") {
    if (ch.rotation === undefined) return setTool("This point has no rotation of its own: use Limb (I) to pull it, or pick the bone above.");
    gizmo.setMode("rotate");
    gizmo.attach(o);
    setTool("Drag the rings to turn " + o.name + ".");
  } else if (state.mode === "move") {
    if (ch.translation === undefined) return setTool("The motion doesn't move this bone, only turns it. The body's centre usually moves (ctr, hip).");
    gizmo.setMode("translate");
    gizmo.attach(o);
    setTool("Drag the arrows to move " + o.name + ".");
  } else {
    const chain = limbChain(i);
    if (!chain) return setTool("Limb needs two animated bones above this one.");
    placeIkTarget();
    gizmo.setMode("translate");
    gizmo.attach(ikTarget);
    setTool(`Drag the handle: ${chain.hip.name} and ${chain.knee.name} bend so ${o.name} follows.`);
  }
}

function setTool(text) { $("toolNote").textContent = text; }

function setMode(mode) {
  state.mode = mode;
  $("modeRotate").classList.toggle("on", mode === "rotate");
  $("modeMove").classList.toggle("on", mode === "move");
  $("modeIK").classList.toggle("on", mode === "ik");
  attachGizmo();
}

function onGizmoChange() {
  if (state.playing) togglePlay(false);
  const i = state.selected;
  if (i < 0) return;
  if (state.mode === "ik") {
    const chain = limbChain(i);
    if (!chain) return;
    solveLimb(chain, ikTarget.getWorldPosition(new THREE.Vector3()));
    storeNode(nodeIndex(chain.hip));
    storeNode(nodeIndex(chain.knee));
  } else {
    storeNode(i);
  }
  state.redo = [];
  updateSkeleton();
  refreshBonePanel();
  drawTimeline();
}

// The two bones a limb bends with. Dark Cloud's chains start on a bone at the same point as the joint below them
// (Toan's chn18 and jnt18_1 are both the wrist), so bones on one spot count once; and a hand or foot point just past
// its own bone (r_hand on the wrist) moves with that bone, bent by the two above it.
function limbChain(i) {
  const end = state.nodes[i];
  state.model.updateMatrixWorld(true);
  const at = (o) => o.getWorldPosition(new THREE.Vector3());
  const joints = [];
  let last = at(end);
  for (let o = animatedAncestor(end); o && joints.length < 3; o = animatedAncestor(o)) {
    const p = at(o);
    if (p.distanceTo(last) < 1e-3) continue;
    joints.push({ o, p });
    last = p;
  }
  if (joints.length < 2) return null;
  const short = joints.length > 2 && at(end).distanceTo(joints[0].p) < 0.35 * joints[0].p.distanceTo(joints[1].p);
  const [knee, hip] = short ? [joints[1].o, joints[2].o] : [joints[0].o, joints[1].o];
  return { hip, knee, end };
}

function placeIkTarget() {
  const o = state.nodes[state.selected];
  if (!o) return;
  state.model.updateMatrixWorld(true);
  o.getWorldPosition(ikTarget.position);
  ikTarget.updateMatrixWorld();
}

// Turns `bone` (in world space) so the line from it to `tip` points along `dir`.
function aimBone(bone, tip, dir) {
  const b = bone.getWorldPosition(new THREE.Vector3());
  const cur = tip.getWorldPosition(new THREE.Vector3()).sub(b);
  if (cur.lengthSq() < 1e-10 || dir.lengthSq() < 1e-10) return;
  const turn = new THREE.Quaternion().setFromUnitVectors(cur.normalize(), dir.clone().normalize());
  const world = bone.getWorldQuaternion(new THREE.Quaternion());
  const parent = bone.parent.getWorldQuaternion(new THREE.Quaternion());
  bone.quaternion.copy(parent.invert().multiply(turn.multiply(world))).normalize();
  bone.updateMatrixWorld(true);
}

// Two-bone IK (moonanim.leg): the knee stays in the plane it bends in now.
function solveLimb({ hip, knee, end }, target) {
  state.model.updateMatrixWorld(true);
  const h = hip.getWorldPosition(new THREE.Vector3());
  const k = knee.getWorldPosition(new THREE.Vector3());
  const a = end.getWorldPosition(new THREE.Vector3());
  const l1 = k.distanceTo(h), l2 = a.distanceTo(k);
  const to = target.clone().sub(h);
  const d = Math.max(Math.abs(l1 - l2) * 1.001 + 1e-4, Math.min(to.length(), (l1 + l2) * 0.999));
  const dir = to.normalize();
  const x = (l1 * l1 - l2 * l2 + d * d) / (2 * d);
  const y = Math.sqrt(Math.max(0, l1 * l1 - x * x));
  let bend = k.clone().sub(h);
  bend.sub(dir.clone().multiplyScalar(bend.dot(dir)));
  if (bend.lengthSq() < 1e-8) bend = new THREE.Vector3(0, 0, 1).sub(dir.clone().multiplyScalar(dir.z));
  bend.normalize();
  const kneeAt = h.clone().add(dir.clone().multiplyScalar(x)).add(bend.multiplyScalar(y));
  aimBone(hip, knee, kneeAt.clone().sub(h));
  aimBone(knee, end, h.clone().add(dir.clone().multiplyScalar(d)).sub(kneeAt));
}

const euler = new THREE.Euler();
function refreshBonePanel() {
  const i = state.selected;
  const o = state.nodes[i];
  const ch = (i >= 0 && state.byNode.get(i)) || {};
  $("boneName").textContent = o ? `${o.name || "node " + i}  (node ${i})` + (state.byNode.has(i) ? "" : " · not animated") : "Click a dot on the model, or pick one below.";
  const rotOn = !!o && ch.rotation !== undefined;
  const posOn = !!o && ch.translation !== undefined;
  for (const id of ["rx", "ry", "rz"]) $(id).disabled = !rotOn;
  for (const id of ["px", "py", "pz"]) $(id).disabled = !posOn;
  if (rotOn && document.activeElement?.parentElement?.id !== "rotBox") {
    euler.setFromQuaternion(o.quaternion, "XYZ");
    $("rx").value = THREE.MathUtils.radToDeg(euler.x).toFixed(1);
    $("ry").value = THREE.MathUtils.radToDeg(euler.y).toFixed(1);
    $("rz").value = THREE.MathUtils.radToDeg(euler.z).toFixed(1);
  }
  if (posOn && document.activeElement?.parentElement?.id !== "posBox") {
    $("px").value = o.position.x.toFixed(2); $("py").value = o.position.y.toFixed(2); $("pz").value = o.position.z.toFixed(2);
  }
}

function typedBone() {
  const i = state.selected;
  const o = state.nodes[i];
  const ch = state.byNode.get(i) || {};
  if (!o) return;
  pushUndo();
  if (ch.rotation !== undefined) {
    const r = ["rx", "ry", "rz"].map((id) => THREE.MathUtils.degToRad(Number($(id).value) || 0));
    o.quaternion.setFromEuler(new THREE.Euler(r[0], r[1], r[2], "XYZ"));
  }
  if (ch.translation !== undefined) o.position.set(Number($("px").value), Number($("py").value), Number($("pz").value));
  storeNode(i);
  showFrame();
}

// ---------------------------------------------------------------------------------------------------------------
// Keys

function pushUndo() {
  state.undo.push(JSON.stringify({ keys: state.keys, length: state.length }));
  if (state.undo.length > 200) state.undo.shift();
  state.redo = [];
}

function restore(from, to) {
  if (!from.length) return;
  to.push(JSON.stringify({ keys: state.keys, length: state.length }));
  const s = JSON.parse(from.pop());
  state.keys = s.keys;
  state.length = s.length;
  $("length").value = state.length;
  state.selectedKey = Math.min(state.selectedKey, state.keys.length - 1);
  showFrame();
}

function keyPose() {
  pushUndo();
  const f = Math.round(state.frame);
  const values = currentValues();
  const i = keyIndexAt(f);
  if (i >= 0) state.keys[i].values = values;
  else { state.keys.push({ frame: f, ease: null, values }); state.keys.sort((a, b) => a.frame - b.frame); }
  state.selectedKey = keyIndexAt(f);
  state.frame = f;
  showFrame();
}

function deleteKey() {
  const i = state.selectedKey;
  if (i <= 0 || i >= state.keys.length) return status("Pick a key after frame 0 to delete (frame 0 always has one).", "bad");
  pushUndo();
  state.keys.splice(i, 1);
  state.selectedKey = Math.min(i, state.keys.length - 1);
  showFrame();
}

// Removes every key the blend between its neighbours already reaches (within 1.5 degrees, 0.05 units).
function thinKeys() {
  if (state.keys.length < 3) return;
  pushUndo();
  const original = [];
  for (let f = 0; f <= state.length; f++) original[f] = valuesAt(f);
  const before = state.keys.length;
  const close = (a, b) => state.channels.every((c, i) => {
    if (c.kind === "rotation") {
      qa.set(...a[i]); qb.set(...b[i]);
      return qa.angleTo(qb) < THREE.MathUtils.degToRad(1.5);
    }
    return a[i].every((x, k) => Math.abs(x - b[i][k]) < 0.05);
  });
  let changed = true;
  while (changed) {
    changed = false;
    for (let k = 1; k < state.keys.length - 1; k++) {
      const a = state.keys[k - 1], b = state.keys[k + 1];
      let ok = true;
      for (let f = a.frame + 1; f < b.frame && ok; f++) {
        const t = ease((f - a.frame) / (b.frame - a.frame), b.ease);
        ok = close(blendValues(a.values, b.values, t), original[f]);
      }
      if (ok) { state.keys.splice(k, 1); changed = true; k--; }
    }
  }
  state.selectedKey = 0;
  status(`Kept ${state.keys.length} of ${before} keys.`, "good");
  showFrame();
}

async function copyNativePose() {
  const list = state.rig.motions.map((m) => `${m.index}: ${m.name}`).join("\n");
  const pick = prompt("Copy a pose from which native motion? Type its number, and a frame after a comma (\"1, 8\").\n\n" + list.slice(0, 1800), "0, 0");
  if (!pick) return;
  const [m, f] = pick.split(",").map((s) => Number(s.trim()));
  const data = await api(`/api/native?id=${state.rig.id}&motion=${m}`);
  const frame = data.frames[Math.max(0, Math.min(data.frames.length - 1, f || 0))];
  state.clipboard = frame.values;
  status(`Copied native motion ${m}, frame ${frame.frame}. Ctrl+V pastes it as a key here.`, "good");
}

function pastePose() {
  if (!state.clipboard) return;
  pushUndo();
  const key = keyHere();
  key.values = state.clipboard.map((v) => v.slice());
  showFrame();
}

// ---------------------------------------------------------------------------------------------------------------
// Travel preview: the engine's lunge, power falling by `decay` every tick (StartPlayerMove)

function travelAt(ticks) {
  const lunge = Number($("lunge").value), decay = Number($("decay").value);
  let dist = 0, power = lunge;
  for (let t = 0; t < Math.floor(ticks) && power > 0; t++) { dist += power; power = Math.max(0, power - decay); }
  return dist;
}

function placeTravel() {
  const on = $("travel").checked;
  const d = on ? travelAt(state.frame / state.speed) : 0;
  stage.position.set(0, 0, $("backward").checked ? -d : d);
}

function updateTravelNote() {
  const lunge = Number($("lunge").value), decay = Number($("decay").value);
  const ticks = Math.ceil(lunge / decay);
  $("travelNote").textContent = `${travelAt(10000).toFixed(1)} units over ${ticks} ticks; the motion lasts ${(state.length / state.speed).toFixed(0)} ticks.`;
  $("speedNote").textContent = `${(state.length / state.speed).toFixed(0)} ticks`;
}

// ---------------------------------------------------------------------------------------------------------------
// Reference: an Elden Ring animation (tools/anim/reference) drawn as a stick figure beside the character, to watch
// how a similar action moves while posing this one. It is scaled to the character's height, turned to face +Z and
// either fitted over this motion's length or played in real time. It is only ever looked at; nothing of it is saved.

const REF_BODY = /^(Pelvis|Spine|Spine1|Spine2|Neck|Head|[LR]_(Clavicle|UpperArm|Forearm|Hand|Finger22|Thigh|Calf|Foot|Toe0))$/;
const ref = { available: false, groups: [], group: "", path: "", anim: null, links: [], head: -1, tall: 1, scale: 1, flipZ: 1,
  figure: new THREE.Group(), frame: 0 };
scene.add(ref.figure);
const refMats = { L: new THREE.MeshBasicMaterial({ color: 0x7fcf8a }), R: new THREE.MeshBasicMaterial({ color: 0xf07a6a }),
  C: new THREE.MeshBasicMaterial({ color: 0xd8d8d8 }) };
const refCyl = new THREE.CylinderGeometry(1, 1, 1, 8).translate(0, 0.5, 0);

async function loadReferenceGroups() {
  const r = await api("/api/ref/groups");
  ref.available = r.available;
  $("refPanel").style.display = r.available ? "" : "none";
  if (!r.available) return;
  ref.groups = r.groups;
  fillSelect($("refGroup"), r.groups.map((g) => [g.id, `${g.name}  (${g.source})`]));
  let want = "";
  try { want = localStorage.getItem("moonanim.refGroup") || ""; } catch (e) { /* private window */ }
  if (r.groups.some((g) => g.id === want)) $("refGroup").value = want;
  await loadReferenceItems($("refGroup").value);
}

async function loadReferenceItems(group) {
  const items = await api("/api/ref/items?group=" + encodeURIComponent(group));
  fillSelect($("refAnim"), items.map((p) => [p, p]));
  try {
    localStorage.setItem("moonanim.refGroup", group);
    const want = localStorage.getItem("moonanim.refAnim");
    if (items.includes(want)) $("refAnim").value = want;
  } catch (e) { /* private window */ }
  return items;
}

async function openReference(group, path) {
  status("Decoding " + path + "…");
  const anim = await api(`/api/ref/anim?group=${encodeURIComponent(group)}&path=${encodeURIComponent(path)}`);
  try { localStorage.setItem("moonanim.refAnim", path); } catch (e) { /* private window */ }
  ref.group = group;
  ref.path = path;
  ref.anim = anim;
  const at = (pose, i) => [pose.p[i * 3], pose.p[i * 3 + 1], pose.p[i * 3 + 2]];
  const index = (name) => anim.bones.indexOf(name);
  // The body's bones, each joined to the nearest body bone above it.
  const body = anim.bones.map((n) => REF_BODY.test(n));
  ref.links = [];
  anim.bones.forEach((n, i) => {
    if (!body[i]) return;
    let p = anim.parents[i];
    while (p >= 0 && !body[p]) p = anim.parents[p];
    if (p >= 0) ref.links.push({ from: p, to: i, side: n[0] === "L" && n[1] === "_" ? "L" : n[0] === "R" && n[1] === "_" ? "R" : "C" });
  });
  ref.head = index("Head");
  // Its toes point the way it faces; the editor's characters face +Z.
  const foot = at(anim.rest, index("L_Foot")), toe = at(anim.rest, index("L_Toe0"));
  ref.flipZ = toe[2] < foot[2] ? -1 : 1;
  // Its height standing (the head bone sits about a head's height below the top) against the character's.
  ref.tall = at(anim.rest, ref.head)[1] * 1.08;
  ref.figure.clear();
  ref.sticks = ref.links.map((l) => { const m = new THREE.Mesh(refCyl, refMats[l.side]); ref.figure.add(m); return m; });
  ref.skull = new THREE.Mesh(new THREE.SphereGeometry(1, 14, 10), refMats.C);
  ref.figure.add(ref.skull);
  if (!$("refBeside").value) $("refBeside").value = Math.round((state.rigHeight || 16) * 0.7);
  $("refFrom").value = 0;
  $("refTo").value = anim.frames - 1;
  $("refShow").checked = true;
  placeReference();
  status(`Reference: ${path}, ${anim.frames} frames (${anim.duration.toFixed(2)} s)`, "good");
  return referenceSummary();
}

// The reference's frame for a frame of this motion.
function referenceFrameAt(frame) {
  const a = ref.anim;
  const from = Math.max(0, Math.min(a.frames - 1, Number($("refFrom").value) || 0));
  const to = Math.max(from, Math.min(a.frames - 1, Number($("refTo").value) || a.frames - 1));
  if ($("refTiming").value === "real") return Math.min(to, from + (frame / state.speed / TICKS) / a.frame_time);
  return from + (to - from) * Math.max(0, Math.min(1, frame / Math.max(1, state.length)));
}

const refA = new THREE.Vector3(), refB = new THREE.Vector3(), refUp = new THREE.Vector3(0, 1, 0);
function placeReference() {
  ref.figure.visible = !!ref.anim && $("refShow").checked;
  if (!ref.figure.visible) return;
  const a = ref.anim;
  ref.frame = referenceFrameAt(state.frame);
  const f0 = Math.floor(ref.frame), f1 = Math.min(a.frames - 1, f0 + 1), t = ref.frame - f0;
  const s = ref.scale = (state.rigHeight || 16) / ref.tall;
  const p0 = a.poses[f0].p, p1 = a.poses[f1].p;
  const point = (i, out) => out.set(
    (p0[i * 3] + (p1[i * 3] - p0[i * 3]) * t) * s,
    (p0[i * 3 + 1] + (p1[i * 3 + 1] - p0[i * 3 + 1]) * t) * s,
    (p0[i * 3 + 2] + (p1[i * 3 + 2] - p0[i * 3 + 2]) * t) * s * ref.flipZ);
  const thick = (state.rigHeight || 16) / 110;
  ref.links.forEach((l, n) => {
    point(l.from, refA); point(l.to, refB);
    const stick = ref.sticks[n], d = refB.clone().sub(refA), len = d.length();
    stick.position.copy(refA);
    stick.quaternion.setFromUnitVectors(refUp, len > 1e-6 ? d.divideScalar(len) : refUp);
    stick.scale.set(thick, len, thick);
  });
  point(ref.head, refA);
  ref.skull.position.copy(refA).addScaledVector(refUp, thick * 4);
  ref.skull.scale.setScalar(thick * 4.5);
  // Beside the character as the camera sees him (to the right on screen), travelling with him or along its own root
  // motion.
  const look = camera.getWorldDirection(refA);
  const side = new THREE.Vector3(-look.z, 0, look.x);
  if (side.lengthSq() < 1e-6) side.set(1, 0, 0);
  side.normalize().multiplyScalar(Number($("refBeside").value || 0));
  let x = side.x, z = side.z + stage.position.z;
  if ($("refRoot").checked && a.root.length) {
    const r = a.root[Math.min(a.root.length - 1, Math.round(ref.frame))];
    x += r[0] * s;
    z += r[2] * s * ref.flipZ - stage.position.z;
  }
  ref.figure.position.set(x, 0, z);
}

function referenceSummary() {
  return { available: ref.available, group: ref.group, path: ref.path, shown: ref.figure.visible,
    frames: ref.anim ? ref.anim.frames : 0, seconds: ref.anim ? r3(ref.anim.duration) : 0,
    from: Number($("refFrom").value), to: Number($("refTo").value), timing: $("refTiming").value,
    beside: Number($("refBeside").value), own_travel: $("refRoot").checked, frame: r3(ref.frame) };
}

// ---------------------------------------------------------------------------------------------------------------
// Timeline

const tl = $("timeline");
const tlx = tl.getContext("2d");
const PAD = 14;
const fx = (f) => PAD + (f / state.length) * (tl.width - 2 * PAD);
const xf = (x) => ((x - PAD) / (tl.width - 2 * PAD)) * state.length;

function drawTimeline() {
  const r = tl.getBoundingClientRect();
  if (tl.width !== Math.round(r.width) || tl.height !== Math.round(r.height)) {
    tl.width = Math.round(r.width);
    tl.height = Math.round(r.height);
  }
  const w = tl.width, h = tl.height;
  tlx.clearRect(0, 0, w, h);
  tlx.fillStyle = "#1a1c20";
  tlx.fillRect(0, 0, w, h);
  const step = (tl.width - 2 * PAD) / state.length;
  const every = step >= 8 ? 1 : step >= 3 ? 5 : 10;
  tlx.font = "10px system-ui";
  for (let f = 0; f <= state.length; f++) {
    if (f % every) continue;
    const x = fx(f);
    const major = f % (every * 5) === 0;
    tlx.strokeStyle = major ? "#4a4e56" : "#33363c";
    tlx.beginPath(); tlx.moveTo(x + 0.5, major ? 0 : 6); tlx.lineTo(x + 0.5, major ? 16 : 12); tlx.stroke();
    if (major) { tlx.fillStyle = "#9a9ca3"; tlx.fillText(String(f), x + 3, 11); }
  }
  state.keys.forEach((k, i) => {
    const x = fx(k.frame), y = 36;
    tlx.fillStyle = i === state.selectedKey ? "#5fb3f9" : "#e0a63a";
    tlx.beginPath(); tlx.moveTo(x, y - 7); tlx.lineTo(x + 6, y); tlx.lineTo(x, y + 7); tlx.lineTo(x - 6, y); tlx.closePath(); tlx.fill();
    if (k.ease) { tlx.fillStyle = "#9a9ca3"; tlx.fillText(k.ease, x - 8, y + 18); }
  });
  const px = fx(state.frame);
  tlx.strokeStyle = "#f0f0f0";
  tlx.beginPath(); tlx.moveTo(px + 0.5, 0); tlx.lineTo(px + 0.5, h); tlx.stroke();
  const k = state.keys[state.selectedKey];
  $("ease").value = k?.ease || "";
  $("ease").disabled = !k || state.selectedKey === 0;
}

let tlDrag = null;
tl.addEventListener("pointerdown", (e) => {
  tl.setPointerCapture(e.pointerId);
  const x = e.offsetX * (tl.width / tl.clientWidth);
  const hit = state.keys.findIndex((k) => Math.abs(fx(k.frame) - x) < 7 && e.offsetY > 22);
  if (state.playing) togglePlay(false);
  if (hit >= 0) {
    state.selectedKey = hit;
    state.frame = state.keys[hit].frame;
    tlDrag = { key: state.keys[hit], moved: false };
  } else {
    tlDrag = { scrub: true };
    state.frame = Math.max(0, Math.min(state.length, Math.round(xf(x))));
  }
  showFrame();
});
tl.addEventListener("pointermove", (e) => {
  if (!tlDrag) return;
  const x = e.offsetX * (tl.width / tl.clientWidth);
  const f = Math.max(0, Math.min(state.length, Math.round(xf(x))));
  if (tlDrag.scrub) { state.frame = f; return showFrame(); }
  const key = tlDrag.key;
  if (key.frame === 0 || f === 0 || f === key.frame || keyIndexAt(f) >= 0) return;
  if (!tlDrag.moved) { pushUndo(); tlDrag.moved = true; }
  key.frame = f;
  state.keys.sort((a, b) => a.frame - b.frame);
  state.selectedKey = state.keys.indexOf(key);
  state.frame = f;
  showFrame();
});
tl.addEventListener("pointerup", () => { tlDrag = null; });

// ---------------------------------------------------------------------------------------------------------------
// Playback

let last = 0;
function togglePlay(on) {
  state.playing = on === undefined ? !state.playing : on;
  $("play").textContent = state.playing ? "Pause" : "Play";
  if (state.playing && state.frame >= state.length) state.frame = 0;
  last = performance.now();
}

function tick(now) {
  if (state.playing) {
    const dt = Math.min(0.1, (now - last) / 1000);
    state.frame += dt * TICKS * state.speed * Number($("rate").value);
    if (state.frame >= state.length) {
      if ($("loop").classList.contains("on")) state.frame = 0;
      else { state.frame = state.length; togglePlay(false); }
    }
    showFrame();
  }
  last = now;
  if (!state.playing && ref.anim) placeReference();
  renderer.render(scene, camera);
  requestAnimationFrame(tick);
}

// ---------------------------------------------------------------------------------------------------------------
// Picking dots in the view

const ray = new THREE.Raycaster();
let downAt = null;
renderer.domElement.addEventListener("pointerdown", (e) => { downAt = [e.clientX, e.clientY]; });
renderer.domElement.addEventListener("pointerup", (e) => {
  if (!downAt || Math.hypot(e.clientX - downAt[0], e.clientY - downAt[1]) > 4 || gizmo.dragging) return;
  const r = renderer.domElement.getBoundingClientRect();
  ray.setFromCamera(new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1), camera);
  const hits = ray.intersectObjects(dots.map((d) => d.mesh), false);
  if (hits.length) select(hits[0].object.userData.node);
});

// ---------------------------------------------------------------------------------------------------------------
// Controls

$("rig").onchange = () => openRig($("rig").value).catch((e) => status(e.message, "bad"));
$("openNative").onclick = () => openNative(Number($("native").value)).catch((e) => status(e.message, "bad"));
$("openFile").onclick = () => $("files").value && openFile($("files").value).catch((e) => status(e.message, "bad"));
$("newMotion").onclick = () => {
  const values = currentValues();
  setMotion({ frames: [{ frame: 0, values }, { frame: 30, values: values.map((v) => v.slice()) }], length: 30, speed: 0.5 }, "new_move", null);
  status("A new motion: one pose held for 30 frames. Pose it and key it.", "good");
};
$("save").onclick = () => save().catch(() => {});
$("play").onclick = () => togglePlay();
$("loop").onclick = () => $("loop").classList.toggle("on");
$("prev").onclick = () => { togglePlay(false); state.frame = Math.max(0, Math.round(state.frame) - 1); showFrame(); };
$("next").onclick = () => { togglePlay(false); state.frame = Math.min(state.length, Math.round(state.frame) + 1); showFrame(); };
$("frame").onchange = () => { state.frame = Math.max(0, Math.min(state.length, Number($("frame").value) || 0)); showFrame(); };
$("length").onchange = () => {
  const n = Math.max(1, Math.round(Number($("length").value) || 1));
  const lastKey = state.keys[state.keys.length - 1].frame;
  if (n < lastKey) { $("length").value = state.length; return status(`A key sits at frame ${lastKey}: move or delete it first.`, "bad"); }
  pushUndo();
  state.length = n;
  state.frame = Math.min(state.frame, n);
  updateTravelNote();
  showFrame();
};
$("speed").onchange = () => { state.speed = Math.max(0.05, Math.min(4, Number($("speed").value) || 0.5)); updateTravelNote(); showFrame(); };
for (const id of ["lunge", "decay", "travel", "backward"]) $(id).onchange = () => { updateTravelNote(); showFrame(); };
$("refGroup").onchange = () => loadReferenceItems($("refGroup").value).catch((e) => status(e.message, "bad"));
$("refOpen").onclick = () => $("refAnim").value && openReference($("refGroup").value, $("refAnim").value).catch((e) => status(e.message, "bad"));
for (const id of ["refShow", "refRoot", "refBeside", "refTiming", "refFrom", "refTo"]) $(id).onchange = showFrame;
$("keyBtn").onclick = keyPose;
$("delKey").onclick = deleteKey;
$("ease").onchange = () => {
  const k = state.keys[state.selectedKey];
  if (!k || state.selectedKey === 0) return;
  pushUndo();
  k.ease = $("ease").value || null;
  showFrame();
};
$("simplify").onclick = thinKeys;
$("nativePose").onclick = () => copyNativePose().catch((e) => status(e.message, "bad"));
$("modeRotate").onclick = () => setMode("rotate");
$("modeMove").onclick = () => setMode("move");
$("modeIK").onclick = () => setMode("ik");
$("space").onclick = () => {
  gizmo.setSpace(gizmo.space === "local" ? "world" : "local");
  $("space").innerHTML = `Gizmo: ${gizmo.space} <kbd>Q</kbd>`;
};
$("showSkel").onchange = updateSkeleton;
for (const id of ["rx", "ry", "rz", "px", "py", "pz"]) $(id).onchange = typedBone;
$("resetBone").onclick = () => {
  const i = state.selected;
  if (i < 0) return;
  const prev = [...state.keys].reverse().find((k) => k.frame < Math.round(state.frame));
  if (!prev) return;
  pushUndo();
  const key = keyHere();
  const ch = state.byNode.get(i) || {};
  for (const kind of ["rotation", "translation", "scale"]) if (ch[kind] !== undefined) key.values[ch[kind]] = prev.values[ch[kind]].slice();
  showFrame();
};

window.addEventListener("keydown", (e) => {
  if (e.target.tagName === "INPUT" || e.target.tagName === "SELECT") {
    if (e.key === "Enter") e.target.blur();
    return;
  }
  const ctrl = e.ctrlKey || e.metaKey;
  if (ctrl && e.key.toLowerCase() === "s") { e.preventDefault(); return save().catch(() => {}); }
  if (ctrl && e.key.toLowerCase() === "z") { e.preventDefault(); return restore(state.undo, state.redo); }
  if (ctrl && e.key.toLowerCase() === "y") { e.preventDefault(); return restore(state.redo, state.undo); }
  if (ctrl && e.key.toLowerCase() === "c") { state.clipboard = currentValues(); return status("Pose copied.", "good"); }
  if (ctrl && e.key.toLowerCase() === "v") return pastePose();
  if (ctrl) return;
  switch (e.key) {
    case " ": e.preventDefault(); togglePlay(); break;
    case "ArrowLeft": $("prev").click(); break;
    case "ArrowRight": $("next").click(); break;
    case "Home": togglePlay(false); state.frame = 0; showFrame(); break;
    case "End": togglePlay(false); state.frame = state.length; showFrame(); break;
    case "k": case "K": keyPose(); break;
    case "Delete": deleteKey(); break;
    case "r": case "R": setMode("rotate"); break;
    case "t": case "T": setMode("move"); break;
    case "i": case "I": setMode("ik"); break;
    case "q": case "Q": $("space").click(); break;
  }
});

// ---------------------------------------------------------------------------------------------------------------
// Bridge: mcp_server.py's commands, through the server (/api/bridge). An assistant poses, keys, saves and looks at
// this page while you watch it; every change it makes is one undo step. Bones are named as in the bone list (a name
// the rig repeats gets "#node"); rotations are the bone panel's degrees (local, XYZ order); positions are in the
// model's space (the travel preview's shift taken out), y up and the character facing +z.

const r3 = (x) => Math.round(x * 1000) / 1000;
const deg = (x) => Math.round(THREE.MathUtils.radToDeg(x) * 10) / 10;
const EASE_NAMES = [null, "", "in", "out", "inout"];

function boneLabels() {
  const count = {};
  state.nodes.forEach((o, i) => { if (pickable(i)) count[o.name] = (count[o.name] || 0) + 1; });
  const labels = new Map();
  state.nodes.forEach((o, i) => {
    if (!pickable(i)) return;
    const name = o.name || "node " + i;
    labels.set(i, count[o.name] > 1 ? `${name}#${i}` : name);
  });
  return labels;
}

function findBone(ref, labels) {
  if (typeof ref === "number" && labels.has(ref)) return ref;
  const text = String(ref);
  for (const [i, label] of labels) if (label === text) return i;
  const lower = [...labels].filter(([, label]) => label.toLowerCase() === text.toLowerCase());
  if (lower.length === 1) return lower[0][0];
  throw new Error(`no bone "${ref}" on ${state.rig.id}; its bones: ${[...labels.values()].join(", ")}`);
}

function modelPosition(o) {
  state.model.updateMatrixWorld(true);
  return stage.worldToLocal(o.getWorldPosition(new THREE.Vector3()));
}

function vec3(v, what) {
  if (!Array.isArray(v) || v.length !== 3 || v.some((x) => typeof x !== "number" || !isFinite(x))) throw new Error(what + " takes [x, y, z]");
  return v;
}

function goTo(frame) {
  togglePlay(false);
  const f = frame === undefined || frame === null ? Math.round(state.frame) : Math.round(frame);
  if (!(f >= 0 && f <= state.length)) throw new Error(`frame ${frame} is outside the motion (0 to ${state.length})`);
  state.frame = f;
  showFrame();
  return f;
}

function needRig() { if (!state.rig || !state.keys.length) throw new Error("the page is still loading"); }

function summary() {
  return {
    page: PAGE, // a new id each time the page loads: a change means everything unsaved was lost
    rig: state.rig && { id: state.rig.id, name: state.rig.name, character: state.rig.character, playable: state.rig.playable },
    motion: state.sourceName, path: $("path").value, length: state.length, speed: state.speed,
    ticks: Math.round(state.length / state.speed), seconds: r3(state.length / state.speed / TICKS), frame: state.frame,
    keys: state.keys.map((k) => (k.ease ? { frame: k.frame, ease: k.ease } : { frame: k.frame })),
    undo_steps: state.undo.length,
    travel: { show: $("travel").checked, lunge: Number($("lunge").value), decay: Number($("decay").value),
      backward: $("backward").checked, distance: r3(travelAt(state.length / state.speed)) },
  };
}

function poseOf(i, label) {
  const o = state.nodes[i];
  const ch = state.byNode.get(i) || {};
  const out = { bone: label };
  if (ch.rotation !== undefined) {
    euler.setFromQuaternion(o.quaternion, "XYZ");
    out.rotation = [deg(euler.x), deg(euler.y), deg(euler.z)];
  }
  if (ch.translation !== undefined) out.translation = [o.position.x, o.position.y, o.position.z].map(r3);
  if (ch.scale !== undefined) out.scale = [o.scale.x, o.scale.y, o.scale.z].map(r3);
  if (!state.byNode.has(i)) out.end = true; // a hand or foot: not animated itself, Limb reaches with it
  out.position = modelPosition(o).toArray().map(r3);
  return out;
}

const BRIDGE = {
  status: async () => summary(),

  open: async ({ rig, native, file, new: fresh, name, length }) => {
    if (rig && rig !== state.rig?.id) {
      if (![...$("rig").options].some((o) => o.value === rig)) throw new Error(`no rig "${rig}" (list_rigs names them)`);
      $("rig").value = rig;
      await openRig(rig);
    }
    needRig();
    if (native !== undefined && native !== null) {
      if (!state.rig.motions[native]) throw new Error(`${state.rig.id} has native motions 0 to ${state.rig.motions.length - 1}`);
      $("native").value = native;
      await openNative(Number(native));
    } else if (file) {
      await openFile(file);
      $("files").value = file;
    } else if (fresh) {
      if (name) $("path").value = ""; // the default path follows the name
      const n = Math.max(1, Math.round(length || 30));
      const values = currentValues();
      setMotion({ frames: [{ frame: 0, values }, { frame: n, values: values.map((v) => v.slice()) }], length: n, speed: 0.5 }, name || "new_move", null);
    }
    frameCamera();
    status("Assistant opened " + state.sourceName, "good");
    return summary();
  },

  get_pose: async ({ frame, bones }) => {
    needRig();
    const f = goTo(frame);
    const labels = boneLabels();
    const which = bones?.length ? bones.map((b) => findBone(b, labels)) : [...labels.keys()];
    return { frame: f, key: keyIndexAt(f) >= 0, bones: which.map((i) => poseOf(i, labels.get(i))) };
  },

  set_pose: async ({ frame, from_native, bones = {}, reach = {}, ease }) => {
    needRig();
    const labels = boneLabels();
    // check everything before changing anything
    const sets = Object.entries(bones).map(([ref, v]) => {
      const i = findBone(ref, labels);
      const ch = state.byNode.get(i) || {};
      for (const k of Object.keys(v)) if (!["rotation", "turn", "translation", "move"].includes(k)) throw new Error(`${labels.get(i)}: "${k}" isn't rotation, turn, translation or move`);
      if ((v.rotation || v.turn) && ch.rotation === undefined) throw new Error(`${labels.get(i)} has no rotation of its own; reach with it instead`);
      if ((v.translation || v.move) && ch.translation === undefined) throw new Error(`the motion doesn't move ${labels.get(i)}, only turns it`);
      for (const k of ["rotation", "turn", "translation", "move"]) if (v[k]) vec3(v[k], `${labels.get(i)}.${k}`);
      return [i, v];
    });
    const reaches = Object.entries(reach).map(([ref, at]) => {
      const i = findBone(ref, labels);
      const chain = limbChain(i);
      if (!chain) throw new Error(`${labels.get(i)} needs two animated bones above it to reach`);
      return [i, chain, new THREE.Vector3(...vec3(at, `reach.${labels.get(i)}`))];
    });
    if (ease !== undefined && !EASE_NAMES.includes(ease)) throw new Error('ease is null, "in", "out" or "inout"');
    let base = null;
    if (from_native) {
      const data = await api(`/api/native?id=${state.rig.id}&motion=${from_native.motion}`);
      const fr = data.frames[from_native.frame || 0];
      if (!fr) throw new Error(`native motion ${from_native.motion} has frames 0 to ${data.frames.length - 1}`);
      base = fr.values;
    }
    const f = goTo(frame);
    pushUndo();
    const key = keyHere();
    if (base) { key.values = base.map((v) => v.slice()); showFrame(); }
    if (ease !== undefined && f > 0) key.ease = ease || null;
    const rad = (d) => d.map((x) => THREE.MathUtils.degToRad(x));
    for (const [i, v] of sets) {
      const o = state.nodes[i];
      if (v.rotation) o.quaternion.setFromEuler(new THREE.Euler(...rad(v.rotation), "XYZ"));
      if (v.turn) o.quaternion.multiply(new THREE.Quaternion().setFromEuler(new THREE.Euler(...rad(v.turn), "XYZ")));
      if (v.translation) o.position.set(...v.translation);
      if (v.move) o.position.add(new THREE.Vector3(...v.move));
      storeNode(i);
    }
    const reached = [];
    for (const [i, chain, at] of reaches) {
      solveLimb(chain, stage.localToWorld(at.clone()));
      storeNode(nodeIndex(chain.hip));
      storeNode(nodeIndex(chain.knee));
      const now = modelPosition(chain.end);
      reached.push({ bone: labels.get(i), at: now.toArray().map(r3), off_by: r3(now.distanceTo(at)),
        bent: [labels.get(nodeIndex(chain.hip)), labels.get(nodeIndex(chain.knee))] });
    }
    showFrame();
    const touched = new Set([...sets.map(([i]) => i), ...reaches.flatMap(([, c]) => [nodeIndex(c.hip), nodeIndex(c.knee), nodeIndex(c.end)])]);
    status(`Assistant keyed frame ${f}`, "good");
    return { frame: f, keys: state.keys.length, bones: [...touched].map((i) => poseOf(i, labels.get(i))), reached };
  },

  edit: async ({ undo, redo, delete: del, move, ease, length, speed, thin, travel }) => {
    needRig();
    togglePlay(false);
    const notes = [];
    for (let n = 0; n < (undo || 0); n++) restore(state.undo, state.redo);
    for (let n = 0; n < (redo || 0); n++) restore(state.redo, state.undo);
    if (del?.length || move?.length || Object.keys(ease || {}).length || length) pushUndo();
    for (const f of del || []) {
      const i = keyIndexAt(f);
      if (i <= 0) { notes.push(i === 0 ? "frame 0 always has a key" : `no key at ${f}`); continue; }
      state.keys.splice(i, 1);
    }
    for (const [from, to] of move || []) {
      const i = keyIndexAt(from);
      if (i <= 0 || !(to > 0 && to <= state.length) || keyIndexAt(Math.round(to)) >= 0) { notes.push(`couldn't move the key at ${from} to ${to}`); continue; }
      state.keys[i].frame = Math.round(to);
      state.keys.sort((a, b) => a.frame - b.frame);
    }
    for (const [f, e] of Object.entries(ease || {})) {
      const i = keyIndexAt(Number(f));
      if (i <= 0 || !EASE_NAMES.includes(e)) { notes.push(`no ease set at ${f}`); continue; }
      state.keys[i].ease = e || null;
    }
    if (length) {
      const n = Math.max(1, Math.round(length));
      const lastKey = state.keys[state.keys.length - 1].frame;
      if (n < lastKey) notes.push(`a key sits at ${lastKey}, so the length stays ${state.length}`);
      else state.length = n;
      $("length").value = state.length;
    }
    if (speed) { state.speed = Math.max(0.05, Math.min(4, speed)); $("speed").value = state.speed; }
    if (thin) thinKeys();
    if (travel) {
      if (travel.show !== undefined) $("travel").checked = !!travel.show;
      if (travel.lunge !== undefined) $("lunge").value = travel.lunge;
      if (travel.decay !== undefined) $("decay").value = travel.decay;
      if (travel.backward !== undefined) $("backward").checked = !!travel.backward;
    }
    state.selectedKey = Math.max(0, Math.min(state.selectedKey, state.keys.length - 1));
    state.frame = Math.min(state.frame, state.length);
    updateTravelNote();
    showFrame();
    return { ...summary(), notes };
  },

  save: async ({ path, sync }) => {
    needRig();
    if (path) $("path").value = path;
    if (sync !== undefined) $("sync").checked = !!sync;
    return await save();
  },

  // The Elden Ring reference beside the character: list its sources and animations, show one, and set how it's placed
  // and timed. `from`/`to` pick the part of it that plays over this motion.
  reference: async ({ groups, group, list, open, show, own_travel, beside, timing, from, to }) => {
    needRig();
    if (!ref.available) throw new Error("no reference cache: run tools/anim/reference (see its README)");
    const out = {};
    if (groups) out.groups = ref.groups;
    if (group !== undefined && group !== $("refGroup").value) {
      if (!ref.groups.some((g) => g.id === group)) throw new Error("no reference group " + group);
      $("refGroup").value = group;
      out.items = await loadReferenceItems(group);
    }
    if (list) out.items = out.items || await api("/api/ref/items?group=" + encodeURIComponent($("refGroup").value));
    if (open) {
      $("refAnim").value = open;
      await openReference($("refGroup").value, open);
    }
    if (show !== undefined) $("refShow").checked = !!show;
    if (own_travel !== undefined) $("refRoot").checked = !!own_travel;
    if (beside !== undefined) $("refBeside").value = Number(beside);
    if (timing !== undefined) {
      if (!["stretch", "real"].includes(timing)) throw new Error("timing is stretch or real");
      $("refTiming").value = timing;
    }
    if (from !== undefined) $("refFrom").value = Math.round(from);
    if (to !== undefined) $("refTo").value = Math.round(to);
    showFrame();
    return { ...out, ...referenceSummary() };
  },

  // Renders frames from a camera of its own (or yours, view "current") and gives them back as one PNG, tiled.
  screenshot: async ({ frame, frames, view = "three-quarter", width, height, bones = false, columns }) => {
    needRig();
    togglePlay(false);
    const list = (frames?.length ? frames : [frame ?? Math.round(state.frame)]).map((f) => Math.max(0, Math.min(state.length, Math.round(f))));
    const many = list.length > 1;
    const w = Math.round(Math.max(96, Math.min(1600, width || (many ? 320 : 720))));
    const h = Math.round(Math.max(96, Math.min(1600, height || (many ? 360 : 640))));
    const cols = Math.max(1, Math.min(list.length, columns || 6));
    const rows = Math.ceil(list.length / cols);
    const dirs = { front: [0, 0.15, 1], back: [0, 0.15, -1], left: [1, 0.15, 0], right: [-1, 0.15, 0], top: [0, 1, 0.001],
      "three-quarter": [0.6, 0.3, 0.75] };
    if (view !== "current" && !dirs[view]) throw new Error(`view is one of current, ${Object.keys(dirs).join(", ")}`);
    const keep = { frame: state.frame, pos: camera.position.clone(), quat: camera.quaternion.clone(), target: orbit.target.clone(),
      near: camera.near, far: camera.far, skel: $("showSkel").checked, gizmo: gizmo.visible, ratio: renderer.getPixelRatio() };
    const sheet = document.createElement("canvas");
    sheet.width = w * cols; sheet.height = h * rows;
    const ctx = sheet.getContext("2d");
    ctx.fillStyle = "#1a1c20";
    ctx.fillRect(0, 0, sheet.width, sheet.height);
    try {
      $("showSkel").checked = !!bones;
      gizmo.visible = false;
      if (view !== "current") {
        camera.position.copy(orbit.target).add(new THREE.Vector3(...dirs[view]));
        camera.lookAt(orbit.target);
        const box = new THREE.Box3();
        for (const f of list) {
          state.frame = f;
          showFrame();
          posedBox(box, true);
          if (ref.figure.visible) box.expandByObject(ref.figure);
        }
        const center = box.getCenter(new THREE.Vector3());
        const radius = Math.max(2, box.getSize(new THREE.Vector3()).length() / 2);
        const half = THREE.MathUtils.degToRad(camera.fov / 2);
        const fit = Math.min(half, Math.atan(Math.tan(half) * w / h));
        camera.position.copy(center).add(new THREE.Vector3(...dirs[view]).normalize().multiplyScalar(radius / Math.sin(fit) * 1.05));
        camera.near = radius / 100; camera.far = radius * 100;
        camera.lookAt(center);
      }
      renderer.setPixelRatio(1);
      renderer.setSize(w, h, false);
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
      ctx.font = "bold 15px system-ui";
      list.forEach((f, n) => {
        state.frame = f;
        showFrame();
        renderer.render(scene, camera);
        const x = (n % cols) * w, y = Math.floor(n / cols) * h;
        ctx.drawImage(renderer.domElement, x, y);
        if (many) {
          ctx.fillStyle = keyIndexAt(f) >= 0 ? "#e0a63a" : "#f0f0f0";
          ctx.fillText(String(f), x + 8, y + 20);
        }
      });
    } finally {
      $("showSkel").checked = keep.skel;
      gizmo.visible = keep.gizmo;
      camera.position.copy(keep.pos); camera.quaternion.copy(keep.quat); orbit.target.copy(keep.target);
      camera.near = keep.near; camera.far = keep.far;
      renderer.setPixelRatio(keep.ratio);
      resize();
      state.frame = keep.frame;
      showFrame();
    }
    return { png: sheet.toDataURL("image/png").split(",")[1], frames: list, view, width: sheet.width, height: sheet.height };
  },
};

// This load of the page. A reload or a closed tab says goodbye, and the server fails whatever it was doing.
const PAGE = Math.random().toString(36).slice(2);

async function bridgeLoop() {
  for (;;) {
    let cmd;
    try {
      const res = await fetch("/api/bridge/next?page=" + PAGE);
      if (res.status === 404) return; // a server from before the bridge
      if (res.status !== 200) continue;
      cmd = await res.json();
    } catch (e) {
      await new Promise((r) => setTimeout(r, 3000)); // the server is down or restarting
      continue;
    }
    let reply;
    try {
      if (!BRIDGE[cmd.cmd]) throw new Error("unknown command " + cmd.cmd);
      reply = { id: cmd.id, ok: true, result: await BRIDGE[cmd.cmd](cmd.args || {}) };
    } catch (e) {
      reply = { id: cmd.id, ok: false, error: e.message };
      status("Assistant: " + e.message, "bad");
    }
    await fetch("/api/bridge/result", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(reply) }).catch(() => {});
  }
}

setMode("rotate");
resize();
requestAnimationFrame(tick);
loadRigList().catch((e) => status("Couldn't load: " + e.message, "bad"))
  .then(() => loadReferenceGroups().catch((e) => status("Reference: " + e.message, "bad")));

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
    fillSelect($("files"), state.rig.files.map((f) => [f, f.replace(/^mods\//, "").replace("/resources/anims/", ": ")]));
    $("files").value = r.path;
    await openFile(r.path);
  } catch (err) {
    status(err.message, "bad");
  }
};
// for inspecting from the console
window.moonanimEditor = { state, valuesAt, applyValues, select, setMode, ikTarget, onGizmoChange, showFrame, openRig, openNative, BRIDGE };
