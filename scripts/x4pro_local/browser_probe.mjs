// Optional offline Home assertion and screenshot through Chrome's local CDP.
// Node 22+ built-in WebSocket; no npm packages. Native owns persistence tests.
import fs from 'node:fs';
import path from 'node:path';

const run = fs.realpathSync(path.resolve(process.argv[2] || ''));
const seconds = Number(process.argv[3] || 90);
if (!process.argv[2] || !Number.isFinite(seconds) || seconds <= 0) {
  throw new Error('Usage: node browser_probe.mjs RUN_DIRECTORY [TIMEOUT_SECONDS]');
}
const info = JSON.parse(fs.readFileSync(path.join(run, 'browser.json'), 'utf8'));
const manifest = JSON.parse(fs.readFileSync(path.join(run, 'manifest.json'), 'utf8'));
if (manifest.firmware.log_profile?.activity_entry_signature_present === false) {
  throw new Error('Browser Home predicate requires x4-pro-debug / LOG_LEVEL=2; production image lacks activity-entry logs');
}
const requestedProfile = path.resolve(info.profile);
if (fs.realpathSync(path.dirname(requestedProfile)) !== run || !info.url.startsWith('http://127.0.0.1:')) {
  throw new Error('Only a runner-owned private local browser profile is supported');
}
const deadline = Date.now() + seconds * 1000;
const remaining = () => {
  const ms = deadline - Date.now();
  if (ms <= 0) throw new Error('Browser readiness deadline exceeded');
  return Math.min(ms, 5000);
};
const delay = () => new Promise(resolve => setTimeout(resolve, 100));
while (!fs.existsSync(requestedProfile)) { remaining(); await delay(); }
const profile = fs.realpathSync(requestedProfile);
if (path.dirname(profile) !== run) throw new Error('Browser profile escapes private run');
const portFile = path.join(profile, 'DevToolsActivePort');
while (!fs.existsSync(portFile)) { remaining(); await delay(); }
const port = Number(fs.readFileSync(portFile, 'utf8').split('\n')[0]);
let page;
while (!page) {
  remaining();
  try {
    const pages = await (await fetch(`http://127.0.0.1:${port}/json`, {
      signal: AbortSignal.timeout(remaining()),
    })).json();
    page = pages.find(p => p.type === 'page' && p.url === info.url);
  } catch (error) {
    remaining(); // Chrome may publish its port before accepting connections.
  }
  if (!page) await delay();
}
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error('CDP connection timeout')), remaining());
  ws.addEventListener('open', () => { clearTimeout(timer); resolve(); }, {once: true});
  ws.addEventListener('error', () => { clearTimeout(timer); reject(new Error('CDP connection failed')); }, {once: true});
});
let sequence = 0;
const calls = new Map();
ws.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  const entry = calls.get(message.id);
  if (!entry) return;
  clearTimeout(entry.timer); calls.delete(message.id);
  if (message.error) entry.reject(new Error(JSON.stringify(message.error)));
  else entry.resolve(message.result);
});
function call(method, params = {}) {
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { calls.delete(id); reject(new Error(`CDP ${method} timeout`)); }, remaining());
    calls.set(id, {resolve, reject, timer});
    ws.send(JSON.stringify({id, method, params}));
  });
}
let log = '';
try {
  await call('Runtime.enable');
  while (true) {
    const response = await call('Runtime.evaluate', {
      expression: `JSON.stringify({log:document.getElementById('console')?.textContent || '',
        isolated:crossOriginIsolated, turbo:document.getElementById('turbo')?.checked,
        wifi:document.getElementById('wifi')?.checked})`, returnByValue: true,
    });
    if (response.exceptionDetails) throw new Error('Browser DOM inspection failed');
    const state = JSON.parse(response.result.value);
    log = state.log;
    fs.writeFileSync(path.join(run, 'browser-serial.log'), log);
    if (/Guru Meditation|assert failed|panic|abort\(/i.test(log)) throw new Error('Browser guest crash');
    if (/Device: xteink_x4_pro/.test(log) && /Entering activity: Home[\s\S]*8179_DRF/.test(log)) {
      if (!state.isolated || state.turbo || state.wifi) throw new Error('Expected isolated offline model timing');
      break;
    }
    remaining(); await delay();
  }
  const shot = await call('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false});
  fs.writeFileSync(path.join(run, 'browser-home.png'), Buffer.from(shot.data, 'base64'));
  fs.writeFileSync(path.join(run, 'browser-assertions.json'), JSON.stringify({status: 'passed',
    predicate: 'CrossInk X4 Pro Home followed by panel refresh', network: 'offline', turbo: false}, null, 2));
  console.log('PASS browser Home, offline, model timing');
} catch (error) {
  fs.writeFileSync(path.join(run, 'browser-assertions.json'), JSON.stringify({status: 'failed', error: String(error)}, null, 2));
  throw error;
} finally {
  for (const entry of calls.values()) clearTimeout(entry.timer);
  ws.close();
}
