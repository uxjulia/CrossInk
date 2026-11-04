const fs = require('fs');
const vm = require('vm');
const script = fs.readFileSync(process.argv[2], 'utf8');
const elements = new Map();
function element() {
  return {children: [], addEventListener() {},
    replaceChildren() { this.children = []; },
    appendChild(child) { this.children.push(child); }};
}
const context = vm.createContext({
  document: { getElementById(id) {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  }, createElement() { return element(); } },
  fetch: async () => ({ok: true, json: async () => ({ttfSupported: false, families: []})})
});
vm.runInContext(script, context);
vm.runInContext(`
  ttfSupported = true;
  if (familyFromFilename('Bitter-BoldItalic.OTF') !== 'Bitter') throw Error('OTF family');
  if (cpfontFilesOnly([{name:'A.OtF'}, {name:'A.TTF'}, {name:'A_14.cpfont'}, {name:'A.ttc'}, {name:'A.otf.tmp'}]).length !== 3)
    throw Error('scalable filter');
  ttfSupported = false;
  if (cpfontFilesOnly([{name:'A.OTF'}, {name:'A.ttf'}, {name:'A_14.cpfont'}]).length !== 1)
    throw Error('C3 gate');
`, context);
console.log('PASS: web OTF family derivation, suffix filtering and C3 gating');

(async () => {
  await vm.runInContext('loadFonts()', context);
  context.fetch = async () => ({ok: false, status: 503, json: async () => ({error: 'out of memory'})});
  await vm.runInContext('loadFonts()', context);
  if (elements.get('families').children[0].textContent !== 'Failed to load font list')
    throw Error('HTTP failure shown as empty font list');
  context.fetch = async () => ({ok: true, json: async () => ({families:
    Array.from({length: 128}, (_, i) => ({name: 'Long font family '.repeat(10) + i, sizes: [12,14], files: []}))})});
  await vm.runInContext('loadFonts()', context);
  if (elements.get('families').children.length !== 128) throw Error('truncated font list');
  console.log('PASS: font list failures and 128 long family names');
})().catch(error => { console.error(error); process.exitCode = 1; });
