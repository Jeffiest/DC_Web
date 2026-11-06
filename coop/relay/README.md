# The co-op relay

Online play's meeting point: a [Cloudflare Worker](https://developers.cloudflare.com/workers/) with one Durable Object
per room. A player hosts a room and gets a five-letter code; up to three others join with it. The relay passes
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

## Running and testing

    npm ci
    npm run dev                      # the relay on ws://127.0.0.1:8787
    npm test                         # hosts, joins and talks against a local wrangler dev (or RELAY_URL)
    npm run test:native              # the game's Linux/macOS socket and session against it

## Deploying

`.github/workflows/coop-relay.yml` tests the relay, deploys it with `wrangler deploy` and tests the deployed one, on
every push to `main` that changes this folder (or run it by hand). It needs two repository secrets:

- `CLOUDFLARE_ACCOUNT_ID`: the account ID on the Cloudflare dashboard's Workers page.
- `CLOUDFLARE_API_TOKEN`: a token made from the "Edit Cloudflare Workers" template.

The run's summary gives the relay's address, `wss://chronicle-coop-relay.<your subdomain>.workers.dev`. The free plan
covers it: 100,000 requests a day, and Durable Objects on SQLite.
