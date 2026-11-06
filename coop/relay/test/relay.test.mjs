// Drives the relay as the games do: starts `wrangler dev` (or uses RELAY_URL), then hosts, joins and talks.
//   npm test                                   against a local wrangler dev
//   RELAY_URL=wss://<relay> npm test           against a deployed relay
import {spawn} from 'node:child_process';
import WebSocket from 'ws';

let passed = 0;
let failed = 0;
function check(ok, what, detail = '') {
    if (ok) {
        passed++;
        console.log(`  ok   ${what}`);
    } else {
        failed++;
        console.log(`  FAIL ${what} ${detail}`);
    }
}

// A player: its socket, the server's JSON events and the binary messages it received.
function player(url) {
    return new Promise((resolve) => {
        const socket = new WebSocket(url);
        const p = {socket, events: [], messages: [], closed: null};
        socket.binaryType = 'nodebuffer';
        socket.on('message', (data, binary) => {
            if (binary) {
                p.messages.push({from: data[0], text: data.subarray(1).toString()});
            } else {
                p.events.push(JSON.parse(data.toString()));
            }
        });
        socket.on('close', (code) => {
            p.closed = code;
        });
        socket.on('error', () => {});
        p.send = (to, text) => socket.send(Buffer.concat([Buffer.from([to]), Buffer.from(text)]));
        p.event = (t) => p.events.find((e) => e.t === t);
        const wait = setInterval(() => {
            if (p.events.length > 0 || p.closed !== null) {
                clearInterval(wait);
                resolve(p);
            }
        }, 20);
    });
}
const pause = (ms) => new Promise((r) => setTimeout(r, ms));

async function startDev() {
    // A process group of its own, so stopping it stops wrangler's workerd too.
    const dev = spawn('npx', ['wrangler', 'dev', '--port', '8787', '--ip', '127.0.0.1'], {stdio: ['ignore', 'pipe', 'pipe'], detached: true});
    dev.stop = () => {
        try {
            process.kill(-dev.pid);
        } catch {
            dev.kill();
        }
    };
    let output = '';
    dev.stdout.on('data', (d) => (output += d));
    dev.stderr.on('data', (d) => (output += d));
    for (let i = 0; i < 120; i++) {
        await pause(500);
        try {
            if ((await fetch('http://127.0.0.1:8787/health')).ok) {
                return dev;
            }
        } catch {
            // not up yet
        }
    }
    dev.stop();
    throw new Error(`wrangler dev did not start:\n${output}`);
}

// RELAY_SERVER=node: the private server (server.mjs) instead of the Worker.
async function startServer() {
    const server = spawn(process.execPath, ['server.mjs', '--port', '8788', '--host', '127.0.0.1', '--quiet'],
        {stdio: ['ignore', 'pipe', 'pipe'], detached: true});
    server.stop = () => server.kill();
    for (let i = 0; i < 40; i++) {
        await pause(250);
        try {
            if ((await fetch('http://127.0.0.1:8788/health')).ok) {
                return server;
            }
        } catch {
            // not up yet
        }
    }
    server.kill();
    throw new Error('server.mjs did not start');
}

const node = process.env.RELAY_SERVER === 'node';
const dev = process.env.RELAY_URL ? null : node ? await startServer() : await startDev();
const base = process.env.RELAY_URL ?? (node ? 'ws://127.0.0.1:8788' : 'ws://127.0.0.1:8787');
const q = (name, proto = '1') => `?game=chronicle&proto=${proto}&name=${encodeURIComponent(name)}`;
try {
    console.log('A room of four');
    const host = await player(`${base}/v1/host${q('Toan')}`);
    const welcome = host.event('welcome');
    check(welcome && welcome.id === 0 && /^[A-Z2-9]{5}$/.test(welcome.code), 'hosting opens a room with a five-letter code',
        JSON.stringify(host.events));
    const code = welcome.code;
    const xiao = await player(`${base}/v1/join/${code.toLowerCase()}${q('Xiao')}`);
    const w1 = xiao.event('welcome');
    check(w1 && w1.id === 1 && w1.peers.length === 1 && w1.peers[0].name === 'Toan', 'a guest joins (the code in any case) as 1, and sees the host',
        JSON.stringify(xiao.events));
    await pause(100);
    check(host.events.some((e) => e.t === 'join' && e.id === 1 && e.name === 'Xiao'), 'the host hears the guest join');
    const goro = await player(`${base}/v1/join/${code}${q('Goro <b>')}`);
    const ruby = await player(`${base}/v1/join/${code}${q('')}`);
    check(goro.event('welcome')?.id === 2 && ruby.event('welcome')?.id === 3, 'two more take 2 and 3');
    check(goro.event('welcome') && host.events.length >= 1, 'names are cleaned');
    await pause(100);
    check(xiao.events.some((e) => e.t === 'join' && e.id === 2 && e.name === 'Goro b'), 'markup is taken out of a name',
        JSON.stringify(xiao.events));
    check(xiao.events.some((e) => e.t === 'join' && e.id === 3 && e.name === 'Player 4'), 'an empty name becomes Player n');
    const ungaga = await player(`${base}/v1/join/${code}${q('Ungaga')}`);
    check(ungaga.event('error')?.reason.includes('full'), 'a fifth is refused: the room is full', JSON.stringify(ungaga.events));

    console.log('Messages');
    host.send(255, 'hello all');
    xiao.send(0, 'to the host');
    goro.send(1, 'to xiao');
    await pause(200);
    check(['hello all'].every((t) => xiao.messages.some((m) => m.from === 0 && m.text === t)) &&
        goro.messages.some((m) => m.from === 0 && m.text === 'hello all') && !host.messages.some((m) => m.text === 'hello all'),
    'a broadcast reaches everyone but the sender');
    check(host.messages.some((m) => m.from === 1 && m.text === 'to the host') && !goro.messages.some((m) => m.text === 'to the host'),
        'a message to the host reaches only the host');
    check(xiao.messages.some((m) => m.from === 2 && m.text === 'to xiao'), 'a guest can message another guest');
    const big = Buffer.alloc(40 * 1024, 65);
    xiao.socket.send(Buffer.concat([Buffer.from([0]), big]));
    await pause(150);
    check(!host.messages.some((m) => m.text.length > 32 * 1024), 'a message over 32 KiB is dropped');

    for (let i = 0; i < 100; i++) {
        xiao.send(0, `n${i}`);
    }
    await pause(400);
    const numbered = host.messages.filter((m) => m.from === 1 && /^n\d+$/.test(m.text)).map((m) => Number(m.text.slice(1)));
    check(numbered.length === 100 && numbered.every((n, i) => n === i), 'a hundred quick messages arrive, in order', numbered.length);

    console.log('Leaving');
    ruby.socket.close();
    await pause(200);
    check(host.events.some((e) => e.t === 'leave' && e.id === 3), 'a guest leaving is announced');
    const again = await player(`${base}/v1/join/${code}${q('Ruby')}`);
    check(again.event('welcome')?.id === 3, 'its seat is free again');
    host.socket.close();
    await pause(300);
    check(xiao.events.some((e) => e.t === 'closed') && xiao.closed !== null, 'the host leaving closes the room for the guests');
    const late = await player(`${base}/v1/join/${code}${q('Late')}`);
    check(late.event('error')?.reason.includes('no room'), 'and the code no longer joins', JSON.stringify(late.events));

    console.log('Refusals');
    const host2 = await player(`${base}/v1/host${q('Toan')}`);
    const other = await player(`${base}/v1/join/${host2.event('welcome').code}${q('Xiao', '2')}`);
    check(other.event('error')?.reason.includes('version'), 'another protocol version is refused', JSON.stringify(other.events));
    const nocode = await player(`${base}/v1/join/ZZZZZ${q('Xiao')}`);
    check(nocode.event('error')?.reason.includes('no room'), 'an unknown code is refused');
    const plain = await fetch(base.replace(/^ws/, 'http') + '/v1/host?game=chronicle&proto=1');
    check(plain.status === 426, 'a request that is not a WebSocket is refused', plain.status);
    for (const p of [xiao, goro, again, host2, other]) {
        p.socket.close();
    }
} finally {
    dev?.stop();
}
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
