// The browser smoke run (web workflow): the page from a plain static server, as GitHub Pages serves it, in
// headless Chromium. It waits for sw.js to isolate the page, stores the archive given as a URL in the origin
// private file system as the page would, starts the game and saves what it printed.
//
//   node browser_smoke.mjs <page url> <archive url> <log file> [game arguments...]

import {writeFile} from 'node:fs/promises';
import {chromium} from 'playwright';

const [pageUrl, archiveUrl, logFile, ...gameArgs] = process.argv.slice(2);
const lines = [];
const note = (text) => {
    lines.push(text);
    console.log(text);
};

const browser = await chromium.launch();
try {
    const page = await browser.newPage();
    page.on('console', (message) => note(`[${message.type()}] ${message.text()}`));
    page.on('pageerror', (error) => note(`[pageerror] ${error.message}`));
    const url = `${pageUrl}?args=${encodeURIComponent(gameArgs.join(' '))}`;

    await page.goto(url);
    await page.waitForFunction(() => self.crossOriginIsolated && !document.getElementById('pick').hidden
        || !document.getElementById('ready').hidden, null, {timeout: 60000});
    note(`isolated: ${await page.evaluate(() => self.crossOriginIsolated)}`);

    await page.evaluate(async (archive) => {
        const bytes = await (await fetch(archive)).arrayBuffer();
        const root = await navigator.storage.getDirectory();
        const dir = await root.getDirectoryHandle('chronicle', {create: true});
        const writable = await (await dir.getFileHandle('data.zip', {create: true})).createWritable();
        await writable.write(bytes);
        await writable.close();
    }, archiveUrl);
    await page.reload();
    await page.waitForSelector('#ready:not([hidden])', {timeout: 60000});
    note(`status: ${await page.textContent('#status')}`);

    await page.click('#start');
    try {
        await page.waitForFunction(() => /^(exit=|abort:)/m.test(document.getElementById('log').textContent),
            null, {timeout: 180000});
    } catch {
        note('timed out waiting for the game to exit');
    }
    note('--- page log ---');
    note(await page.textContent('#log'));
} finally {
    await browser.close();
    await writeFile(logFile, lines.join('\n') + '\n');
}
