#!/usr/bin/env python3
"""Exercise production plugin queue handlers, including lazy allocation failure."""
from pathlib import Path
import os
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

header = (ROOT / 'src/network/CrossPointWebServer.h').read_text()
impl = (ROOT / 'src/network/CrossPointPluginServer.cpp').read_text()
queue = header[header.index('  struct PluginJob {'):header.index('  void handlePluginRunnerPage()')]
source = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
struct String : std::string {
  using std::string::string;
  String(const std::string& s) : std::string(s) {}
  bool isEmpty() const { return empty(); }
};
static bool failAlloc = false;
static unsigned allocations = 0;
template<class T> std::unique_ptr<T> makeUniqueNoThrow(size_t n) {
  ++allocations;
  if (failAlloc) return {};
  return std::unique_ptr<T>(new(std::nothrow) std::remove_extent_t<T>[n]());
}
static uint32_t now = 100;
uint32_t millis() { return now; }
bool safeComponent(const String& s) { return !s.empty() && s.find('/') == std::string::npos; }
struct Server {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> args;
  void send(int code, const char*, const char* text) { status = code; body = text; }
  String arg(const char* key) { return args[key]; }
};
struct CrossPointWebServer {
  Server storage;
  Server* server = &storage;
  std::string request;
  bool rejectIfPluginsDisabled() { return false; }
  bool readJsonBody(JsonDocument& doc) { return !deserializeJson(doc, request); }
  void handlePluginJobSubmit();
  void handlePluginJobClaim();
  void handlePluginJobComplete();
  void handlePluginJobStatus();
'''
source += queue + '\n};\n'
for name, ret in [('allocPluginJob', 'CrossPointWebServer::PluginJob*'),
                  ('handlePluginJobSubmit', 'void'), ('handlePluginJobClaim', 'void'),
                  ('handlePluginJobComplete', 'void'), ('handlePluginJobStatus', 'void')]:
    source += function(impl, f'{ret} CrossPointWebServer::{name}(') + '\n'
source += r'''
int main() {
  CrossPointWebServer web;
  web.server->args["plugin"] = "demo";
  web.handlePluginJobClaim();
  assert(web.storage.status == 200 && web.storage.body == "{\"id\":0}");
  web.server->args["id"] = "1";
  web.handlePluginJobStatus();
  assert(web.storage.body.find("unknown") != std::string::npos);
  web.request = R"({"id":1,"claim":1,"ok":true})";
  web.handlePluginJobComplete();
  assert(web.storage.status == 404 && allocations == 0 && !web.pluginJobs);
  web.request = R"({"plugin":"../bad","action":"run"})";
  web.handlePluginJobSubmit();
  assert(web.storage.status == 400 && allocations == 0);
  web.request = R"({"plugin":"demo","action":"run","args":{"x":1}})";
  failAlloc = true;
  web.handlePluginJobSubmit();
  assert(web.storage.status == 503 && !web.pluginJobs && web.nextPluginJobId == 1);
  assert(web.storage.body.find("out of memory") != std::string::npos);
  failAlloc = false;
  for (int i = 0; i < 6; ++i) { web.handlePluginJobSubmit(); assert(web.storage.status == 200); }
  assert(web.pluginJobs && allocations == 2);
  web.handlePluginJobSubmit();
  assert(web.storage.status == 503 && allocations == 2);
  assert(web.storage.body.find("job queue full") != std::string::npos);
  web.handlePluginJobClaim();
  assert(web.pluginJobs[0].state == web.JOB_RUNNING && web.pluginJobs[0].claim == 1);
  now += web.PLUGIN_JOB_LEASE_MS + 1;
  web.handlePluginJobClaim();
  assert(web.pluginJobs[0].claim == 2);
  web.request = R"({"id":1,"claim":1,"ok":true})";
  web.handlePluginJobComplete();
  assert(web.storage.status == 409 && web.pluginJobs[0].state == web.JOB_RUNNING);
  web.request = R"({"id":1,"claim":2,"ok":true,"result":{"ok":42}})";
  web.handlePluginJobComplete();
  assert(web.storage.status == 200 && web.pluginJobs[0].state == web.JOB_DONE);
  web.handlePluginJobStatus();
  assert(web.storage.body.find("\"state\":\"done\"") != std::string::npos);
  assert(web.storage.body.find("42") != std::string::npos);
  web.request = R"({"plugin":"demo","action":"again"})";
  web.handlePluginJobSubmit();
  assert(web.storage.status == 200 && web.pluginJobs[0].id == 7 && allocations == 2);
  web.pluginJobs.reset();
  assert(!web.pluginJobs);
}
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--json-include', type=Path, default=ROOT / '.pio/libdeps/default/ArduinoJson/src')
json_include = parser.parse_args().json_include
if not json_include.exists():
    raise SystemExit('Build default firmware first to provide the pinned ArduinoJson dependency')
with tempfile.TemporaryDirectory(prefix='plugin-job-heap-') as temp:
    cpp = Path(temp) / 'test.cpp'
    exe = Path(temp) / 'test'
    cpp.write_text(source)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(json_include), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Plugin queue lazy allocation, failure/retry, leases, completion and recycling passed')
