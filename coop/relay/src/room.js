// A co-op room (README.md has the protocol), shared by the Cloudflare Worker (index.js) and the private server
// (server.mjs) so the two can never speak differently. A room holds its players' sockets by id: the host is 0, guests
// take the lowest free id from 1. A socket here is anything with send(data) and close(code, reason).

export const CODE_LETTERS = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
export const CODE_LENGTH = 5;
export const MAX_PLAYERS = 4;
export const MAX_PAYLOAD = 32 * 1024;
// A player sending more than this many messages in a second has the rest of that second's dropped.
export const MAX_RATE = 240;
const NAME_MAX = 24;
const BROADCAST = 255;

export function newCode() {
    const bytes = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
    return [...bytes].map((b) => CODE_LETTERS[b % CODE_LETTERS.length]).join('');
}

// The query a player connects with: the game and protocol, and a name.
export function checkQuery(params) {
    const game = params.get('game') ?? '';
    const proto = params.get('proto') ?? '';
    if (!/^[a-z0-9-]{1,32}$/.test(game) || !/^[0-9]{1,4}$/.test(proto)) {
        return {error: 'the game and protocol version are missing'};
    }
    return {game, proto, name: params.get('name') ?? ''};
}

// The path a player connects to: {role: 'host'} or {role: 'join', code}, or null.
export function route(pathname) {
    if (pathname === '/v1/host') {
        return {role: 'host'};
    }
    const join = pathname.match(/^\/v1\/join\/([A-Za-z0-9]{5})$/);
    return join ? {role: 'join', code: join[1].toUpperCase()} : null;
}

function cleanName(raw, id) {
    const name = (raw ?? '').replace(/[^\p{L}\p{N} _.-]/gu, '').trim().slice(0, NAME_MAX);
    return name || `Player ${id + 1}`;
}

export class Room {
    constructor(code) {
        this.code = code;
        this.players = new Map(); // id -> {socket, name, second, sent, reading}
        this.game = null;
        this.proto = null;
        this.onEmpty = null; // called when the last player has gone
    }

    get open() {
        return this.players.has(0);
    }

    // Seats a player, or says why not: {id}, or {error} (sent to the player, whose socket is then closed), or
    // {taken: true} for a host asking for a room someone already holds.
    admit(role, {game, proto, name}, socket) {
        if (role === 'host') {
            if (this.players.size > 0) {
                return {taken: true};
            }
            this.game = game;
            this.proto = proto;
            this.seat(0, socket, name);
            return {id: 0};
        }
        let error = null;
        if (!this.open) {
            error = 'no room with that code';
        } else if (game !== this.game) {
            error = `that room is playing ${this.game}`;
        } else if (proto !== this.proto) {
            error = 'that room is on another version of the game; update both to the same build';
        }
        let id = 1;
        while (this.players.has(id)) {
            id++;
        }
        if (!error && id >= MAX_PLAYERS) {
            error = 'that room is full';
        }
        if (error) {
            try {
                socket.send(JSON.stringify({t: 'error', reason: error}));
                socket.close(4000, error.slice(0, 120));
            } catch {
                // already gone
            }
            return {error};
        }
        this.seat(id, socket, name);
        return {id};
    }

    seat(id, socket, rawName) {
        const name = cleanName(rawName, id);
        const peers = [...this.players].map(([peer, player]) => ({id: peer, name: player.name}));
        this.players.set(id, {socket, name, second: 0, sent: 0, reading: Promise.resolve()});
        socket.send(JSON.stringify({t: 'welcome', id, code: this.code, host: 0, peers}));
        this.broadcast(JSON.stringify({t: 'join', id, name}), id);
    }

    // A frame from a player: text means nothing here (players talk in binary). A binary frame comes as an
    // ArrayBuffer, a typed array or Node's Buffer, or in some runtimes a Blob, which reads asynchronously: each
    // player's frames are read one after another, so none overtakes the one before it.
    receive(from, data, binary = true) {
        const player = this.players.get(from);
        if (!player || !binary || typeof data === 'string') {
            return;
        }
        if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
            player.reading = player.reading.then(() => this.route(from, data));
        } else if (typeof data?.arrayBuffer === 'function') {
            if (data.size - 1 > MAX_PAYLOAD) {
                return;
            }
            player.reading = player.reading.then(() => data.arrayBuffer()).then((buffer) => this.route(from, buffer));
        }
        player.reading = player.reading.catch(() => {});
    }

    route(from, data) {
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
        if (!this.players.delete(id)) {
            return;
        }
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
        } else {
            this.broadcast(JSON.stringify({t: 'leave', id}), id);
        }
        if (this.players.size === 0) {
            this.onEmpty?.();
        }
    }
}
