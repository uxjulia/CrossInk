#pragma once
#include <Logging.h>
#include <WebServer.h>

class CrossPointHttpServer final : public WebServer {
 public:
  explicit CrossPointHttpServer(uint16_t port) : WebServer(port) {}

  void releaseRequestArguments() {
#ifndef SIMULATOR
    if (_currentArgs) {
      delete[] _currentArgs;
      _currentArgs = nullptr;
    }
    _currentArgCount = 0;

    if (_postArgs) {
      delete[] _postArgs;
      _postArgs = nullptr;
    }
    _postArgsLen = 0;
#endif
  }
#ifndef SIMULATOR
  bool hasUpload() const { return static_cast<bool>(_currentUpload); }

 protected:
  size_t _currentClientWrite(const char* data, size_t size) override {
    return checkedWrite(size, WebServer::_currentClientWrite(data, size));
  }
  size_t _currentClientWrite_P(PGM_P data, size_t size) override {
    return checkedWrite(size, WebServer::_currentClientWrite_P(data, size));
  }

 private:
  size_t checkedWrite(size_t wanted, size_t sent) {
    // WebServer ignores short writes and would block again for every chunk.
    if (sent < wanted) {
      LOG_ERR("WEB", "Short response write: %u/%u, closing client", static_cast<unsigned>(sent),
              static_cast<unsigned>(wanted));
      _currentClient.stop();
    }
    return sent;
  }
#endif
};
