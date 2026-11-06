#!/usr/bin/env node
// The co-op relay as a private server (README.md, "Running your own server"): the same rooms and protocol as the
// Cloudflare Worker (src/room.js is shared), on a machine of your own. Node 20 or later.
//
//   node server.mjs                                   ws://<this machine>:7777
//   node server.mjs --port 443 --cert cert.pem --key key.pem   wss:// with your TLS certificate
//
// Every option has an environment variable too: PORT, HOST, TLS_CERT, TLS_KEY, MAX_ROOMS.

import {readFileSync} from 'node:fs';
import {createServer as createHttp} from 'node:http';
import {createServer as createHttps} from 'node:https';
import {parseArgs} from 'node:util';
import {WebSocketServer} from 'ws';
import {Room, checkQuery, newCode, route} from './src/room.js';

const {values: options} = parseArgs({
    options: {
        port: {type: 'string', default: process.env.PORT ?? '7777'},
        host: {type: 'string', default: process.env.HOST ?? '0.0.0.0'},
        cert: {type: 'string', default: process.env.TLS_CERT},
        key: {type: 'string', default: process.env.TLS_KEY},
        'max-rooms': {type: 'string', default: process.env.MAX_ROOMS ?? '100'},
        quiet: {type: 'boolean', default: false},
        help: {type: 'boolean', short: 'h', default: false},
    },
});
if (options.help) {
    console.log(`Chronicle co-op relay (private server)

  node server.mjs [--port 7777] [--host 0.0.0.0] [--cert cert.pem --key key.pem] [--max-rooms 100] [--quiet]

Without --cert/--key it serves ws:// (PC players on your network or a VPN such as Tailscale). Browser players need
wss://: give a certificate here, or put the server behind a proxy that has one (Caddy, nginx, Cloudflare Tunnel).`);
    process.exit(0);
}
const port = Number(options.port);
const maxRooms = Number(options['max-rooms']);
if ((options.cert && !options.key) || (!options.cert && options.key)) {
    console.error('--cert and --key go together');
    process.exit(2);
}
const log = (...parts) => {
    if (!options.quiet) {
        console.log(new Date().toISOString(), ...parts);
    }
};

const rooms = new Map(); // code -> Room
const secure = Boolean(options.cert);
const server = secure ?
    createHttps({cert: readFileSync(options.cert), key: readFileSync(options.key)}) :
    createHttp();
const sockets = new WebSocketServer({noServer: true, maxPayload: 64 * 1024});
// A socket nobody answers on is closed after a minute: each is pinged every 30 seconds (pinger, below).
const alive = new WeakSet();

server.on('request', (request, response) => {
    const {pathname} = new URL(request.url, 'http://relay');
    if (pathname === '/' || pathname === '/health') {
        response.writeHead(200, {'content-type': 'text/plain'});
        response.end(`chronicle co-op relay (private), ${rooms.size} room(s)\n`);
        return;
    }
    response.writeHead(426, {'content-type': 'application/json'});
    response.end(JSON.stringify({t: 'error', reason: 'expected a WebSocket'}));
});

server.on('upgrade', (request, socket, head) => {
    const url = new URL(request.url, 'http://relay');
    const query = checkQuery(url.searchParams);
    const path = route(url.pathname);
    if (query.error || !path) {
        socket.end(`HTTP/1.1 ${path ? 400 : 404} Bad Request\r\nConnection: close\r\n\r\n`);
        return;
    }
    sockets.handleUpgrade(request, socket, head, (ws) => {
        alive.add(ws);
        ws.on('pong', () => alive.add(ws));
        let room;
        if (path.role === 'host') {
            if (rooms.size >= maxRooms) {
                ws.send(JSON.stringify({t: 'error', reason: 'this server is full; try again later'}));
                ws.close(4000, 'server full');
                return;
            }
            let code = newCode();
            while (rooms.has(code)) {
                code = newCode();
            }
            room = new Room(code);
            room.onEmpty = () => {
                rooms.delete(code);
                log(`room ${code} closed`);
            };
            rooms.set(code, room);
        } else {
            // An unknown code gets a room that refuses it, as the Worker's empty Durable Object does.
            room = rooms.get(path.code) ?? new Room(path.code);
        }
        const seated = room.admit(path.role, query, ws);
        if (seated.id === undefined) {
            return;
        }
        log(path.role === 'host' ? `room ${room.code} opened` : `room ${room.code}: player ${seated.id} joined`);
        ws.on('message', (data, binary) => room.receive(seated.id, data, binary));
        ws.on('close', () => {
            room.remove(seated.id);
            if (seated.id !== 0 && room.open) {
                log(`room ${room.code}: player ${seated.id} left`);
            }
        });
        ws.on('error', () => room.remove(seated.id));
    });
});

const pinger = setInterval(() => {
    for (const ws of sockets.clients) {
        if (!alive.has(ws)) {
            ws.terminate();
            continue;
        }
        alive.delete(ws);
        ws.ping();
    }
}, 30000);

server.listen(port, options.host, () => {
    log(`co-op relay listening on ${secure ? 'wss' : 'ws'}://${options.host}:${port}`);
});
for (const signal of ['SIGINT', 'SIGTERM']) {
    process.on(signal, () => {
        clearInterval(pinger);
        for (const ws of sockets.clients) {
            ws.close(1001, 'the server is stopping');
        }
        server.close(() => process.exit(0));
        setTimeout(() => process.exit(0), 2000).unref();
    });
}
