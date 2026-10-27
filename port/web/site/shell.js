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
            return `The disc image is stored (${mib(what.size)}). The game reads it in place.`;
        default:
            return "The disc's files are stored. The game reads them in place.";
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
            element.setPointerCapture(event.pointerId);
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
    window.Module = {
        arguments: args,
        canvas,
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
