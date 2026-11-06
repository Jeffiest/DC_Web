// The co-op relay on Cloudflare Workers (README.md): one Durable Object per room, each holding a Room (room.js),
// which the private server (server.mjs) shares, so the two speak the same protocol.
//
//   wss://<relay>/v1/host?game=chronicle&proto=1&name=Toan        opens a room; the welcome names its code
//   wss://<relay>/v1/join/<CODE>?game=chronicle&proto=1&name=Xiao joins one

import {Room, checkQuery, newCode, route} from './room.js';

function refuse(reason, status = 400) {
    return new Response(JSON.stringify({t: 'error', reason}), {status, headers: {'content-type': 'application/json'}});
}

function forward(env, code, request, role) {
    const room = env.ROOMS.get(env.ROOMS.idFromName(code));
    const url = new URL(request.url);
    url.searchParams.set('role', role);
    url.searchParams.set('code', code);
    return room.fetch(new Request(url, request));
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        if (url.pathname === '/' || url.pathname === '/health') {
            return new Response('chronicle co-op relay\n', {headers: {'content-type': 'text/plain'}});
        }
        if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') {
            return refuse('expected a WebSocket', 426);
        }
        const query = checkQuery(url.searchParams);
        if (query.error) {
            return refuse(query.error);
        }
        const path = route(url.pathname);
        if (!path) {
            return refuse('unknown path', 404);
        }
        if (path.role === 'join') {
            return forward(env, path.code, request, 'join');
        }
        // A fresh code; a room already open under it says so, and another is tried.
        for (let attempt = 0; attempt < 8; attempt++) {
            const response = await forward(env, newCode(), request, 'host');
            if (response.status !== 409) {
                return response;
            }
        }
        return refuse('no free room code; try again', 503);
    },
};

// One room's Durable Object.
export class RoomObject {
    constructor() {
        this.room = null;
    }

    async fetch(request) {
        const url = new URL(request.url);
        const code = url.searchParams.get('code');
        this.room ??= new Room(code);
        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair);
        server.accept();
        const seated = this.room.admit(url.searchParams.get('role'), checkQuery(url.searchParams), server);
        if (seated.taken) {
            server.close(4009, 'taken');
            return new Response('room code taken', {status: 409});
        }
        if (seated.id !== undefined) {
            server.addEventListener('message', (event) => this.room.receive(seated.id, event.data));
            const gone = () => this.room.remove(seated.id);
            server.addEventListener('close', gone);
            server.addEventListener('error', gone);
        }
        return new Response(null, {status: 101, webSocket: client});
    }
}
