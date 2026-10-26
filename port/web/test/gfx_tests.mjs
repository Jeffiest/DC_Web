// Runs Kazarov's renderer tests (gfx_tests.html) in headless Chromium on SwiftShader's Vulkan, so WebGPU works
// without a GPU, and saves what they print.
//
//   node gfx_tests.mjs <page url> <log file> [GoogleTest arguments...]

import {writeFile} from 'node:fs/promises';
import {chromium} from 'playwright';

const [pageUrl, logFile, ...testArgs] = process.argv.slice(2);
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
    page.on('console', (message) => note(message.text()));
    page.on('pageerror', (error) => note(`[pageerror] ${error.message}`));
    const query = testArgs.length > 0 ? `?args=${encodeURIComponent(testArgs.join(' '))}` : '';
    await page.goto(pageUrl + query);
    try {
        await page.waitForFunction(() => /^(exit=|abort:|tests: isolation)/m.test(document.getElementById('log').textContent),
            null, {timeout: 840000});
    } catch {
        note('timed out waiting for the tests');
    }
} finally {
    await browser.close();
    await writeFile(logFile, lines.join('\n') + '\n');
}
