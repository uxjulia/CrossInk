// Build first: python3 scripts/build_web.py
// Run with Playwright installed: node --test scripts/test_web_feedback.cjs
// Tests the actual generated, minified portal in Chromium; APIs are mocked.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync, mkdirSync } = require('node:fs');
const { join } = require('node:path');
const { gunzipSync } = require('node:zlib');
const { chromium } = require('playwright');
const root = join(__dirname, '..');
function asset(name) {
  const header = readFileSync(join(root, 'src/network/html', name + '.generated.h'), 'utf8');
  return gunzipSync(Buffer.from(header.match(/0x[0-9a-f]{2}/gi).map(x => parseInt(x, 16))));
}
for (const viewport of [{ width: 1280, height: 900 }, { width: 390, height: 844 }, { width: 320, height: 568 }]) {
  test(`generated portal feedback and interruption at ${viewport.width}px`, { timeout: 60000 }, async () => {
    const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH });
    try {
      const page = await browser.newPage({ viewport, isMobile: viewport.width < 600, hasTouch: viewport.width < 600 });
      page.setDefaultTimeout(10000);
      const errors = [];
      page.on('pageerror', e => errors.push(e.message));
      let outcome = 'success', calls = 0, release;
      page.on('dialog', dialog => { errors.push('Unexpected native dialog'); dialog.dismiss(); });
      await page.route('http://reader.test/**', async route => {
        const path = new URL(route.request().url()).pathname;
        const names = { '/files': 'FilesPageHtml', '/settings': 'SettingsPageHtml', '/fonts': 'FontsPageHtml', '/style.css': 'StyleCss' };
        if (names[path]) return route.fulfill({ contentType: path.endsWith('.css') ? 'text/css' : 'text/html', body: asset(names[path]) });
        if (path === '/api/fonts') return route.fulfill({ json: { families: Array.from({ length: 25 }, (_, i) => ({ name: 'Family_' + i, sizes: [12], files: [] })) } });
        if (path === '/api/fonts/delete' || path === '/api/fonts/upload') {
          calls++;
          if (outcome === 'pending') await new Promise(resolve => { release = resolve; });
          if (outcome === 'network') return route.abort();
          return route.fulfill({ status: outcome === 'failure' ? 500 : 200, json: { ok: outcome !== 'failure', error: 'SD full' } });
        }
        if (path === '/plugin-host.js') return route.fulfill({ body: 'function loadPlugins() {}', contentType: 'application/javascript' });
        if (path.startsWith('/api/')) return route.fulfill({ json: path === '/api/status' ? {} : path === '/api/files' ? [{ name: 'Books', isDirectory: true, size: 0 }] : [] });
        return route.fulfill({ body: '', contentType: 'application/javascript' });
      });
      await page.goto('http://reader.test/fonts');
      await page.waitForFunction(() => document.querySelectorAll('.family').length === 25);
      const feedback = page.locator('#status');
      async function visible(message) {
        await page.waitForFunction(text => document.getElementById('status').textContent.includes(text), message);
        assert.ok(await feedback.isVisible());
        const b = await feedback.boundingBox();
        assert.ok(b.x >= 0 && b.y >= 0 && b.x + b.width <= viewport.width + 1 && b.y + b.height <= viewport.height + 1, JSON.stringify(b));
      }
      await page.locator('.family button').first().click();
      await page.locator('#webDialogCancel').click();
      await page.waitForTimeout(30);
      assert.equal(calls, 0, 'cancelled deletion must not call API');
      await page.locator('.family button').first().click();
      await page.locator('#webDialogConfirm').click();
      await visible('Deleted "Family_0"');
      outcome = 'failure';
      await page.locator('.family button').last().click();
      await page.locator('#webDialogConfirm').click();
      await visible('Failed to delete');
      outcome = 'network';
      await page.locator('.family button').last().click();
      await page.locator('#webDialogConfirm').click();
      await visible('Delete error:');
      async function submit(names) {
        await page.evaluate(names => {
          const transfer = new DataTransfer();
          names.forEach(name => transfer.items.add(new File(['font'], name)));
          document.getElementById('fontFiles').files = transfer.files;
          document.getElementById('uploadForm').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
        }, names);
      }
      await submit([]);
      await visible('No supported font files');
      await submit(['Alpha_12.cpfont', 'Beta_12.cpfont']);
      await visible('single font family');
      outcome = 'pending';
      await submit(['Alpha_12.cpfont']);
      await visible('Uploading 1/1');
      await page.waitForFunction(() => document.getElementById('status').textContent.startsWith('Uploading'));
      while (!release) await page.waitForTimeout(10);
      await page.clock.install();
      await page.clock.fastForward(10000);
      await visible('Uploading 1/1');
      await page.getByRole('button', { name: 'Dismiss font notification' }).click();
      assert.equal(await feedback.isVisible(), false);
      outcome = 'success'; release();
      await visible('Uploaded 1 file');
      outcome = 'failure';
      await submit(['Alpha_12.cpfont']);
      await visible('Failed on Alpha_12.cpfont: SD full');
      outcome = 'network';
      await submit(['Alpha_12.cpfont']);
      await visible('Upload error on');
      outcome = 'success';
      await submit(['Alpha_12.cpfont', 'Alpha_14.cpfont']);
      await visible('Uploaded 2 files');
      await page.evaluate(() => scrollTo(0, document.body.scrollHeight));
      await visible('Uploaded 2 files');
      assert.equal(await feedback.getAttribute('role'), 'status');
      const screenshots = process.env.WEB_SCREENSHOTS;
      if (screenshots) {
        mkdirSync(screenshots, { recursive: true });
        await page.screenshot({ path: join(screenshots, `fonts-${viewport.width}.png`) });
      }
      const successColor = await feedback.evaluate(e => getComputedStyle(e).color);
      outcome = 'failure';
      await submit(['LongFontName'.repeat(16) + '_12.cpfont']);
      await visible('Failed on');
      assert.notEqual(await feedback.evaluate(e => getComputedStyle(e).color), successColor);
      await page.emulateMedia({ colorScheme: 'dark' });
      await visible('Failed on');
      if (screenshots) await page.screenshot({ path: join(screenshots, `fonts-dark-error-${viewport.width}.png`) });
      await page.getByRole('button', { name: 'Dismiss font notification' }).click();
      assert.equal(await feedback.isVisible(), false);
      await page.goto('http://reader.test/settings');
      await page.evaluate(() => {
        window.dialogResults = [];
        showWebConfirm('<img src=x onerror=alert(1)>').then(value => dialogResults.push(value));
        showWebAlert('Second message').then(() => dialogResults.push('closed'));
      });
      await page.waitForSelector('#webMessageDialog[open]');
      assert.equal(await page.locator('#webDialogMessage').textContent(), '<img src=x onerror=alert(1)>');
      assert.equal(await page.locator('#webDialogMessage img').count(), 0);
      assert.equal(await page.evaluate(() => document.activeElement.id), 'webDialogCancel');
      if (screenshots) await page.screenshot({ path: join(screenshots, `confirmation-${viewport.width}.png`) });
      await page.keyboard.press('Escape');
      await page.waitForFunction(() => document.getElementById('webDialogMessage').textContent === 'Second message');
      assert.equal(await page.locator('#webDialogCancel').isVisible(), false);
      await page.locator('#webDialogConfirm').click();
      await page.waitForFunction(() => dialogResults.length === 2);
      assert.deepEqual(await page.evaluate(() => dialogResults), [false, 'closed']);
      console.log(viewport.width, 'settings timers');
      await page.evaluate(() => showMessage('first', false));
      await page.clock.fastForward(3900);
      await page.evaluate(() => showMessage('second', true));
      await page.clock.fastForward(200);
      assert.equal(await page.locator('#message').isVisible(), true);
      assert.equal(await page.locator('#message').textContent(), 'second');
      await page.clock.fastForward(3800);
      assert.equal(await page.locator('#message').isVisible(), false);
      await page.goto('http://reader.test/files');
      console.log(viewport.width, 'files timers');
      await page.waitForFunction(() => document.querySelector('#file-table table'));
      await page.evaluate(() => showNotification('first'));
      await page.clock.fastForward(4900);
      await page.evaluate(() => showNotification('second'));
      await page.clock.fastForward(400);
      assert.equal(await page.locator('#notification').evaluate(e => e.style.opacity), '1');
      await page.clock.fastForward(4600); // second has now begun fading
      await page.evaluate(() => showNotification('during fade', 'success'));
      await page.clock.fastForward(400);
      assert.equal(await page.locator('#notification').textContent(), 'during fade');
      assert.equal(await page.locator('#notification').evaluate(e => e.style.opacity), '1');
      await page.clock.fastForward(4900);
      await page.clock.fastForward(300);
      assert.equal(await page.locator('#notification').count(), 0);
      await page.evaluate(() => openFolderModal());
      const modal = page.locator('#folderModal .modal');
      const box = await modal.boundingBox();
      await page.mouse.move(box.x + 20, box.y + 20); await page.mouse.down();
      await page.mouse.move(2, 2); await page.mouse.up();
      assert.ok(await modal.isVisible(), 'drag from dialog must not dismiss');
      await page.mouse.click(2, 2);
      assert.equal(await modal.isVisible(), false);
      await page.evaluate(() => openUploadModal());
      await page.evaluate(() => {
        isUploadInProgress = true; window.aborts = 0;
        currentUploadXhr = { abort() { window.aborts++; } };
      });
      await page.mouse.click(2, 2);
      assert.ok(await page.locator('#uploadModal').isVisible());
      assert.equal(await page.evaluate(() => window.aborts), 0);
      await page.keyboard.press('Escape');
      assert.equal(await page.evaluate(() => window.aborts), 1);
      assert.equal(await page.evaluate(() => operationCancelled), true);
      await page.evaluate(() => { isUploadInProgress = false; closeUploadModal(); openUploadModal(); });
      await page.keyboard.press('Escape');
      assert.equal(await page.locator('#uploadModal').isVisible(), false);
      assert.deepEqual(errors, []);
    } finally { await browser.close(); }
  });
}
