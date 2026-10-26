// Cross-origin isolation for hosts that cannot send headers, GitHub Pages among them. The game's threads
// need SharedArrayBuffer, which a page has only when it is served with COOP and COEP; this worker adds
// them to every response it passes on. shell.js registers it and reloads once it controls the page.

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
    const request = event.request;
    if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') {
        return;
    }
    event.respondWith(fetch(request).then((response) => {
        if (response.status === 0) {
            return response;
        }
        const headers = new Headers(response.headers);
        headers.set('Cross-Origin-Opener-Policy', 'same-origin');
        headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
        headers.set('Cross-Origin-Resource-Policy', 'cross-origin');
        return new Response(response.body, {status: response.status, statusText: response.statusText, headers});
    }));
});
