// The page around the game (docs/WEB.md). It makes the page cross-origin isolated (sw.js), keeps the player's
// game data in the origin private file system under chronicle/, and starts darkcloud.js with that directory
// mounted at /opfs (port/web/src/storage.cpp). Extra arguments for the game can be given as
// ?args=--headless+--frames+30.
//
// What the player chooses is stored as one of:
// - chronicle/data.zip, the archive dcdata packs;
// - chronicle/data/, an extracted data folder;
// - chronicle/disc.iso or chronicle/disc/ (DATA.DAT and DATA.HD2), which the game extracts into chronicle/data/
//   at its first start (port/web/src/firstrun.cpp) and then removes.
// Storing one removes the others.

'use strict';

const $ = (id) => document.getElementById(id);
const ISOLATE_FLAG = 'dc-isolate-reload';
const KINDS = ['data.zip', 'data', 'disc.iso', 'disc'];

function setStatus(text) {
    $('status').textContent = text;
}

function log(text) {
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
async function copyFiles(files) {
    const home = await gameDirectory();
    const progress = $('progress');
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

function describe(what) {
    switch (what.kind) {
        case 'data.zip':
            return `data.zip is stored (${mib(what.size)}).`;
        case 'data':
            return 'The extracted game data is stored.';
        case 'disc.iso':
            return `The disc image is stored (${mib(what.size)}). The game extracts it when it starts.`;
        default:
            return "The disc's files are stored. The game extracts them when it starts.";
    }
}

async function refresh() {
    const what = await stored();
    $('pick').hidden = what !== null;
    $('ready').hidden = what === null;
    if (what) {
        $('stored').textContent = describe(what);
        setStatus('Ready.');
    } else {
        setStatus('No game data in this browser yet.');
    }
}

// The game's "extract: files total bytes total" lines, shown over the canvas while a disc is extracted.
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
        notice(`Extracting the game data: ${files} of ${total} files`, totalBytes > 0 ? bytes / totalBytes : 0);
        return;
    }
    if (text === 'extract: done') {
        notice(null);
    } else if (text.startsWith('extract: ')) {
        notice(text.slice('extract: '.length));
    }
    log(text);
}

function start() {
    $('setup').hidden = true;
    $('game').hidden = false;
    const extra = new URLSearchParams(location.search).get('args');
    const args = ['--data', '/opfs/chronicle/data', '--save', '/opfs/chronicle/save'];
    if (extra) {
        args.push(...extra.split(/\s+/).filter(Boolean));
    }
    log('darkcloud ' + args.join(' '));
    const canvas = $('canvas');
    window.Module = {
        arguments: args,
        canvas,
        print,
        printErr: print,
        onExit(code) {
            log(`exit=${code}`);
            notice(`The game exited (${code}).`);
        },
        onAbort(reason) {
            log(`abort: ${reason}`);
            notice('The game stopped with an error. The log has what it printed.');
            $('drawer').hidden = false;
        },
    };
    const script = document.createElement('script');
    script.src = 'darkcloud.js';
    document.body.appendChild(script);
    canvas.addEventListener('click', () => canvas.focus());
    canvas.focus();
}

async function main() {
    $('fullscreen').addEventListener('click', () => {
        if (document.fullscreenElement) {
            document.exitFullscreen();
        } else {
            $('game').requestFullscreen?.().catch(() => {});
        }
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
    if (!navigator.gpu) {
        setStatus('This browser has no WebGPU, which the game draws with. Use a recent Chrome or Edge.');
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
    $('start').addEventListener('click', start);
    $('forget').addEventListener('click', async () => {
        await removeAll();
        await refresh();
    });
    await refresh();
}

main().catch((error) => setStatus(`Startup failed: ${error}`));
