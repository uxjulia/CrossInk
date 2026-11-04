"""Execute focused firmware paths with host doubles, without booting the UI.

Compile the actual method definitions from the production translation unit so
the upload/Retry tests exercise the same branches the firmware builds.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "src/network/CrossPointWebServer.cpp"


def between(source, first, following):
    return source[source.index(first):source.index(following, source.index(first))]


class ReliabilityTest(unittest.TestCase):
    def compile_and_run(self, code, headers=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for name, content in (headers or {}).items():
                (path / name).write_text(content)
            (path / "test.cpp").write_text(code)
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I", str(path), "-I", str(ROOT / "src/network"), str(path / "test.cpp"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)

    def test_http_short_writes_and_upload_completion(self):
        source = WEB.read_text()
        methods = between(source, "void CrossPointWebServer::handleUploadPost(", "void CrossPointWebServer::handleCreateFolder(")
        methods += between(source, "void CrossPointWebServer::handleFontUpload()", "void CrossPointWebServer::handleFontDelete()")
        self.compile_and_run(r'''
#include <cassert>
#include <string>
#include "CrossPointHttpServer.h"
struct String : std::string {
  using std::string::string;
  bool isEmpty() const { return empty(); }
};
struct FontSystem { int changes=0; void markRegistryDirty() { ++changes; } } sdFontSystem;
class CrossPointWebServer {
 public:
  struct UploadState { bool success=false; String fileName="book.epub", error; };
  struct FontUpload { bool valid=false; std::string filePath; } fontUpload;
  CrossPointHttpServer* server;
  void handleUploadPost(UploadState&) const;
  void handleFontUpload();
};
''' + methods + r'''
int main() {
  CrossPointHttpServer http(80);
  assert(!http.hasUpload()); http.setUpload(); assert(http.hasUpload());
  http.resetClient(100); assert(http.writeTest(false, 12)==12 && http.connected());
  http.resetClient(4); assert(http.writeTest(false, 12)==4 && !http.connected());
  // Further chunks fail immediately, without another blocking write attempt.
  assert(http.writeTest(false, 12)==0 && http.writeCalls()==1);
  http.resetClient(0); assert(http.writeTest(true, 12)==0 && !http.connected());
  CrossPointWebServer web; web.server=&http;
  CrossPointWebServer::UploadState upload;
  upload.success=true; web.handleUploadPost(upload); assert(http.status==200 && !upload.success);
  web.handleUploadPost(upload); assert(http.status==400);
  web.fontUpload.valid=true; web.fontUpload.filePath="/fonts/f.cpfont";
  web.handleFontUpload(); assert(http.status==200 && !web.fontUpload.valid && web.fontUpload.filePath.empty());
  web.handleFontUpload(); assert(http.status==400 && sdFontSystem.changes==1);
}
''', {"Logging.h": "#define LOG_ERR(...) ((void)0)\n#define LOG_DBG(...) ((void)0)\n",
      "WebServer.h": r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
using PGM_P=const char*;
struct MockClient {
  bool open=true; size_t limit=0, calls=0;
  void stop() { open=false; }
  size_t write(size_t size) { if (!open) return 0; ++calls; return std::min(limit,size); }
};
class WebServer {
 protected:
  char* _currentArgs=nullptr; char* _postArgs=nullptr;
  size_t _currentArgCount=0, _postArgsLen=0;
  std::unique_ptr<int> _currentUpload;
  MockClient _currentClient;
  virtual size_t _currentClientWrite(const char*, size_t size) { return _currentClient.write(size); }
  virtual size_t _currentClientWrite_P(PGM_P, size_t size) { return _currentClient.write(size); }
 public:
  explicit WebServer(uint16_t) {}
  virtual ~WebServer()=default;
  int status=0;
  void send(int code, const char*, const std::string&) { status=code; }
  void setUpload() { _currentUpload=std::make_unique<int>(1); }
  void resetClient(size_t limit) { _currentClient={true,limit,0}; }
  size_t writeTest(bool flash,size_t size) { return flash ? _currentClientWrite_P("",size) : _currentClientWrite("",size); }
  bool connected() { return _currentClient.open; }
  size_t writeCalls() { return _currentClient.calls; }
};
'''})

    def test_discovery_and_idle_connections(self):
        source = WEB.read_text()
        discovery = between(source, "void CrossPointWebServer::openDiscoverySocket()", "CrossPointWebServer::WsUploadStatus")
        idle = between(source, "void closeIdleWsClients(", "#endif")
        self.compile_and_run(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <string>
#include <unistd.h>
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
constexpr uint16_t LOCAL_UDP_PORT=8134;
constexpr unsigned WEBSOCKETS_SERVER_CLIENT_MAX=5;
constexpr uint32_t WS_IDLE_TIMEOUT_MS=120000;
uint32_t wsLastActivity[WEBSOCKETS_SERVER_CLIENT_MAX]={}, now=1000;
uint32_t millis() { return now; }
struct WebSocketsServer {
  bool connected[5]={}; unsigned disconnects=0;
  bool clientIsConnected(unsigned i) { return connected[i]; }
  void disconnect(unsigned i) { connected[i]=false; wsLastActivity[i]=0; ++disconnects; }
};
struct { const char* getHostname() { return "reader"; } } WiFi;
class CrossPointWebServer {
 public:
  int discoverySocket=-1; uint16_t wsPort=81;
  void openDiscoverySocket(); void closeDiscoverySocket(); void answerDiscovery() const;
};
''' + discovery + idle + r'''
int main() {
  CrossPointWebServer web; web.openDiscoverySocket(); assert(web.discoverySocket>=0);
  const int fd=web.discoverySocket; web.openDiscoverySocket(); assert(web.discoverySocket==fd);
  int client=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); assert(client>=0);
  assert(fcntl(client,F_SETFL,O_NONBLOCK)>=0);
  sockaddr_in target={}; target.sin_family=AF_INET; target.sin_port=htons(LOCAL_UDP_PORT);
  target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  auto send=[&](const char* text,size_t len) { assert(sendto(client,text,len,0,(sockaddr*)&target,sizeof(target))==static_cast<int>(len)); };
  char reply[128];
  // Reject oversized and embedded-NUL packets, and still answer the next datagram.
  std::string longPacket(2000,'x'); send(longPacket.data(),longPacket.size());
  send("hello\0bad",9); send("hello",5);
  { pollfd ready{fd,POLLIN,0}; assert(poll(&ready,1,1000)==1); }
  web.answerDiscovery(); assert(recv(client,reply,sizeof(reply),0)<0);
  { pollfd ready{fd,POLLIN,0}; assert(poll(&ready,1,1000)==1); }
  web.answerDiscovery(); assert(recv(client,reply,sizeof(reply),0)<0);
  { pollfd ready{fd,POLLIN,0}; assert(poll(&ready,1,1000)==1); }
  web.answerDiscovery();
  { pollfd ready{client,POLLIN,0}; assert(poll(&ready,1,1000)==1); }
  int size=recv(client,reply,sizeof(reply),0); assert(size>0);
  assert(std::string(reply,size)=="crosspoint (on reader);81");
  web.answerDiscovery(); web.closeDiscoverySocket(); assert(web.discoverySocket==-1);
  web.closeDiscoverySocket(); close(client);
  WebSocketsServer ws; ws.connected[0]=true; closeIdleWsClients(ws); assert(wsLastActivity[0]==now);
  now+=WS_IDLE_TIMEOUT_MS; closeIdleWsClients(ws); assert(ws.disconnects==0);
  ++now; closeIdleWsClients(ws); assert(ws.disconnects==1);
  ws.connected[1]=true; wsLastActivity[1]=now; now+=100000;
  wsLastActivity[1]=now; now+=100000; closeIdleWsClients(ws); assert(ws.disconnects==1);
  // Unsigned time differences work across millis() wrap.
  ws.connected[2]=true; wsLastActivity[2]=UINT32_MAX-60000; now=60001;
  closeIdleWsClients(ws); assert(!ws.connected[2]);
}
''')

    def test_long_font_lists_stream_without_truncation(self):
        writer = between(WEB.read_text(), "class WebJsonWriter", "// WebSocket upload state")
        self.compile_and_run(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
struct WebServer {
  std::string output; size_t chunks=0;
  void sendContent(const char* text, size_t length) {
    assert(length<=192); output.append(text,length); ++chunks;
  }
};
''' + writer + r'''
int main() {
  WebServer server; WebJsonWriter out(server); std::string expected="[";
  out.append("[");
  for (int i=0;i<128;++i) {
    const std::string name=std::string(160,'a')+std::to_string(i);
    if (i) { out.append(","); expected+=","; }
    out.appendJsonString(name.c_str()); expected+='"'+name+'"';
  }
  out.append("]"); out.flush(); expected+="]";
  assert(server.output==expected && server.output.size()>20000 && server.chunks>100);
}
''')

    def test_retry_uses_same_path_for_touch_and_button(self):
        source = (ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text()
        start = source.index("  } else if (state_ == ERROR) {", source.index("void FontDownloadActivity::loop()"))
        error = source[start:source.index("// --- Rendering ---",start)]
        error = "  if (false) {" + error
        self.compile_and_run(r'''
#include <cassert>
#include <string>
struct MappedInputManager {
  enum class Button { Back, Confirm };
  bool back=false, confirm=false, tap=false;
  bool wasReleased(Button b) { return b==Button::Back ? back : confirm; }
  bool wasScreenTapped(int&,int&) { return tap; }
};
struct ManifestFamily { int fileCount=3; };
struct FontDownloadActivity {
  enum State { ERROR, FAMILY_LIST }; State state_=ERROR;
  MappedInputManager mappedInput;
  bool manifestReloadNeeded_=false, hasRetryFamily_=false;
  int downloadingFamilyIndex_=-1, manifestFamilyCount_=0, currentFileIndex_=0,currentFileTotal_=0;
  ManifestFamily retryFamily_, manifestFamilies_[1];
  std::string activeDownloadFamilyName_,errorMessage_,errorHint_;
  int fetches=0, downloads=0, exits=0;
  struct RenderLock { explicit RenderLock(FontDownloadActivity&) {} };
  void requestUpdate() {} void requestUpdateAndWait() {}
  void finishAfterBackPress() { ++exits; }
  void returnToFamilyList() { state_=FAMILY_LIST; }
  void onWifiSelectionComplete(bool ok) { assert(ok); ++fetches; }
  void downloadFamily(ManifestFamily&) { ++downloads; }
  void loop() {
''' + error + r'''
};
int main() {
  for (bool touch : {false,true}) {
    FontDownloadActivity a; a.mappedInput.tap=touch; a.mappedInput.confirm=!touch;
    a.loop(); assert(a.fetches==1 && a.downloads==0);
    FontDownloadActivity b; b.hasRetryFamily_=true; b.mappedInput=a.mappedInput;
    b.loop(); assert(b.downloads==1 && b.fetches==0 && b.hasRetryFamily_);
    FontDownloadActivity c; c.downloadingFamilyIndex_=0; c.manifestFamilyCount_=1; c.mappedInput=a.mappedInput;
    c.loop(); assert(c.downloads==1 && c.fetches==0);
  }
  FontDownloadActivity idle; idle.loop(); assert(idle.fetches==0 && idle.downloads==0);
  FontDownloadActivity back; back.mappedInput.back=true; back.loop(); assert(back.exits==1 && back.fetches==0);
}
''')


if __name__ == "__main__":
    unittest.main()
