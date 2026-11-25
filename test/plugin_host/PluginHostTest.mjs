import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const context = vm.createContext({TextDecoder, Uint8Array, Blob, FormData, atob, setInterval: () => 1});
context.window = context;
vm.runInContext(fs.readFileSync(new URL('../../web/assets/plugin-host.js', import.meta.url), 'utf8'), context);
const host = context.CrossPoint;
let calls = [];
let replies = [];
context.fetch = async (url, init) => {
  calls.push({url, body: init?.body && JSON.parse(init.body)});
  return {ok: true, json: async () => replies.shift()};
};
replies = [{status: 200, bytes: 5, complete: false}, {status: 206, bytes: 8, complete: true}];
assert.equal((await host.fetchToSd('fixture', 'https://example.test/book', '/book')).bytes, 8);
assert.equal(calls[0].body.offset, 0);
assert.equal(calls[1].body.offset, 5);
for (const reply of [
  {status: 404, bytes: 0, complete: true},
  {status: 200, error: 'sd write failed', bytes: 8, complete: true},
  {status: 200, bytes: 0, complete: false}
]) {
  replies = [reply];
  await assert.rejects(() => host.fetchToSd('fixture', 'https://example.test/book', '/book'));
}
// Plugin and action names must never resolve inherited Object.prototype methods.
host.registerAction('__proto__', 'refresh', () => ({updated: true}));
assert.equal(Object.getPrototypeOf(host._actions), null);
assert.equal(Object.getPrototypeOf(host._actions.__proto__), null);
replies = [{id: 3, claim: 9, action: 'toString', args: {}}, {ok: true}];
await host._pollJobs();
assert.equal(calls.at(-1).body.ok, false);
assert.equal(calls.at(-1).body.claim, 9);
// An async renderer failure must be visible without an unhandled rejection.
const container = {appendChild() {}, replaceChildren(...children) { this.children = children; }};
context.document = {
  getElementById: () => container,
  createElement: (kind) => kind === 'script' ? {} : {style: {}, replaceChildren: container.replaceChildren},
  body: {appendChild(script) {
    host.registerPlugin(async () => { throw new Error('fixture rejection'); });
    script.onload();
  }}
};
let rendered;
container.appendChild = value => { rendered = value; };
replies = [[{name: 'fixture', title: 'Fixture'}]];
await context.loadPlugins('settings');
assert.equal(rendered.children[1].textContent, 'Failed to load plugin.');
console.log('Plugin host tests passed: staged fetch errors, progress, action isolation, async renderer failures.');
