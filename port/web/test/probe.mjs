// Runs the WebGPU probe page (port/web/probe) in headless Chromium on SwiftShader's Vulkan, so WebGPU works
// without a GPU, and saves its output and screenshots of the canvas while it draws and after.
//
//   node probe.mjs <page url> <log file> <screenshot prefix>

import {writeFile} from 'node:fs/promises';
import {chromium} from 'playwright';

const [pageUrl, logFile, shotPrefix] = process.argv.slice(2);
const lines = [];
const note = (text) => {
    lines.push(text);
    console.log(text);
};

const browser = await chromium.launch({
    args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
        '--use-webgpu-adapter=swiftshader', '--disable-vulkan-surface'],
});
try {
    const page = await browser.newPage();
    page.on('console', (message) => note(`[${message.type()}] ${message.text()}`));
    page.on('pageerror', (error) => note(`[pageerror] ${error.message}`));
    await page.goto(pageUrl);
    const finished = () => /^(probe: done|exit=|abort:|probe: isolation)/m.test(document.getElementById('log').textContent);
    try {
        await page.waitForFunction(() => /probe: frame|probe: readback/.test(document.getElementById('log').textContent),
            null, {timeout: 60000});
        await page.waitForTimeout(500);
        await page.locator('#canvas').screenshot({path: `${shotPrefix}-during.png`});
        await page.waitForFunction(finished, null, {timeout: 120000});
    } catch {
        note('timed out waiting for the probe');
    }
    await page.locator('#canvas').screenshot({path: `${shotPrefix}-after.png`});
} finally {
    await browser.close();
    await writeFile(logFile, lines.join('\n') + '\n');
}
