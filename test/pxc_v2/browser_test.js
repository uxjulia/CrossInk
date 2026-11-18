// Exercise the real browser encoders without requiring canvas or DOM setup.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../web/pages/files.js'), 'utf8');
vm.runInThisContext(source.slice(source.indexOf('function crossInkCrc32('), source.indexOf('async function buildCrossInkPxc(')));
const golden = (name) => new Uint8Array(fs.readFileSync(path.join(__dirname, 'golden', name)));
const output = encodeCrossInkPxc2(golden('pixels.pxc'));
assert.deepEqual(output, golden('browser.pxc2'));
assert.deepEqual(output, golden('packbits.pxc2'));
for (const count of [0, 1, 89, 256]) {
  const entries = Array.from({ length: count }, (_, i) => ({ href: `EPUB/${i}.jpg`, pxc: 'META-INF/crossink/pxc/x.pxc2',
    width: 127, height: 131, pxcBytes: output.length, pixelCrc32: new DataView(output.buffer).getUint32(24, true) }));
  const manifest = new TextEncoder().encode(JSON.stringify({ images: entries }));
  const index = buildCrossInkImageIndex(manifest, entries), view = new DataView(index.buffer);
  assert.equal(index.length, 32 + 208 * count);
  assert.equal(view.getUint32(16, true), crossInkCrc32(manifest));
  assert.equal(view.getUint32(24, true), crossInkCrc32(index.subarray(32)));
  assert.equal(view.getUint32(28, true), crossInkCrc32(index.subarray(0, 28)));
  if (count) assert.throws(() => buildCrossInkImageIndex(manifest, [entries[0], entries[0]]));
}
for (const bad of ['', '/x', '../x', 'a\\b', 'a%00', 'x'.repeat(129)]) assert.ok(!crossInkIndexPath(bad, 128));
console.log('Browser PXC2 golden and COIX tests passed');

vm.runInThisContext(source.slice(source.indexOf('function crossInkPxcRules('), source.indexOf('function crossInkPxcLength(')));
const rules = crossInkPxcRules('div.full img { width:100px; height:200px }');
const image = (inside) => ({ getAttribute: () => '', matches: (selector) => inside && selector === 'div.full img' });
assert.deepEqual(crossInkPxcStyle(rules, image(true)), { width: '100px', height: '200px' });
assert.deepEqual(crossInkPxcStyle(rules, image(false)), {});
assert.deepEqual(crossInkPxcStyle(rules, { getAttribute: () => '', matches: () => { throw new Error('Invalid selector'); } }), {});

vm.runInThisContext(source.slice(source.indexOf('function applyImageContrast('), source.indexOf('function applyGrayscale(')));
// Reference tones from Inky desktop (Pillow RGB-to-L, 1% autocontrast, 1.2x contrast).
function checkTones(samples) {
  const pixels = [];
  const expected = [];
  for (const [rgb, count, tone] of samples) {
    for (let i = 0; i < count; i++) {
      pixels.push(...rgb, 255);
      expected.push(tone, tone, tone, 255);
    }
  }
  const actual = new Uint8ClampedArray(pixels);
  applyImageContrast(actual);
  assert.deepEqual(Array.from(actual), expected);
}
checkTones([[[191,191,191],4,0], [[200,200,200],50,22], [[215,215,215],92,96],
            [[232,232,232],45,177], [[254,254,254],7,255], [[0,0,0],1,0], [[255,255,255],1,255]]);
checkTones([[[64,126,98],1,98], [[150,156,129],1,155], [[200,57,75],1,96],
            [[154,169,207],1,176], [[255,255,255],1,255], [[0,0,0],1,0]]);
checkTones([[[191,191,191],100,191]]);

vm.runInThisContext(source.slice(source.indexOf('function crossInkPxcLength('), source.indexOf('function crossInkPxcPathKey(')));
for (const [width, height] of [[480, 800], [528, 792]]) {
  globalThis.MAX_WIDTH = width;
  globalThis.MAX_HEIGHT = height;
  assert.deepEqual(crossInkPxcSize(width, 116, {width: '100%'}), {width, height: 116});
  assert.equal(crossInkPxcSize(1, 1, {height: '50vw'}).height, width / 2);
  assert.equal(crossInkPxcSize(1, 1, {height: '50%'}).height, height / 2);
}
console.log('Browser image contrast and portrait sizing tests passed');
