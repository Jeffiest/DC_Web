// The page around the game (docs/WEB.md). It makes the page cross-origin isolated (sw.js), keeps the
// player's data.zip in the origin private file system as chronicle/data.zip, and starts darkcloud.js with
// that directory mounted at /opfs (port/web/src/storage.cpp). Extra arguments for the game can be given
// as ?args=--headless+--frames+30.

'use strict';

const $ = (id) => document.getElementById(id);
const ISOLATE_FLAG = 'dc-isolate-reload';
const ARCHIVE = 'data.zip';

function setStatus(text) {
    $('status').textContent = text;
}

function log(text) {
    const pre = $('log');
    pre.textContent += text + '\n';
    if (pre.textContent.length > 200000) {
        pre.textContent = pre.textContent.slice(-100000);
    }
    pre.scrollTop = pre.scrollHeight;
    console.log(text);
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

async function storedArchive() {
    try {
        const file = await (await (await gameDirectory()).getFileHandle(ARCHIVE)).getFile();
        return file.size > 0 ? file : null;
    } catch {
        return null;
    }
}

// createWritable writes to a swap file and commits on close, so an interrupted copy leaves nothing behind.
async function storeArchive(file) {
    const handle = await (await gameDirectory()).getFileHandle(ARCHIVE, {create: true});
    const writable = await handle.createWritable();
    const progress = $('progress');
    progress.hidden = false;
    progress.max = file.size;
    progress.value = 0;
    let copied = 0;
    const counter = new TransformStream({
        transform(chunk, controller) {
            copied += chunk.byteLength;
            progress.value = copied;
            controller.enqueue(chunk);
        },
    });
    await file.stream().pipeThrough(counter).pipeTo(writable);
    progress.hidden = true;
}

function showReady(file) {
    $('pick').hidden = true;
    $('ready').hidden = false;
    setStatus(`data.zip is stored (${(file.size / 1048576).toFixed(0)} MiB).`);
}

function showPick() {
    $('ready').hidden = true;
    $('pick').hidden = false;
    setStatus('No game data in this browser yet.');
}

function start() {
    $('ready').hidden = true;
    setStatus('Running.');
    const extra = new URLSearchParams(location.search).get('args');
    const args = ['--data', '/opfs/chronicle/data', '--save', '/opfs/chronicle/save'];
    if (extra) {
        args.push(...extra.split(/\s+/).filter(Boolean));
    }
    log('darkcloud ' + args.join(' '));
    window.Module = {
        arguments: args,
        canvas: $('canvas'),
        print: log,
        printErr: log,
        onExit(code) {
            log(`exit=${code}`);
            setStatus(`The game exited (${code}).`);
        },
        onAbort(reason) {
            log(`abort: ${reason}`);
            setStatus('The game stopped with an error.');
        },
    };
    const script = document.createElement('script');
    script.src = 'darkcloud.js';
    document.body.appendChild(script);
}

async function main() {
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
    navigator.storage.persist?.();

    $('file').addEventListener('change', async (event) => {
        const file = event.target.files[0];
        if (!file) {
            return;
        }
        setStatus(`Copying ${file.name}…`);
        try {
            await storeArchive(file);
        } catch (error) {
            setStatus(`Could not store ${file.name}: ${error}`);
            return;
        }
        showReady(await storedArchive());
    });
    $('start').addEventListener('click', start);
    $('forget').addEventListener('click', async () => {
        await (await gameDirectory()).removeEntry(ARCHIVE);
        showPick();
    });

    const stored = await storedArchive();
    if (stored) {
        showReady(stored);
    } else {
        showPick();
    }
}

main().catch((error) => setStatus(`Startup failed: ${error}`));
