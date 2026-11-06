/* Execute the shipped browser uploader against a two-connection transport double. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../web/pages/files.js'), 'utf8');
const uploader = source.slice(source.indexOf('function uploadFileWebSocket('), source.indexOf('// Upload file via HTTP'));
const catchStart = source.indexOf('if (useWebSocket && error.message === "WebSocket connection failed")');
const fallback = source.slice(catchStart, source.indexOf('\n    }\n  }', catchStart));
assert.ok(catchStart >= 0);
let active = 0;
const sockets = [];
class Socket {
  static OPEN = 1;
  constructor() {
    sockets.push(this);
    this.sent = [];
    this.bufferedAmount = 0;
    this.readyState = active < 2 ? 1 : 3;
    if (this.readyState === 1) active++;
    else queueMicrotask(() => this.onerror({}));
  }
  send(data) { this.sent.push(data); }
  close() {
    if (this.readyState !== 3) { this.readyState = 3; active--; this.onclose({code: 1000, reason: ''}); }
  }
}
const state = {
  WebSocket: Socket,
  getWsUrl: () => 'ws://reader:81/',
  currentPath: '/', currentUploadWs: null, WS_CHUNK_SIZE: 4,
  console: {log() {}, error() {}},
  setTimeout: callback => queueMicrotask(callback),
  useWebSocket: true,
  httpUploads: 0, completed: 0,
  file: {name: 'test.epub', size: 7, slice: (a, b) => ({arrayBuffer: async () => new ArrayBuffer(b-a)})},
  onProgress() {},
  onComplete() { state.completed++; },
  onError(message) { throw new Error(message); },
  uploadFileHTTP: async () => { state.httpUploads++; },
};
vm.createContext(state);
vm.runInContext(uploader + '\nasync function fallbackForTest(error) {' + fallback + '\n}', state);
(async () => {
  const one = state.uploadFileWebSocket(state.file).catch(e => e);
  const two = state.uploadFileWebSocket(state.file).catch(e => e);
  assert.equal(active, 2);
  const exhausted = await state.uploadFileWebSocket(state.file).catch(e => e);
  assert.equal(exhausted.message, 'WebSocket connection failed');
  await state.fallbackForTest(exhausted);
  assert.equal(state.httpUploads, 1);
  assert.equal(state.completed, 1);
  sockets[0].close();
  assert.equal((await one).message, 'WebSocket closed during upload');
  const progress = [];
  const retry = state.uploadFileWebSocket(state.file, (n, total) => progress.push([n, total]));
  const socket = sockets.at(-1);
  socket.onopen();
  assert.match(socket.sent[0], /^START:test.epub:7:\/$/);
  await socket.onmessage({data: 'READY'});
  assert.equal(socket.sent.length, 3); // START plus two binary chunks
  await socket.onmessage({data: 'DONE'});
  await retry;
  assert.deepEqual(progress.at(-1), [7, 7]);
  assert.equal(socket.readyState, 3);
  // A peer close after the last chunk but before DONE must still fail.
  const interrupted = state.uploadFileWebSocket(state.file).catch(e => e);
  const last = sockets.at(-1);
  last.onopen();
  await last.onmessage({data: 'READY'});
  last.close();
  assert.equal((await interrupted).message, 'WebSocket closed during upload');
  sockets[1].close();
  await two;
  assert.equal(active, 0);
  console.log('Browser uploader: two slots, exhaustion/HTTP fallback, reconnect, DONE and interrupted completion passed');
})().catch(e => { console.error(e); process.exitCode = 1; });
