# The co-op relay

Online play's meeting point, in two forms that speak the same protocol and share their room code (`src/room.js`):
the public relay, a [Cloudflare Worker](https://developers.cloudflare.com/workers/) with one Durable Object per room,
and a **private server** anyone can run (`server.mjs`, below). A player hosts a room and gets a five-letter code; up to three others join with it. The relay passes
messages between the players' games and keeps no game state: the host's game is the world, and the co-op mod decides
what the messages mean. The game reaches it through `net.*` (`port/include/moonmod/net_session.hpp`): the browser's
WebSocket on the web, WinHTTP on Windows, libcurl's TLS connection on Linux and macOS.

## Protocol (version 1 of the path)

    wss://<relay>/v1/host?game=chronicle&proto=<n>&name=<name>        opens a room
    wss://<relay>/v1/join/<CODE>?game=chronicle&proto=<n>&name=<name> joins one (the code in any case)

`proto` is the mod's message protocol number: a room takes only players on the same game and number. Names keep
letters, digits, spaces and `_ . -`, at most 24 characters; an empty one becomes "Player n".

The relay talks in **text frames** of JSON:

| `t` | fields | when |
| :-- | :-- | :-- |
| `welcome` | `id`, `code`, `host` (0), `peers` (`[{id, name}]`, the others already there) | on joining |
| `join` | `id`, `name` | another player joined |
| `leave` | `id` | another player left |
| `error` | `reason` | refused (no such room, full, another game or version); the socket then closes |
| `closed` | `reason` | the host left, which ends the room; the socket then closes |

Players talk in **binary frames**: a player sends `[to, ...payload]`, `to` a player's id (the host is 0, guests 1 to
3) or 255 for everyone else, and receives `[from, ...payload]`. A payload is at most 32 KiB (the game's `net.send`
takes 16 KiB of text); a player sending more than 240 frames in a second has the rest of that second's dropped. A
frame to oneself is dropped, which the game uses to keep a quiet socket open.

## Running your own server

`server.mjs` is the relay on a machine of your own: a home PC, a Raspberry Pi, a VPS. It needs Node.js 20 or later
(or Docker), and nothing else; rooms live in memory, and none of the game passes through it beyond the messages.

    cd coop/relay
    npm ci --omit=dev
    node server.mjs                        # ws://<this machine>:7777

or with Docker:

    docker build -t chronicle-coop-relay coop/relay
    docker run -d --restart unless-stopped -p 7777:7777 chronicle-coop-relay

Options (or the environment variable in brackets): `--port` (`PORT`, 7777), `--host` (`HOST`, 0.0.0.0), `--cert` and
`--key` (`TLS_CERT`, `TLS_KEY`: PEM files, to serve `wss://` itself), `--max-rooms` (`MAX_ROOMS`, 100), `--quiet`. It
logs rooms opening and closing and players joining and leaving; `/health` answers with the room count.

Then give the players the address. In the co-op mod's settings it is the server; a mod passes it as `relay`, or the
`DC_COOP_RELAY` environment variable sets it for every mod.

| Who plays | Address | What it takes |
| :-- | :-- | :-- |
| PCs on one network | `ws://192.168.1.20:7777` | the server machine's address; its firewall letting port 7777 in |
| PCs anywhere, privately | `ws://100.x.y.z:7777` | [Tailscale](https://tailscale.com) (or another VPN) on each PC and the server |
| PCs anywhere, open | `ws://your.host:7777` | the router forwarding 7777 to the server, or a VPS |
| Browser players too | `wss://your.host` | a certificate: `--cert`/`--key`, or [Caddy](https://caddyserver.com) in front (`reverse_proxy localhost:7777` gets one from Let's Encrypt), or a [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/) |

Browsers refuse `ws://` from the game's `https://` page, so a server browser players use needs `wss://` with a
certificate they trust (Let's Encrypt's is). The game checks a `wss://` server's certificate: a self-signed one is
refused unless the system trusts it. Plain `ws://` is not encrypted: fine on a home network or Tailscale (which
encrypts it itself), better avoided across the open internet.

## Running and testing

    npm ci
    npm run dev                      # the Worker on ws://127.0.0.1:8787, under wrangler dev
    npm start                        # the private server on ws://0.0.0.0:7777
    npm test                         # hosts, joins and talks against a local wrangler dev (or RELAY_URL)
    npm run test:server              # the same against the private server
    npm run test:native              # the game's Linux/macOS socket and session against it

## Deploying

`.github/workflows/coop-relay.yml` tests the relay, deploys it with `wrangler deploy` and tests the deployed one, on
every push to `main` that changes this folder (or run it by hand). It needs two repository secrets:

- `CLOUDFLARE_ACCOUNT_ID`: the account ID on the Cloudflare dashboard's Workers page.
- `CLOUDFLARE_API_TOKEN`: a token made from the "Edit Cloudflare Workers" template.

The run's summary gives the relay's address, `wss://chronicle-coop-relay.<your subdomain>.workers.dev`. The free plan
covers it: 100,000 requests a day, and Durable Objects on SQLite.
