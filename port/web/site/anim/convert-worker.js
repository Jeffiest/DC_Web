// Runs the model converter (dc_anim_convert.js, port/web/anim/convert.cpp) off the page's thread: {id, path, bytes}
// in, a pack's raw path and bytes; {id, files: [[path under data/, bytes]]} or {id, error} out. Each conversion
// gets a fresh instance, so nothing of the last one's file system is left over.
'use strict';

importScripts('dc_anim_convert.js');

function collect(FS, directory, prefix, files) {
    for (const name of FS.readdir(directory)) {
        if (name === '.' || name === '..') {
            continue;
        }
        const path = directory + '/' + name;
        if (FS.isDir(FS.stat(path).mode)) {
            collect(FS, path, prefix + name + '/', files);
        } else {
            files.push([prefix + name, FS.readFile(path)]);
        }
    }
}

onmessage = async ({data: {id, path, bytes}}) => {
    const log = [];
    try {
        const module = await DcConvert({print: (t) => log.push(t), printErr: (t) => log.push(t)});
        const raw = '/raw/' + path;
        module.FS.mkdirTree(raw.slice(0, raw.lastIndexOf('/')));
        module.FS.writeFile(raw, bytes);
        const code = module.callMain(['/raw', '/out']);
        if (code !== 0) {
            throw new Error(log.join('\n') || `the converter stopped (${code})`);
        }
        const files = [];
        collect(module.FS, '/out/data', '', files);
        postMessage({id, files}, files.map(([, b]) => b.buffer));
    } catch (error) {
        postMessage({id, error: String(error.message ?? error)});
    }
};
