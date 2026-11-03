// The page around the game (docs/WEB.md). It makes the page cross-origin isolated (sw.js), keeps the player's
// game data in the origin private file system under chronicle/, and starts the game with that directory
// mounted at /opfs (port/web/src/storage.cpp): darkcloud.js, drawing with WebGPU, or darkcloud_gl.js, drawing
// with WebGL2 (chooseRenderer). Extra arguments for the game can be given as ?args=--headless+--frames+30, and
// the renderer as ?renderer=gl or ?renderer=webgpu.
//
// What the player chooses is stored as one of:
// - chronicle/data.zip, the archive dcdata packs;
// - chronicle/data/, an extracted data folder;
// - chronicle/disc.iso or chronicle/disc/ (DATA.DAT and DATA.HD2), which the game reads in place; at its first
//   start it writes the few files dcdata adds beside a disc's into chronicle/data/ (port/web/src/firstrun.cpp).
// Storing one removes the others.

'use strict';

const $ = (id) => document.getElementById(id);
const ISOLATE_FLAG = 'dc-isolate-reload';
// A disc comes first: chronicle/data/ beside it holds only what the game prepared.
const KINDS = ['disc.iso', 'disc', 'data.zip', 'data'];

function setStatus(text) {
    $('status').textContent = text;
}

// The run's last lines, kept in the browser's storage as they come, so a tab the browser kills (Safari reloads
// one that crashed or ran out of memory) leaves them to read at the next visit, as "Previous run's log".
const LOG_KEY = 'dc-log';
const LOG_LINES = 400;
const kept = [];
let keepTimer = 0;

function keepLog(now) {
    clearTimeout(keepTimer);
    keepTimer = 0;
    const save = () => {
        try {
            localStorage.setItem(LOG_KEY, kept.join('\n'));
        } catch {
            // Storage full or refused: the log is a help, not a need.
        }
    };
    if (now) {
        save();
    } else {
        keepTimer = setTimeout(save, 250);
    }
}

function log(text) {
    if (window.dcLogging) {
        kept.push(text);
        if (kept.length > LOG_LINES) {
            kept.splice(0, kept.length - LOG_LINES);
        }
        // What precedes a crash is written at once; the rest a little later, in one go.
        keepLog(/error|abort|fail|nan|exception|lost/i.test(text));
    }
    const pre = $('log');
    pre.textContent += text + '\n';
    if (pre.textContent.length > 200000) {
        pre.textContent = pre.textContent.slice(-100000);
    }
    if (!$('drawer').hidden) {
        pre.scrollTop = pre.scrollHeight;
    }
    console.log(text);
}

function mib(bytes) {
    return `${(bytes / 1048576).toFixed(0)} MiB`;
}

// True once the page is isolated; false when it cannot be; null while it reloads under the worker.
async function isolate() {
    if (self.crossOriginIsolated) {
        sessionStorage.removeItem(ISOLATE_FLAG);
        return true;
    }
    if (!('serviceWorker' in navigator) || sessionStorage.getItem(ISOLATE_FLAG)) {
        return false;
    }
    await navigator.serviceWorker.register('sw.js');
    await navigator.serviceWorker.ready;
    sessionStorage.setItem(ISOLATE_FLAG, '1');
    location.reload();
    return null;
}

async function gameDirectory() {
    const root = await navigator.storage.getDirectory();
    return root.getDirectoryHandle('chronicle', {create: true});
}

async function entry(directory, name) {
    try {
        return await directory.getFileHandle(name);
    } catch {
        try {
            return await directory.getDirectoryHandle(name);
        } catch {
            return null;
        }
    }
}

async function hasFile(directory) {
    for await (const handle of directory.values()) {
        if (handle.kind === 'file' || await hasFile(handle)) {
            return true;
        }
    }
    return false;
}

// What is stored, as {kind, size}, or null.
async function stored() {
    const home = await gameDirectory();
    for (const kind of KINDS) {
        const handle = await entry(home, kind);
        if (handle === null) {
            continue;
        }
        if (handle.kind === 'file') {
            const file = await handle.getFile();
            if (file.size > 0) {
                return {kind, size: file.size};
            }
        } else if (await hasFile(handle)) {
            return {kind, size: 0};
        }
    }
    return null;
}

async function removeAll() {
    const home = await gameDirectory();
    for (const kind of KINDS) {
        await home.removeEntry(kind, {recursive: true}).catch(() => {});
    }
}

// Copies files into the game's directory, each a [relative path, File] pair, with the progress bar showing bytes.
// createWritable writes to a swap file and commits on close, so a file cut short is never left in place.
async function copyFiles(files, progress = $('progress')) {
    const home = await gameDirectory();
    progress.hidden = false;
    progress.max = files.reduce((total, [, file]) => total + file.size, 0) || 1;
    progress.value = 0;
    let copied = 0;
    const directories = new Map();
    for (const [path, file] of files) {
        const parts = path.split('/').filter(Boolean);
        let directory = home;
        let key = '';
        for (const part of parts.slice(0, -1)) {
            key += '/' + part;
            if (!directories.has(key)) {
                directories.set(key, await directory.getDirectoryHandle(part, {create: true}));
            }
            directory = directories.get(key);
        }
        const writable = await (await directory.getFileHandle(parts.at(-1), {create: true})).createWritable();
        const counter = new TransformStream({
            transform(chunk, controller) {
                copied += chunk.byteLength;
                progress.value = copied;
                controller.enqueue(chunk);
            },
        });
        await file.stream().pipeThrough(counter).pipeTo(writable);
    }
    progress.hidden = true;
}

// A disc image or data.zip.
async function storeFile(file) {
    const name = file.name.toLowerCase();
    const kind = name.endsWith('.zip') ? 'data.zip' : name.endsWith('.iso') ? 'disc.iso' : null;
    if (kind === null) {
        throw new Error('choose a .iso disc image or a data.zip');
    }
    await removeAll();
    await copyFiles([[kind, file]]);
}

// A folder: the disc's DATA.DAT and DATA.HD2, or an extracted data folder (or the folder holding one).
async function storeFolder(list) {
    const files = [...list].map((file) => [file.webkitRelativePath.split('/').slice(1), file]);
    if (files.length === 0) {
        throw new Error('the folder is empty');
    }
    const top = (name) => files.find(([parts]) => parts.length === 1 && parts[0].toUpperCase() === name);
    const dat = top('DATA.DAT');
    const hd2 = top('DATA.HD2');
    await removeAll();
    if (dat && hd2) {
        await copyFiles([['disc/DATA.DAT', dat[1]], ['disc/DATA.HD2', hd2[1]]]);
        return;
    }
    // The folder holding data/, as a chronicle or run folder is, gives just its data/.
    const inner = files.filter(([parts]) => parts.length > 1 && parts[0].toLowerCase() === 'data');
    const chosen = inner.length > 0 ? inner.map(([parts, file]) => [parts.slice(1), file]) : files;
    await copyFiles(chosen.map(([parts, file]) => [['data', ...parts].join('/'), file]));
}

// MoonMod's mods: save/mods/<id>/, where the game looks for them (moonmod/runtime.cpp), each a folder holding
// mod.json. Chosen files come in as [path parts, Blob] pairs from a folder or a zip.
async function modsDirectory() {
    const save = await (await gameDirectory()).getDirectoryHandle('save', {create: true});
    return save.getDirectoryHandle('mods', {create: true});
}

async function readText(directory, name) {
    try {
        return await (await (await directory.getFileHandle(name)).getFile()).text();
    } catch {
        return null;
    }
}

// The stored mods, as [{id, manifest, problem}]: problem says why mod.json can't be read, and the game will
// list the mod as broken then.
async function storedMods() {
    const mods = [];
    for await (const [id, handle] of (await modsDirectory()).entries()) {
        if (handle.kind !== 'directory') {
            continue;
        }
        let manifest = {};
        let problem = null;
        try {
            const text = await readText(handle, 'mod.json');
            if (text === null) {
                problem = 'it has no mod.json';
            } else {
                manifest = JSON.parse(text);
            }
        } catch (error) {
            problem = `its mod.json can't be read (${error.message})`;
        }
        if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) {
            manifest = {};
            problem ??= 'its mod.json is not an object';
        }
        mods.push({id, manifest, problem});
    }
    return mods.sort((a, b) => (a.manifest.name ?? a.id).localeCompare(b.manifest.name ?? b.id));
}

// Splits chosen files into mods: the files themselves when mod.json is at the top, else each top folder that
// holds one. Each mod is stored under its manifest's id (its folder's name when it has none).
async function storeMods(files) {
    const isManifest = (parts) => parts.at(-1).toLowerCase() === 'mod.json';
    const roots = new Set();
    for (const [parts] of files) {
        if (isManifest(parts) && parts.length <= 2) {
            roots.add(parts.length === 1 ? '' : parts[0]);
        }
    }
    if (roots.size === 0) {
        throw new Error('no mod.json in it, at the top or in a folder at the top');
    }
    const copies = [];
    const ids = [];
    for (const root of roots) {
        const inside = files.filter(([parts]) => root === '' || (parts.length > 1 && parts[0] === root))
            .map(([parts, blob]) => [root === '' ? parts : parts.slice(1), blob]);
        const manifest = inside.find(([parts]) => parts.length === 1 && isManifest(parts));
        let id = root;
        try {
            id = JSON.parse(await manifest[1].text()).id || root;
        } catch {
            id = root;
        }
        if (!id || /[\\/]|^\.\.?$/.test(id)) {
            throw new Error(`a mod has no usable id (${id || 'none'})`);
        }
        await (await modsDirectory()).removeEntry(id, {recursive: true}).catch(() => {});
        copies.push(...inside.map(([parts, blob]) => [['save', 'mods', id, ...parts].join('/'), blob]));
        ids.push(id);
    }
    await copyFiles(copies, $('mod-progress'));
    await enableMods(ids, true);
    // A built-in mod the player adds themselves is their own copy from now on.
    await markBuiltin(ids, () => ({own: true}));
}

// MoonMod's profile, save/moonmod/profile.json, which the game's Mods menu also writes: which mods are on for the
// next start, and each mod's settings as {mod id: {setting id: value}}. The game reads it as it starts and keeps
// a setting's default for any value that doesn't fit the setting (moonmod/runtime.cpp).
async function profileFolder() {
    const save = await (await gameDirectory()).getDirectoryHandle('save', {create: true});
    return save.getDirectoryHandle('moonmod', {create: true});
}

async function readProfile() {
    let profile = null;
    try {
        profile = JSON.parse(await readText(await profileFolder(), 'profile.json') ?? 'null');
    } catch {
        profile = null;
    }
    if (!profile || typeof profile !== 'object' || profile.format_version !== 1) {
        profile = {format_version: 1};
    }
    for (const part of ['enabled', 'settings', 'native_trusted']) {
        if (!profile[part] || typeof profile[part] !== 'object' || Array.isArray(profile[part])) {
            profile[part] = {};
        }
    }
    return profile;
}

// Changes the profile with change(profile) and writes it, one change after another, so quick changes in a row
// each see the one before. Gives the profile as written.
let profileChanges = Promise.resolve();

function updateProfile(change) {
    const done = profileChanges.then(async () => {
        const profile = await readProfile();
        change(profile);
        const writable = await (await (await profileFolder()).getFileHandle('profile.json', {create: true}))
            .createWritable();
        await writable.write(JSON.stringify(profile, null, 2));
        await writable.close();
        return profile;
    });
    profileChanges = done.catch(() => {});
    return done;
}

// Turns mods on or off for the next start: a mod added here is on, and one removed is forgotten with its settings.
async function enableMods(ids, on, forget = false) {
    await updateProfile((profile) => {
        for (const id of ids) {
            if (on) {
                profile.enabled[id] = true;
            } else {
                delete profile.enabled[id];
                if (forget) {
                    delete profile.settings[id];
                }
            }
        }
    });
}

// The mods built into the page: MoonMod's, zipped into mods/builtin/ at the commit the build pins
// (port/web/mods/builtin.py), with index.json listing them. Each is installed into save/mods/ as if the player had
// added it, but off, as the game leaves a new mod; save/moonmod/builtin.json remembers, per mod, the version
// installed, whether the player removed it (it stays removed until restored) and whether the folder is the
// player's own copy (added on this page, or there before), which is left alone. A newer built-in version is
// copied over the old one's folder, so files the player saved into it (the animation editor's motions) stay.
const BUILTIN_STATE = 'builtin.json';
let builtinIndex;
let builtinInstalling = null;

async function builtinMods() {
    if (builtinIndex === undefined) {
        try {
            const response = await fetch('mods/builtin/index.json', {cache: 'no-cache'});
            builtinIndex = response.ok ? await response.json() : null;
        } catch {
            builtinIndex = null;
        }
        if (!Array.isArray(builtinIndex?.mods)) {
            builtinIndex = {mods: []};
        }
    }
    return builtinIndex.mods;
}

async function readBuiltinState() {
    try {
        const state = JSON.parse(await readText(await profileFolder(), BUILTIN_STATE) ?? '{}');
        return state && typeof state === 'object' && !Array.isArray(state) ? state : {};
    } catch {
        return {};
    }
}

async function writeBuiltinState(state) {
    const writable = await (await (await profileFolder()).getFileHandle(BUILTIN_STATE, {create: true}))
        .createWritable();
    await writable.write(JSON.stringify(state, null, 2));
    await writable.close();
}

// Changes one mod's record in builtin.json, for a mod the player adds or removes on the page.
async function markBuiltin(ids, record) {
    const listed = new Set((await builtinMods()).map((mod) => mod.id));
    const state = await readBuiltinState();
    let changed = false;
    for (const id of ids) {
        if (listed.has(id)) {
            state[id] = record(state[id] ?? {});
            changed = true;
        }
    }
    if (changed) {
        await writeBuiltinState(state);
    }
}

// Installs the built-in mods that are missing or older, one install at a time. restore brings back the ones the
// player removed. Resolves to false, with the status saying why, if one could not be installed.
function installBuiltinMods(restore = false) {
    const run = (builtinInstalling ?? Promise.resolve()).then(async () => {
        const mods = await builtinMods();
        if (mods.length === 0) {
            return true;
        }
        const state = await readBuiltinState();
        const folder = await modsDirectory();
        const due = [];
        for (const mod of mods) {
            const record = state[mod.id] ?? {};
            if (record.own || (record.removed && !restore)) {
                continue;
            }
            const present = await entry(folder, mod.id);
            if (present !== null && record.version === undefined && !record.removed) {
                // There before the page had built-in mods: the player's own.
                state[mod.id] = {own: true};
                continue;
            }
            if (present === null || record.version !== mod.version || record.removed) {
                due.push(mod);
            }
        }
        if (due.length === 0) {
            await writeBuiltinState(state);
            return true;
        }
        const start = $('start');
        start.disabled = true;
        try {
            setStatus(`Installing the built-in mods (${due.map((mod) => mod.name).join(', ')})…`);
            for (const mod of due) {
                const response = await fetch(`mods/builtin/${mod.file}`);
                if (!response.ok) {
                    throw new Error(`${mod.file}: ${response.status}`);
                }
                const files = [];
                for (const [path, inside] of await dcZip.open(await response.blob())) {
                    const parts = path.split('/').filter(Boolean);
                    if (parts.length > 1 && parts[0] === mod.id && !parts.includes('..')) {
                        files.push([['save', 'mods', ...parts].join('/'), await inside.read()]);
                    }
                }
                await copyFiles(files, $('mod-progress'));
                state[mod.id] = {version: mod.version};
                await writeBuiltinState(state);
            }
            return true;
        } catch (error) {
            setStatus(`Could not install the built-in mods: ${error.message ?? error}`);
            $('mod-progress').hidden = true;
            return false;
        } finally {
            start.disabled = false;
        }
    });
    builtinInstalling = run.catch(() => {});
    return run;
}

// Whether the game starts with mods: off starts it with MOONMOD_NO_MODS=1, every mod off for that run while the
// profile keeps its choices. Remembered in this browser only.
function modsOn() {
    try {
        return localStorage.getItem('dc.mods') !== 'off';
    } catch {
        return true;
    }
}

function setModsOn(on) {
    try {
        localStorage.setItem('dc.mods', on ? 'on' : 'off');
    } catch {
        // Not remembered; the switch still holds for this page.
    }
}

async function modsFromFolder(list) {
    return [...list].map((file) => [file.webkitRelativePath.split('/').slice(1), file]);
}

// A zip's own top folder is kept, so a zip of one mod's folder and a zip of its contents both work.
async function modsFromZip(file) {
    const files = [];
    for (const [path, entry] of await dcZip.open(file)) {
        files.push([path.split('/').filter(Boolean), await entry.read()]);
    }
    return files;
}

function element(tag, properties = {}, ...children) {
    const made = Object.assign(document.createElement(tag), properties);
    made.append(...children);
    return made;
}

// The mods a mod needs, with the ones they need in turn, among the stored mods.
function needed(mod, byId, seen = new Set()) {
    for (const id of mod.manifest.dependencies ?? []) {
        if (!seen.has(id) && byId.has(id)) {
            seen.add(id);
            needed(byId.get(id), byId, seen);
        }
    }
    return seen;
}

// One setting's control, as the game's Mods menu offers it (moonmod/manifest.cpp): a switch, a slider for a
// whole number in a short range, a number box, a list of choices, or text. Each change is saved at once.
function settingControl(setting, value, save) {
    const range = Number.isFinite(setting.min) && Number.isFinite(setting.max);
    switch (setting.type) {
        case 'boolean': {
            const box = element('input', {type: 'checkbox', checked: value === true});
            box.addEventListener('change', () => save(box.checked));
            return [box];
        }
        case 'integer':
        case 'number': {
            const step = setting.step > 0 ? setting.step : 1;
            const fit = (number) => {
                let fitted = range ? Math.min(setting.max, Math.max(setting.min, number)) : number;
                if (setting.type === 'integer') {
                    fitted = Math.round(fitted);
                }
                return fitted;
            };
            if (setting.type === 'integer' && range && (setting.max - setting.min) / step <= 200) {
                const slider = element('input', {type: 'range', min: setting.min, max: setting.max, step, value});
                const shown = element('output', {textContent: value});
                slider.addEventListener('input', () => shown.textContent = slider.value);
                slider.addEventListener('change', () => save(fit(Number(slider.value))));
                return [slider, shown];
            }
            const box = element('input', {type: 'number', step: setting.type === 'integer' ? step : 'any', value});
            if (range) {
                Object.assign(box, {min: setting.min, max: setting.max});
            }
            box.addEventListener('change', () => {
                const number = Number(box.value);
                if (box.value === '' || !Number.isFinite(number)) {
                    box.value = value;
                    return;
                }
                value = fit(number);
                box.value = value;
                save(value);
            });
            return [box];
        }
        case 'choice': {
            const list = element('select');
            for (const choice of setting.choices ?? []) {
                list.append(element('option', {value: choice, textContent: choice, selected: choice === value}));
            }
            list.addEventListener('change', () => save(list.value));
            return [list];
        }
        default: {
            const box = element('input', {type: 'text', maxLength: 256, value: value ?? ''});
            box.addEventListener('change', () => save(box.value));
            return [box];
        }
    }
}

// A setting's value in the profile when it fits the setting, else its default, as the game reads it.
function settingValue(setting, stored) {
    const fits = {
        boolean: () => typeof stored === 'boolean',
        integer: () => Number.isInteger(stored),
        number: () => Number.isFinite(stored),
        string: () => typeof stored === 'string' && stored.length <= 256,
        choice: () => (setting.choices ?? []).includes(stored),
    }[setting.type]?.() ?? false;
    const inRange = typeof stored !== 'number' ||
        ((setting.min ?? -Infinity) <= stored && stored <= (setting.max ?? Infinity));
    return fits && inRange ? stored : setting.default;
}

function settingsPanel(mod, profile) {
    const settings = Array.isArray(mod.manifest.settings) ? mod.manifest.settings : [];
    if (settings.length === 0) {
        return null;
    }
    const grid = element('div', {className: 'settings'});
    const draw = () => {
        grid.replaceChildren();
        const chosen = profile.settings[mod.id] ?? {};
        for (const setting of settings) {
            const save = async (value) => {
                const latest = await updateProfile((profile) => {
                    profile.settings[mod.id] = {...(profile.settings[mod.id] ?? {}), [setting.id]: value};
                });
                profile.settings = latest.settings;
            };
            const name = element('div', {className: 'name', textContent: setting.name ?? setting.id});
            if (setting.description) {
                name.append(element('small', {textContent: setting.description}));
            }
            grid.append(name, element('div', {className: 'value'},
                ...settingControl(setting, settingValue(setting, chosen[setting.id]), save)));
        }
    };
    draw();
    const reset = element('button', {className: 'secondary', textContent: 'Reset to defaults'});
    reset.addEventListener('click', async () => {
        const latest = await updateProfile((profile) => delete profile.settings[mod.id]);
        profile.settings = latest.settings;
        draw();
    });
    return element('details', {},
        element('summary', {textContent: `Settings (${settings.length})`}), grid,
        element('div', {className: 'row'}, reset));
}

async function refreshMods() {
    const list = $('mod-list');
    list.classList.toggle('off', !modsOn());
    const mods = await storedMods();
    const profile = await readProfile();
    const builtin = new Map((await builtinMods()).map((mod) => [mod.id, mod]));
    const builtinState = await readBuiltinState();
    const removedBuiltin = [...builtin.keys()].filter((id) => builtinState[id]?.removed);
    $('mod-restore').hidden = removedBuiltin.length === 0;
    $('mod-restore').textContent = `Restore the built-in mods (${removedBuiltin.length})`;
    const byId = new Map(mods.map((mod) => [mod.id, mod]));
    const open = new Set([...list.querySelectorAll('details[open]')].map((panel) => panel.dataset.mod));
    list.replaceChildren();
    if (mods.length === 0) {
        list.append(element('li', {textContent: 'None yet.', style: 'color: var(--dim)'}));
    }
    for (const mod of mods) {
        const on = profile.enabled[mod.id] === true;
        const toggle = element('input', {type: 'checkbox', checked: on, disabled: mod.problem !== null});
        toggle.addEventListener('change', async () => {
            // A mod turned on brings the stored mods it needs with it.
            await enableMods(toggle.checked ? [mod.id, ...needed(mod, byId)] : [mod.id], toggle.checked);
            await refreshMods();
        });
        const manifest = mod.manifest;
        const isBuiltin = builtin.has(mod.id) && builtinState[mod.id]?.version !== undefined;
        const title = element('span', {}, element('strong', {textContent: manifest.name ?? mod.id}),
            ' ', element('small', {textContent: [manifest.version, manifest.author && `by ${manifest.author}`,
                isBuiltin && 'built in'].filter(Boolean).join(' · ')}));
        const remove = element('button', {className: 'secondary', textContent: 'Remove'});
        remove.addEventListener('click', async () => {
            const again = builtin.has(mod.id) ? ' It is built in: "Restore the built-in mods" brings it back.' : '';
            if (!confirm(`Remove ${manifest.name ?? mod.id} and its settings from this browser?${again}`)) {
                return;
            }
            await (await modsDirectory()).removeEntry(mod.id, {recursive: true}).catch(() => {});
            await enableMods([mod.id], false, true);
            await markBuiltin([mod.id], () => ({removed: true}));
            await refreshMods();
        });
        const item = element('li', {},
            element('div', {className: 'mod-head'}, element('label', {}, toggle, title), remove));
        const note = (text, warn = false) =>
            item.append(element('div', {className: warn ? 'mod-note warn' : 'mod-note', textContent: text}));
        if (manifest.description) {
            note(manifest.description);
        }
        if (mod.problem) {
            note(`It can't load: ${mod.problem}.`, true);
        }
        for (const id of manifest.dependencies ?? []) {
            if (!byId.has(id)) {
                note(`It needs ${id}, which isn't added.`, true);
            } else if (on && profile.enabled[id] !== true) {
                note(`It needs ${byId.get(id).manifest.name ?? id}, which is off.`, true);
            }
        }
        if (manifest.native_extension) {
            note("Its native library doesn't run in the browser.", true);
        }
        const panel = settingsPanel(mod, profile);
        if (panel) {
            panel.dataset.mod = mod.id;
            panel.open = open.has(mod.id);
            item.append(panel);
        }
        list.append(item);
    }
}

// The mods for the PC (mods/catalog.json, port/web/mods/catalog.py): each mod version with the build it was
// confirmed with, both GitHub releases of the repository the list names. The newest version of each mod comes
// first, the older ones folded under it.
const PC_BUILDS = {
    windows: 'darkcloud-windows-x64.zip',
    linux: 'darkcloud-linux-x86_64.flatpak',
    macos: 'darkcloud-macos-arm64.tar.gz',
};

function pcSystem() {
    const platform = (navigator.userAgentData?.platform ?? navigator.platform ?? '').toLowerCase();
    if (platform.includes('mac')) {
        return 'macos';
    }
    return platform.includes('linux') && !/android/i.test(navigator.userAgent) ? 'linux' : 'windows';
}

function versionParts(version) {
    return version.split(/[+-]/)[0].split('.').map(Number);
}

function newerFirst(a, b) {
    const [x, y] = [versionParts(a.version), versionParts(b.version)];
    return y[0] - x[0] || y[1] - x[1] || y[2] - x[2];
}

// The repository's releases, newest first, or null when GitHub can't be asked. Asked once a page.
const PAGE_REPOSITORY = 'Jeffiest/DC_Web';
const releaseLists = new Map();

function releases(repository) {
    if (!releaseLists.has(repository)) {
        releaseLists.set(repository, (async () => {
            try {
                const all = [];
                for (let page = 1; page <= 10; page++) {
                    const response = await fetch(
                        `https://api.github.com/repos/${repository}/releases?per_page=100&page=${page}`);
                    if (!response.ok) {
                        return null;
                    }
                    const listed = await response.json();
                    all.push(...listed);
                    if (listed.length < 100) {
                        break;
                    }
                }
                return all.sort((a, b) => (b.published_at ?? '').localeCompare(a.published_at ?? ''));
            } catch {
                return null;
            }
        })());
    }
    return releaseLists.get(repository);
}

// The repository's release tags, or null when GitHub can't be asked (then every link is offered).
async function releaseTags(repository) {
    const all = await releases(repository);
    return all === null ? null : new Set(all.map((release) => release.tag_name));
}

const PC_SYSTEM_NAMES = {windows: 'Windows', linux: 'Linux', macos: 'macOS'};
const PC_INSTALL = {
    windows: 'Unzip the game anywhere and run darkcloud.exe. For mods, make a folder named mods beside ' +
        'darkcloud.exe and unzip the mods into it, a folder per mod.',
    linux: 'Install it with: flatpak install --user darkcloud-linux-x86_64.flatpak. For mods, unzip them into the ' +
        "game's save/mods/ folder, a folder per mod.",
    macos: "Unpack the archive and start the game. For mods, unzip them into the game's save/mods/ folder, a " +
        'folder per mod.',
};

// "Play on your PC": the newest of this repository's build releases (build-<commit>, .github/workflows/builds.yml)
// for the chosen system, and MoonMod's mods as the page builds them in (mods/builtin/, port/web/mods/builtin.py).
// Shown whether or not this browser can run the game.
async function showPcDownloads() {
    const select = $('pc-system');
    select.value = pcSystem();
    const mods = await builtinMods();
    const bundle = builtinIndex?.bundle;
    if (bundle?.file) {
        $('pc-mods-all').href = `mods/builtin/${bundle.file}`;
        $('pc-mods-all').textContent = `All the mods (.zip, ${mib(bundle.size)})`;
        $('pc-mods-all').hidden = false;
    }
    if (mods.length > 0) {
        $('pc-builtin').hidden = false;
        $('pc-builtin-list').replaceChildren(...mods.map((mod) => element('li', {},
            element('a', {href: `mods/builtin/${mod.file}`, textContent: `${mod.name} ${mod.version}`}),
            ' ', element('small', {style: 'color: var(--dim)', textContent: mod.id}))));
    }
    const builds = (await releases(PAGE_REPOSITORY))?.filter((release) => /^build-/.test(release.tag_name)) ?? null;
    const draw = () => {
        const system = select.value;
        const file = PC_BUILDS[system];
        const button = $('pc-build');
        const note = $('pc-build-note');
        $('pc-install').textContent = PC_INSTALL[system] + ' New mods start off: turn them on in Options > Game > ' +
            'Mod Manager.';
        if (builds === null) {
            button.href = `https://github.com/${PAGE_REPOSITORY}/releases`;
            button.textContent = `The game for ${PC_SYSTEM_NAMES[system]} (GitHub releases)`;
            note.textContent = "GitHub couldn't be asked for the newest build; its releases page lists them all.";
            return;
        }
        const release = builds.find((candidate) => candidate.assets?.some((asset) => asset.name === file));
        const asset = release?.assets.find((candidate) => candidate.name === file);
        button.hidden = !asset;
        if (!asset) {
            note.textContent = `There is no ${PC_SYSTEM_NAMES[system]} build yet.`;
            return;
        }
        button.href = asset.browser_download_url;
        button.textContent = `The game for ${PC_SYSTEM_NAMES[system]} (${mib(asset.size)})`;
        const commit = release.tag_name.slice('build-'.length);
        note.textContent = `Build ${commit}, ${(release.published_at ?? '').slice(0, 10)}. `;
        note.append(element('a', {href: release.html_url, textContent: 'Every file of this build'}));
    };
    select.addEventListener('change', draw);
    draw();
}

async function showPcMods() {
    let catalog;
    try {
        catalog = await (await fetch('mods/catalog.json', {cache: 'no-cache'})).json();
    } catch {
        return;
    }
    const entries = Array.isArray(catalog?.mods) ? catalog.mods : [];
    $('pc-mods').hidden = entries.length === 0;
    if (entries.length === 0) {
        return;
    }
    const released = await releaseTags(catalog.repository);
    const download = (tag, file) => `https://github.com/${catalog.repository}/releases/download/${tag}/${file}`;
    const link = (text, tag, file, secondary) => {
        const ready = released === null || released.has(tag);
        return element('a', {
            className: `button${secondary ? ' secondary' : ''}${ready ? '' : ' unpublished'}`,
            href: download(tag, file),
            textContent: ready ? text : `${text} (not published yet)`,
        });
    };
    const versions = new Map();
    for (const entry of entries) {
        versions.set(entry.id, [...(versions.get(entry.id) ?? []), entry]);
    }
    const draw = () => {
        const system = $('pc-system').value;
        const systemName = $('pc-system').selectedOptions[0].textContent;
        const list = $('pc-mod-list');
        list.replaceChildren();
        const sorted = [...versions.values()].map((all) => all.sort(newerFirst))
            .sort((a, b) => a[0].name.localeCompare(b[0].name));
        for (const [latest, ...older] of sorted) {
            const buttons = (entry) => {
                const build = `build-${entry.build.slice(0, 8)}`;
                return element('div', {className: 'row'},
                    link(`Game build ${entry.build.slice(0, 8)} for ${systemName}`, build, PC_BUILDS[system]),
                    link(`${entry.name} ${entry.version}`, `mod-${entry.id}-${entry.version}`,
                        `${entry.id}-${entry.version}.zip`, true));
            };
            const confirmed = (entry) =>
                `Confirmed with build ${entry.build.slice(0, 8)}${entry.confirmed ? ` on ${entry.confirmed}` : ''}.`;
            const item = element('li', {},
                element('div', {}, element('strong', {textContent: latest.name}), ' ',
                    element('small', {style: 'color: var(--dim)', textContent: [latest.version,
                        latest.author && `by ${latest.author}`].filter(Boolean).join(' · ')})));
            const note = (text) => item.append(element('div', {className: 'mod-note', style: 'margin-left: 0',
                textContent: text}));
            if (latest.description) {
                note(latest.description);
            }
            if (latest.note) {
                note(latest.note);
            }
            note(confirmed(latest));
            item.append(buttons(latest));
            if (older.length > 0) {
                const panel = element('details', {}, element('summary', {textContent: `Older versions (${older.length})`}));
                for (const entry of older) {
                    panel.append(element('div', {style: 'margin-top: 8px'},
                        element('div', {textContent: `${entry.version}: ${confirmed(entry)}`}), buttons(entry)));
                }
                item.append(panel);
            }
            list.append(item);
        }
    };
    $('pc-system').addEventListener('change', draw);
    draw();
}

function describe(what) {
    switch (what.kind) {
        case 'data.zip':
            return `data.zip is stored (${mib(what.size)}).`;
        case 'data':
            return 'The extracted game data is stored.';
        case 'disc.iso':
            return `The disc image is stored (${mib(what.size)}). The game reads it in place.`;
        default:
            return "The disc's files are stored. The game reads them in place.";
    }
}

async function refresh() {
    const what = await stored();
    $('pick').hidden = what !== null;
    $('ready').hidden = what === null;
    $('mods').hidden = what === null;
    if (what) {
        $('stored').textContent = describe(what);
        const installed = await installBuiltinMods();
        await refreshMods();
        if (installed) {
            setStatus('Ready.');
        }
    } else {
        setStatus('No game data in this browser yet.');
    }
}

// The game's "extract: files total bytes total" lines, shown over the canvas while it prepares a disc's data.
function notice(text, fraction) {
    $('notice').hidden = text === null;
    $('notice-text').textContent = text ?? '';
    const bar = $('notice-progress');
    bar.hidden = fraction === undefined;
    if (fraction !== undefined) {
        bar.max = 1;
        bar.value = fraction;
    }
}

function print(text) {
    const extract = /^extract: (\d+) (\d+) (\d+) (\d+)$/.exec(text);
    if (extract) {
        const [files, total, bytes, totalBytes] = extract.slice(1).map(Number);
        notice(`Preparing the game data: ${files} of ${total} files`, totalBytes > 0 ? bytes / totalBytes : 0);
        return;
    }
    if (text === 'extract: done') {
        notice(null);
    } else if (text.startsWith('extract: ')) {
        notice(text.slice('extract: '.length));
    }
    log(text);
}

// The on-screen gamepad on a touch screen (port/web/src/touch.cpp): its state goes to the game as a button mask
// in Android's order and two sticks from -1 to 1, y down; the game says when it shows and how opaque it is.
const PAD_BITS = {up: 0, down: 1, left: 2, right: 3, cross: 4, circle: 5, square: 6, triangle: 7,
    l1: 8, r1: 9, l2: 10, r2: 11, select: 12, start: 13};
const TOUCH_SCREEN = matchMedia('(pointer: coarse)').matches;
const touches = new Map(); // pointer id -> {mask} or {stick, x, y}

function sendPad() {
    let mask = 0;
    const sticks = {left: [0, 0], right: [0, 0]};
    for (const touch of touches.values()) {
        mask |= touch.mask ?? 0;
        if (touch.stick) {
            sticks[touch.stick] = [touch.x, touch.y];
        }
    }
    window.Module?._dc_web_touch_state?.(mask, ...sticks.left, ...sticks.right);
}

// Where a touch lies in an element, from -1 to 1 each way.
function offset(element, event) {
    const box = element.getBoundingClientRect();
    return [(event.clientX - box.left) / box.width * 2 - 1, (event.clientY - box.top) / box.height * 2 - 1];
}

function dpadMask([x, y]) {
    const dead = 0.33;
    return (y < -dead) << PAD_BITS.up | (y > dead) << PAD_BITS.down | (x < -dead) << PAD_BITS.left |
        (x > dead) << PAD_BITS.right;
}

function setupPad() {
    const track = (element, update) => {
        element.addEventListener('pointerdown', (event) => {
            event.preventDefault();
            try {
                element.setPointerCapture(event.pointerId);
            } catch {
                // The pointer already lifted (Android hands a quick tap's pointerdown late); its move and up still
                // reach this element.
            }
            touches.set(event.pointerId, update(event));
            sendPad();
        });
        element.addEventListener('pointermove', (event) => {
            if (touches.has(event.pointerId)) {
                touches.set(event.pointerId, update(event));
                sendPad();
            }
        });
        const release = (event) => {
            if (touches.delete(event.pointerId)) {
                update(null);
                sendPad();
            }
        };
        element.addEventListener('pointerup', release);
        element.addEventListener('pointercancel', release);
    };
    for (const button of document.querySelectorAll('#pad [data-button]')) {
        const bit = 1 << PAD_BITS[button.dataset.button];
        track(button, (event) => {
            button.classList.toggle('held', event !== null);
            return {mask: bit};
        });
    }
    const dpad = $('pad-dpad');
    track(dpad, (event) => {
        const mask = event ? dpadMask(offset(dpad, event)) : 0;
        for (const arm of dpad.children) {
            arm.classList.toggle('held', (mask & 1 << PAD_BITS[arm.dataset.dir]) !== 0);
        }
        return {mask};
    });
    for (const [id, stick] of [['pad-left', 'left'], ['pad-right', 'right']]) {
        const base = $(id);
        const knob = base.firstElementChild;
        track(base, (event) => {
            let [x, y] = event ? offset(base, event) : [0, 0];
            const length = Math.hypot(x, y);
            if (length > 1) {
                x /= length;
                y /= length;
            }
            knob.style.transform = `translate(${x * 75}%, ${y * 75}%)`;
            return {stick, x, y};
        });
    }
}

// The Capture button's frames, as the game drew them (port/web/vulkan/device.cpp, CaptureFrame), laid out in one
// image: frames the screen shows wrong but the capture shows right are the browser's, not the game's.
const kCaptureFrames = 8;
let captured = [];

function showCaptured() {
    const columns = 2;
    const width = Math.min(800, captured[0].width);
    const height = Math.round(captured[0].height * width / captured[0].width);
    const sheet = document.createElement('canvas');
    sheet.width = columns * width;
    sheet.height = Math.ceil(captured.length / columns) * height;
    const context = sheet.getContext('2d');
    const frame = document.createElement('canvas');
    captured.forEach(({pixels, width: w, height: h}, index) => {
        frame.width = w;
        frame.height = h;
        frame.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(pixels.buffer), w, h), 0, 0);
        const x = (index % columns) * width;
        const y = Math.floor(index / columns) * height;
        context.drawImage(frame, x, y, width, height);
        context.fillStyle = '#ff0';
        context.font = `${Math.round(height / 8)}px sans-serif`;
        context.fillText(String(index + 1), x + 8, y + Math.round(height / 7));
    });
    captured = [];
    sheet.toBlob((blob) => {
        const url = URL.createObjectURL(blob);
        $('captured-image').src = url;
        $('captured-save').href = url;
        $('captured').hidden = false;
        log(`capture: ${kCaptureFrames} frames ready`);
    });
}

// The GPU's name as WebGL tells it, or '' where it does not.
function gpuName() {
    try {
        const gl = document.createElement('canvas').getContext('webgl2');
        if (!gl) {
            return '';
        }
        const info = gl.getExtension('WEBGL_debug_renderer_info');
        const name = String(gl.getParameter(info ? info.UNMASKED_RENDERER_WEBGL : gl.RENDERER) ?? '');
        gl.getExtension('WEBGL_lose_context')?.loseContext();
        return name;
    } catch {
        return '';
    }
}

function hasWebGl2() {
    try {
        return !!document.createElement('canvas').getContext('webgl2');
    } catch {
        return false;
    }
}

// Which build the game runs as: WebGL2 where there is no WebGPU, and on Arm's Mali GPUs (Android phones), whose
// WebGPU showed frames before they were drawn and ran the game at half speed once made to wait (docs/WEB.md);
// WebGPU elsewhere. ?renderer=gl or ?renderer=webgpu chooses.
function chooseRenderer() {
    const asked = new URLSearchParams(location.search).get('renderer');
    if (asked === 'gl' || asked === 'webgpu') {
        return asked;
    }
    if (!navigator.gpu) {
        return 'gl';
    }
    return /mali/i.test(gpuName()) ? 'gl' : 'webgpu';
}

function start() {
    $('setup').hidden = true;
    $('game').hidden = false;
    const extra = new URLSearchParams(location.search).get('args');
    const args = ['--data', '/opfs/chronicle/data', '--save', '/opfs/chronicle/save'];
    if (extra) {
        args.push(...extra.split(/\s+/).filter(Boolean));
    }
    window.dcLogging = true;
    kept.length = 0;
    log(`${new Date().toISOString()} ${navigator.userAgent}`);
    const renderer = chooseRenderer();
    log(`darkcloud ${args.join(' ')} (${renderer === 'gl' ? 'WebGL2' : 'WebGPU'}, ${gpuName() || 'GPU unknown'})`);
    const canvas = $('canvas');
    // Threads still running when the game exits can abort as it takes them down; the exit is what matters.
    let exited = false;
    const withMods = modsOn();
    if (!withMods) {
        log('MOONMOD_NO_MODS=1: every mod is off for this run');
    }
    window.Module = {
        arguments: args,
        canvas,
        // ENV is the game script's own top-level variable, and Module.ENV where the build exports it.
        preRun: withMods ? [] : [() => (Module.ENV ?? globalThis.ENV).MOONMOD_NO_MODS = '1'],
        print,
        printErr: print,
        onRuntimeInitialized() {
            if (TOUCH_SCREEN) {
                Module._dc_web_touch_screen(1);
            }
        },
        dcTouchVisible(visible) {
            $('pad').hidden = !(visible && TOUCH_SCREEN);
            if ($('pad').hidden && touches.size > 0) {
                touches.clear();
                sendPad();
            }
        },
        dcTouchOpacity(opacity) {
            $('pad').style.opacity = String(opacity);
        },
        dcCaptured(pixels, width, height, remaining) {
            captured.push({pixels, width, height});
            if (remaining <= 0) {
                showCaptured();
            }
        },
        onExit(code) {
            exited = true;
            log(`exit=${code}`);
            notice(`The game exited (${code}).`);
        },
        onAbort(reason) {
            log(`abort: ${reason}`);
            if (exited) {
                return;
            }
            notice('The game stopped with an error. The log has what it printed.');
            $('drawer').hidden = false;
        },
    };
    const script = document.createElement('script');
    script.src = renderer === 'gl' ? 'darkcloud_gl.js' : 'darkcloud.js';
    document.body.appendChild(script);
    canvas.addEventListener('click', () => canvas.focus());
    canvas.focus();
}

// The last run's log, if a run left one (keepLog), whether or not this browser can run the game.
function showPreviousLog() {
    let previous = null;
    try {
        previous = localStorage.getItem(LOG_KEY);
    } catch {
        previous = null;
    }
    if (!previous) {
        return;
    }
    $('last-log').textContent = previous;
    $('last-log-toggle').hidden = false;
    $('last-log-toggle').addEventListener('click', () => {
        $('last-log').hidden = !$('last-log').hidden;
        $('last-log').scrollTop = $('last-log').scrollHeight;
    });
}

async function main() {
    showPreviousLog();
    showPcDownloads().catch((error) => log(`[page error] PC downloads: ${error}`));
    $('fullscreen').addEventListener('click', () => {
        if (document.fullscreenElement) {
            document.exitFullscreen();
        } else {
            $('game').requestFullscreen?.().catch(() => {});
        }
    });
    setupPad();
    $('capture').addEventListener('click', () => {
        if (window.Module && Module._dc_web_capture) {
            captured = [];
            log(`capture: the next ${kCaptureFrames} frames`);
            Module._dc_web_capture(kCaptureFrames);
        }
        $('canvas').focus();
    });
    $('captured-close').addEventListener('click', () => {
        $('captured').hidden = true;
        $('canvas').focus();
    });
    $('log-toggle').addEventListener('click', () => {
        $('drawer').hidden = !$('drawer').hidden;
        $('log').scrollTop = $('log').scrollHeight;
        $('canvas').focus();
    });

    const isolated = await isolate();
    if (isolated === null) {
        return;
    }
    if (!isolated) {
        setStatus('This browser cannot run the game: it needs cross-origin isolation for threads.');
        return;
    }
    if (!navigator.storage || !navigator.storage.getDirectory) {
        setStatus('This browser has no origin private file system to keep the game data in.');
        return;
    }
    if (!navigator.gpu && !hasWebGl2()) {
        setStatus('This browser has neither WebGPU nor WebGL2 to draw the game with. Use a recent Chrome or Edge.');
        return;
    }
    navigator.storage.persist?.();


    const choose = (store) => async (event) => {
        const chosen = event.target.files;
        if (!chosen || chosen.length === 0) {
            return;
        }
        setStatus('Copying into this browser…');
        try {
            await store(chosen);
        } catch (error) {
            setStatus(`Could not store it: ${error.message ?? error}`);
            $('progress').hidden = true;
            return;
        }
        event.target.value = '';
        await refresh();
    };
    $('file').addEventListener('change', choose((files) => storeFile(files[0])));
    $('folder').addEventListener('change', choose(storeFolder));
    const addMods = (gather) => async (event) => {
        const chosen = event.target.files;
        if (!chosen || chosen.length === 0) {
            return;
        }
        setStatus('Adding the mod…');
        try {
            await storeMods(await gather(chosen));
            setStatus('Ready.');
        } catch (error) {
            setStatus(`Could not add it: ${error.message ?? error}`);
            $('mod-progress').hidden = true;
        }
        event.target.value = '';
        await refreshMods();
    };
    $('mod-folder').addEventListener('change', addMods(modsFromFolder));
    $('mod-zip').addEventListener('change', addMods((files) => modsFromZip(files[0])));
    showPcMods();
    $('mod-restore').addEventListener('click', async () => {
        if (await installBuiltinMods(true)) {
            setStatus('Ready.');
        }
        await refreshMods();
    });
    $('mods-on').checked = modsOn();
    $('mods-on').addEventListener('change', async () => {
        setModsOn($('mods-on').checked);
        await refreshMods();
    });
    $('start').addEventListener('click', start);
    $('forget').addEventListener('click', async () => {
        await removeAll();
        await refresh();
    });
    await refresh();
}

// Errors the page itself meets go to the log too, as the game's do.
window.addEventListener('error', (event) => log(`[page error] ${event.message} (${event.filename}:${event.lineno})`));
window.addEventListener('unhandledrejection', (event) => log(`[page error] ${event.reason?.stack ?? event.reason}`));

main().catch((error) => setStatus(`Startup failed: ${error}`));
