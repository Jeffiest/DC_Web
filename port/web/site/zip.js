// Reads a .zip in the browser, a file at a time, without loading it whole: the mods the page stores (shell.js) and
// data.zip for the animation editor (anim/). Stored and deflated entries; no ZIP64, which neither needs.
//
// dcZip.open(blob) gives a Map of path -> {size, read()}, read() resolving to the entry's bytes as a Blob.
'use strict';

window.dcZip = (() => {
    const u16 = (view, at) => view.getUint16(at, true);
    const u32 = (view, at) => view.getUint32(at, true);

    async function view(blob, start, end) {
        return new DataView(await blob.slice(start, end).arrayBuffer());
    }

    // The end of central directory record: within the last 64 KiB and 22 bytes, the comment's room.
    async function endRecord(blob) {
        const from = Math.max(0, blob.size - 65557);
        const tail = await view(blob, from, blob.size);
        for (let at = tail.byteLength - 22; at >= 0; at--) {
            if (u32(tail, at) === 0x06054b50) {
                return {count: u16(tail, at + 10), size: u32(tail, at + 12), offset: u32(tail, at + 16)};
            }
        }
        throw new Error('not a zip file');
    }

    async function open(blob) {
        const end = await endRecord(blob);
        const directory = await view(blob, end.offset, end.offset + end.size);
        const decoder = new TextDecoder();
        const entries = new Map();
        let at = 0;
        for (let index = 0; index < end.count; index++) {
            if (u32(directory, at) !== 0x02014b50) {
                throw new Error('the zip directory is damaged');
            }
            const method = u16(directory, at + 10);
            const packed = u32(directory, at + 20);
            const size = u32(directory, at + 24);
            const nameLength = u16(directory, at + 28);
            const skip = u16(directory, at + 30) + u16(directory, at + 32);
            const local = u32(directory, at + 42);
            // Windows' own zips write backslashes.
            const name = decoder.decode(new Uint8Array(directory.buffer, directory.byteOffset + at + 46, nameLength))
                .replace(/\\/g, '/');
            at += 46 + nameLength + skip;
            if (name.endsWith('/')) {
                continue;
            }
            if (method !== 0 && method !== 8) {
                throw new Error(`${name} is packed a way this page cannot read (method ${method})`);
            }
            entries.set(name, {
                size,
                async read() {
                    const header = await view(blob, local, local + 30);
                    const data = local + 30 + u16(header, 26) + u16(header, 28);
                    const raw = blob.slice(data, data + packed);
                    if (method === 0) {
                        return raw;
                    }
                    const stream = raw.stream().pipeThrough(new DecompressionStream('deflate-raw'));
                    return new Response(stream).blob();
                },
            });
        }
        return entries;
    }

    return {open};
})();
