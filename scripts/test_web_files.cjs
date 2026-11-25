// Run with: node --test scripts/test_web_files.cjs
// FILES_PAGE_SCRIPT may point at the script extracted from the generated page.
// Browser APIs are faked at the boundary; the complete portal script and upload
// orchestration run unchanged. This does not exercise device storage or layout.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { File } = require('node:buffer');
const vm = require('node:vm');
const { test } = require('node:test');

const source = readFileSync(process.env.FILES_PAGE_SCRIPT || join(__dirname, '../web/pages/files.js'), 'utf8');

class Element {
  constructor(id) {
    this.id = id;
    this.listeners = new Map();
    const classes = new Set();
    this.classList = {
      add: (...names) => names.forEach(name => classes.add(name)),
      remove: (...names) => names.forEach(name => classes.delete(name)),
      contains: name => classes.has(name),
      toggle: (name, enabled = !classes.has(name)) => enabled ? classes.add(name) : classes.delete(name),
    };
    this.style = {};
    this.dataset = {};
    this.value = '';
    this.textContent = '';
    this.files = [];
    this.checked = false;
  }
  addEventListener(type, callback) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(callback);
  }
  emit(type, target = this, extra = {}) {
    const event = { target, preventDefault() {}, ...extra };
    for (const callback of this.listeners.get(type) || []) callback(event);
  }
  removeAttribute(name) { delete this[name]; }
  showModal() { this.open = true; }
  close() { this.open = false; }
  focus() {}
  select() {}
  setSelectionRange() {}
  querySelectorAll() { return []; }
}

function portal({ entries = [], fallback = false } = {}) {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) elements.set(id, new Element(id));
    return elements.get(id);
  };
  const overlays = ['uploadModal', 'folderModal', 'deleteModal', 'renameModal', 'moveModal', 'imagePreviewModal'].map(element);
  const document = new Element('document');
  document.getElementById = element;
  document.querySelectorAll = selector => ['.modal-overlay', 'dialog.modal-overlay'].includes(selector) ? overlays : [];
  document.querySelector = selector => selector === '.modal-overlay.open'
    ? overlays.find(overlay => overlay.open)
    : element(selector);
  const uploads = [];
  const requests = [];
  const logs = [];
  const storage = new Map();
  class WebSocket {
    static OPEN = 1;
    constructor(url) {
      this.url = url;
      this.readyState = 1;
      this.bufferedAmount = 0;
      queueMicrotask(() => fallback ? this.onerror({}) : this.onopen());
    }
    send(message) {
      if (typeof message === 'string') {
        const [, name, size, path] = message.split(':');
        this.upload = { transport: 'ws', name, size: Number(size), path, chunks: [] };
        uploads.push(this.upload);
        queueMicrotask(() => this.onmessage({ data: 'READY' }));
      } else {
        this.upload.chunks.push(Buffer.from(message));
        if (Buffer.concat(this.upload.chunks).length === this.upload.size) {
          queueMicrotask(() => this.onmessage({ data: 'DONE' }));
        }
      }
    }
    close() { this.readyState = 3; this.onclose({ code: 1000, reason: '' }); }
  }
  class XMLHttpRequest {
    constructor() { this.upload = {}; }
    open(method, url) { this.method = method; this.url = url; }
    send(form) {
      const file = form.get('file');
      uploads.push({ transport: 'http', file, name: file.name, url: this.url });
      this.status = 200;
      queueMicrotask(() => this.onload());
    }
    abort() { this.onabort(); }
  }
  const context = vm.createContext({
    document, navigator: { onLine: true },
    window: { location: { search: '?path=%2Fbooks', hostname: 'reader.test', port: '', reload() {} }, addEventListener() {} },
    URLSearchParams, File, Blob, FormData, WebSocket, XMLHttpRequest,
    console: { log: (...args) => logs.push(args.join(' ')), warn() {}, error() {} },
    showWebAlert: message => { throw new Error(message); },
    localStorage: { getItem: key => storage.get(key), setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
    fetch: async url => {
      requests.push(url);
      return { ok: true, json: async () => url.startsWith('/api/files') ? entries : {} };
    },
    // Complete short transfer delays; do not execute the post-upload reload.
    setTimeout: (callback, delay) => { if (delay < 1000) queueMicrotask(callback); return 1; },
    clearTimeout() {},
  });
  // The test controls when hydration occurs, after installing its fixtures.
  assert.match(source, /hydrate\(\);?\s*(?:loadPlugins\("files"\);)?\s*$/);
  vm.runInContext(source.replace(/hydrate\(\);?\s*(?:loadPlugins\("files"\);)?\s*$/, ''), context);
  return { context, element, overlays, document, uploads, requests, logs, storage,
    run: code => vm.runInContext(code, context) };
}

async function finishUpload(page) {
  await page.context.uploadFile();
  for (let i = 0; i < 100; i++) {
    if (page.element('progress-text').textContent === 'All uploads complete!') return;
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.fail(`Upload did not finish: ${page.element('progress-text').textContent}`);
}

test('canonical and case-insensitive collisions preserve numbered suffixes and source entries', async () => {
  const entries = [{ name: 'CAFE\u0301.EPUB' }, { name: 'Café (2).epub' }];
  const page = portal({ entries });
  const used = await page.context.fetchExistingUploadNames();
  assert.equal(page.context.reserveAvailableUploadFilename('Cafe\u0301.epub', used), 'Café (3).epub');
  assert.equal(page.context.reserveAvailableUploadFilename('Café.epub', used), 'Café (4).epub');
  assert.equal(page.context.reserveAvailableUploadFilename('Re\u0301sume\u0301', used), 'Résumé');
  assert.equal(page.context.reserveAvailableUploadFilename('Résumé', used), 'Résumé (2)');
  assert.equal(page.context.reserveAvailableUploadFilename('.cafe\u0301', used), '.café');
  assert.equal(page.context.reserveAvailableUploadFilename('.café', used), '.café (2)');
  assert.deepEqual(entries, [{ name: 'CAFE\u0301.EPUB' }, { name: 'Café (2).epub' }]);
  assert.equal(page.requests.length, 1);
  assert.match(page.requests[0], /^\/api\/files\?path=%2Fbooks/);
  assert.ok(page.logs.some(line => line.includes('Normalized filename to NFC:')));
  assert.ok(page.logs.some(line => line.includes('Renamed to avoid collision:')));
});

test('metadata sanitation still strips illegal characters and preserves filename limits', () => {
  const { context } = portal();
  assert.equal(context.buildMetadataFilename('.. Cafe\u0301: / <Guide>?*\u0000. ', ' A\\B|C" '), 'Café Guide - A B C.epub');
  assert.equal(context.buildMetadataFilename('... ', 'Author'), '');
  assert.equal(context.buildMetadataFilename('x'.repeat(200), '').length, 185);
});

for (const fallback of [false, true]) {
  for (const rename of [false, true]) {
    for (const convert of [false, true]) {
      test(`${fallback ? 'HTTP fallback' : 'WebSocket'}: metadata ${rename ? 'on' : 'off'}, conversion ${convert ? 'on' : 'off'}`, async () => {
        const existing = rename ? 'CAFE\u0301 Guide - Jose\u0301.epub' : 'CAFE\u0301.epub';
        const page = portal({ entries: [{ name: existing }], fallback });
        const inputFiles = ['Cafe\u0301.epub', 'Café.epub'].map(name => new File(['original book'], name,
          { type: 'application/epub+zip', lastModified: 123456 }));
        page.element('fileInput').files = inputFiles;
        page.element('renameFromMetadataToggle').checked = rename;
        page.element('convertBeforeUpload').checked = convert;
        page.storage.set('saved-settings', 'preserve me');
        const beforeSettings = JSON.stringify(page.context.getCurrentUploadSettings());
        let metadataReads = 0;
        let conversions = 0;
        page.context.getMetadataFilenameForEpub = async () => {
          metadataReads++;
          return page.context.buildMetadataFilename('Cafe\u0301: Guide', 'Jose\u0301');
        };
        page.context.convertEpubFile = async file => {
          conversions++;
          return new Blob(['converted book']);
        };
        // Logging UI is outside filename/transport behavior.
        page.context.clearLog = () => {};
        page.context.showLog = () => {};
        page.context.logError = () => {};
        await finishUpload(page);
        const base = rename ? 'Café Guide - José' : 'Café';
        assert.deepEqual(page.uploads.map(upload => upload.name), [`${base} (2).epub`, `${base} (3).epub`]);
        assert.equal(metadataReads, rename ? 2 : 0);
        assert.equal(conversions, convert ? 2 : 0);
        assert.equal(JSON.stringify(page.context.getCurrentUploadSettings()), beforeSettings);
        assert.equal(page.storage.get('saved-settings'), 'preserve me');
        assert.deepEqual(inputFiles.map(file => file.name), ['Cafe\u0301.epub', 'Café.epub']);
        for (const upload of page.uploads) {
          assert.equal(upload.transport, fallback ? 'http' : 'ws');
          const bytes = fallback ? Buffer.from(await upload.file.arrayBuffer()) : Buffer.concat(upload.chunks);
          assert.equal(bytes.toString(), convert ? 'converted book' : 'original book');
          if (fallback) {
            assert.equal(upload.url, '/upload?path=%2Fbooks');
            assert.equal(upload.file.type, 'application/epub+zip');
            if (!convert) assert.equal(upload.file.lastModified, 123456);
          } else assert.equal(upload.path, '/books');
        }
        assert.ok(page.requests.every(url => !url.includes('/rename')));
      });
    }
  }
}

test('missing or failed metadata keeps the original name and still normalizes it', async () => {
  for (const fail of [false, true]) {
    const page = portal();
    page.element('fileInput').files = [new File(['book'], 'Re\u0301sume\u0301.epub')];
    page.element('renameFromMetadataToggle').checked = true;
    page.context.getMetadataFilenameForEpub = async () => { if (fail) throw Error('bad EPUB'); return ''; };
    await finishUpload(page);
    assert.equal(page.uploads[0].name, 'Résumé.epub');
  }
});

test('dialog drags and inside interactions stay open; actual backdrop clicks clean up', async () => {
  const page = portal();
  await page.context.hydrate();
  for (const overlay of page.overlays) {
    overlay.showModal();
    const inside = new Element('dialog text');
    overlay.emit('mousedown', inside);
    overlay.emit('click', overlay);
    assert.ok(overlay.open, `${overlay.id}: selection drag closed dialog`);
    overlay.emit('mousedown', inside);
    overlay.emit('click', inside);
    assert.ok(overlay.open, `${overlay.id}: inside click closed dialog`);
    overlay.emit('mousedown');
    overlay.emit('click', inside);
    assert.ok(overlay.open, `${overlay.id}: backdrop-to-inside drag closed dialog`);
    overlay.emit('mousedown');
    overlay.emit('click');
    assert.equal(overlay.open, false, `${overlay.id}: backdrop click failed`);
    overlay.showModal();
    // A prior close must not leave a stale mousedown target when reopened.
    overlay.emit('click');
    assert.ok(overlay.open, `${overlay.id}: stale target dismissed reopened dialog`);
    overlay.close();
  }
});

test('backdrop and close icon cannot interrupt uploads; Cancel and Escape still cancel', async () => {
  const page = portal();
  await page.context.hydrate();
  const overlay = page.element('uploadModal');
  overlay.showModal();
  page.run('isUploadInProgress = true');
  let aborts = 0;
  page.context.activeXhr = { abort() { aborts++; } };
  page.run('currentUploadXhr = activeXhr');
  overlay.emit('mousedown');
  overlay.emit('click');
  assert.ok(overlay.open);
  assert.equal(page.run('operationCancelled'), false);
  assert.equal(aborts, 0);
  page.context.closeUploadModal();
  assert.ok(overlay.open);
  assert.equal(aborts, 0);
  page.context.handleCancelUploadModal();
  assert.equal(page.run('operationCancelled'), true);
  assert.equal(aborts, 1);
  assert.ok(overlay.open);
  page.run('operationCancelled = false; currentUploadXhr = activeXhr');
  overlay.emit('cancel');
  assert.equal(page.run('operationCancelled'), true);
  assert.equal(aborts, 2);
  page.run('isUploadInProgress = false');
  page.context.closeUploadModal();
  assert.equal(overlay.open, false);
});

test('normal close, Cancel and Escape still close idle dialogs', async () => {
  const page = portal();
  await page.context.hydrate();
  for (const name of ['closeUploadModal', 'handleCancelUploadModal']) {
    page.element('uploadModal').showModal();
    page.context[name]();
    assert.equal(page.element('uploadModal').open, false);
  }
  for (const overlay of page.overlays) {
    overlay.showModal();
    overlay.emit('cancel');
    assert.equal(overlay.open, false, overlay.id);
  }
});

test('image picker escapes EPUB filenames in every preview style', () => {
  const { document, element, run } = portal();
  const rows = [];
  document.createElement = () => ({});
  element('imageGrid').appendChild = item => rows.push(item.innerHTML);
  run(`epubImagesCache = [{isCover:true},{isSeparator:true},{}].map(img =>
    ({...img, name:'cover"><img src=x onerror=alert(1)>.jpg', dataUrl:'blob:preview'}));
    renderImageGrid();`);
  assert.equal(rows.length, 3);
  for (const html of rows) {
    assert.equal((html.match(/<img /g) || []).length, 1);
    assert.match(html, /cover&quot;&gt;&lt;img/);
  }
});
