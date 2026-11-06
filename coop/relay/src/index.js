// The co-op relay (coop/relay/README.md): rooms of up to four players, joined by a short code, that pass messages
// between the players' games. It keeps no game state: the host's game is the world, and the relay only forwards.
//
//   wss://<relay>/v1/host?game=chronicle&proto=1&name=Toan        opens a room; the welcome names its code
//   wss://<relay>/v1/join/<CODE>?game=chronicle&proto=1&name=Xiao joins one
//
// The server talks in text frames of JSON: welcome {id, code, host, peers: [{id, name}]}, join {id, name}, leave {id},
// error {reason} and closed {reason} (both followed by the socket closing). Players talk in binary frames: a player
// sends [to, ...payload], to being a peer id (0 is the host) or 255 for everyone else, and receives [from, ...payload].

const CODE_LETTERS = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
const CODE_LENGTH = 5;
const MAX_PLAYERS = 4;
const MAX_PAYLOAD = 32 * 1024;
// A player sending more than this many messages in a second has the rest of that second's dropped.
const MAX_RATE = 240;
const NAME_MAX = 24;
const BROADCAST = 255;

function newCode() {
    const bytes = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
    return [...bytes].map((b) => CODE_LETTERS[b % CODE_LETTERS.length]).join('');
}

function cleanName(raw, id) {
    const name = (raw ?? '').replace(/[^\p{L}\p{N} _.-]/gu, '').trim().slice(0, NAME_MAX);
    return name || `Player ${id + 1}`;
}

function refuse(reason, status = 400) {
    return new Response(JSON.stringify({t: 'error', reason}), {status, headers: {'content-type': 'application/json'}});
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
        const game = url.searchParams.get('game') ?? '';
        const proto = url.searchParams.get('proto') ?? '';
        if (!/^[a-z0-9-]{1,32}$/.test(game) || !/^[0-9]{1,4}$/.test(proto)) {
            return refuse('the game and protocol version are missing');
        }
        if (url.pathname === '/v1/host') {
            // A fresh code; a room already open under it says so, and another is tried.
            for (let attempt = 0; attempt < 8; attempt++) {
                const code = newCode();
                const room = env.ROOMS.get(env.ROOMS.idFromName(code));
                const forwarded = new URL(request.url);
                forwarded.searchParams.set('role', 'host');
                forwarded.searchParams.set('code', code);
                const response = await room.fetch(new Request(forwarded, request));
                if (response.status !== 409) {
                    return response;
                }
            }
            return refuse('no free room code; try again', 503);
        }
        const join = url.pathname.match(/^\/v1\/join\/([A-Za-z0-9]{5})$/);
        if (join) {
            const code = join[1].toUpperCase();
            const room = env.ROOMS.get(env.ROOMS.idFromName(code));
            const forwarded = new URL(request.url);
            forwarded.searchParams.set('role', 'join');
            forwarded.searchParams.set('code', code);
            return room.fetch(new Request(forwarded, request));
        }
        return refuse('unknown path', 404);
    },
};

// One room: its players' sockets, by id. The host is id 0; guests take the lowest free id from 1.
export class Room {
    constructor(state) {
        this.state = state;
        this.players = new Map(); // id -> {socket, name, second, sent}
        this.code = null;
        this.game = null;
        this.proto = null;
    }

    async fetch(request) {
        const url = new URL(request.url);
        const role = url.searchParams.get('role');
        const game = url.searchParams.get('game');
        const proto = url.searchParams.get('proto');
        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair);
        server.accept();
        const fail = (reason) => {
            server.send(JSON.stringify({t: 'error', reason}));
            server.close(4000, reason.slice(0, 120));
            return new Response(null, {status: 101, webSocket: client});
        };

        if (role === 'host') {
            if (this.players.size > 0) {
                server.close(4009, 'taken');
                return new Response('room code taken', {status: 409});
            }
            this.code = url.searchParams.get('code');
            this.game = game;
            this.proto = proto;
            this.add(0, server, url.searchParams.get('name'));
            return new Response(null, {status: 101, webSocket: client});
        }

        if (!this.players.has(0)) {
            return fail('no room with that code');
        }
        if (game !== this.game) {
            return fail(`that room is playing ${this.game}`);
        }
        if (proto !== this.proto) {
            return fail('that room is on another version of the game; update both to the same build');
        }
        let id = 1;
        while (this.players.has(id)) {
            id++;
        }
        if (id >= MAX_PLAYERS) {
            return fail('that room is full');
        }
        this.add(id, server, url.searchParams.get('name'));
        return new Response(null, {status: 101, webSocket: client});
    }

    add(id, socket, rawName) {
        const name = cleanName(rawName, id);
        const peers = [...this.players].map(([peer, player]) => ({id: peer, name: player.name}));
        this.players.set(id, {socket, name, second: 0, sent: 0, reading: Promise.resolve()});
        socket.send(JSON.stringify({t: 'welcome', id, code: this.code, host: 0, peers}));
        this.broadcast(JSON.stringify({t: 'join', id, name}), id);
        socket.addEventListener('message', (event) => this.receive(id, event.data));
        const gone = () => this.remove(id);
        socket.addEventListener('close', gone);
        socket.addEventListener('error', gone);
    }

    // A binary frame comes as an ArrayBuffer or, in some runtimes, a Blob, which reads asynchronously: each player's
    // frames are read one after another, so none overtakes the one before it.
    receive(from, data) {
        const player = this.players.get(from);
        if (!player || typeof data === 'string') {
            return; // players talk in binary; text from a player means nothing here
        }
        if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
            player.reading = player.reading.then(() => this.message(from, data));
        } else if (typeof data?.arrayBuffer === 'function') {
            if (data.size - 1 > MAX_PAYLOAD) {
                return;
            }
            player.reading = player.reading.then(() => data.arrayBuffer()).then((buffer) => this.message(from, buffer));
        }
        player.reading = player.reading.catch(() => {});
    }

    message(from, data) {
        const player = this.players.get(from);
        if (!player) {
            return;
        }
        const bytes = ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : new Uint8Array(data);
        if (bytes.length < 1 || bytes.length - 1 > MAX_PAYLOAD) {
            return;
        }
        const second = Math.floor(Date.now() / 1000);
        if (player.second !== second) {
            player.second = second;
            player.sent = 0;
        }
        if (++player.sent > MAX_RATE) {
            return;
        }
        const to = bytes[0];
        const out = new Uint8Array(bytes.length);
        out.set(bytes);
        out[0] = from;
        if (to === BROADCAST) {
            this.broadcast(out, from);
        } else if (to !== from) {
            this.send(to, out);
        }
    }

    send(id, data) {
        const player = this.players.get(id);
        if (!player) {
            return;
        }
        try {
            player.socket.send(data);
        } catch {
            this.remove(id);
        }
    }

    broadcast(data, except) {
        for (const id of [...this.players.keys()]) {
            if (id !== except) {
                this.send(id, data);
            }
        }
    }

    remove(id) {
        const player = this.players.get(id);
        if (!player) {
            return;
        }
        this.players.delete(id);
        if (id === 0) {
            // The host's game was the world: without it the room ends.
            for (const [, guest] of this.players) {
                try {
                    guest.socket.send(JSON.stringify({t: 'closed', reason: 'the host left'}));
                    guest.socket.close(4001, 'the host left');
                } catch {
                    // already gone
                }
            }
            this.players.clear();
            this.code = null;
            return;
        }
        this.broadcast(JSON.stringify({t: 'leave', id}), id);
    }
}
