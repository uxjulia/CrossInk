#pragma once

#include "WString.h"
#include <memory>

class NetworkClient;
class Stream;

enum {
  HTTPC_STRICT_FOLLOW_REDIRECTS,
  HTTP_CODE_OK = 200
};

class HTTPClient {
public:
    HTTPClient() {}
    ~HTTPClient() {}

    void begin(NetworkClient& client, const char* url) {}
    void setFollowRedirects(int mode) {}
    void addHeader(const char* name, const String& value) {}
    void addHeader(const char* name, const char* value) {}
    void setAuthorization(const char* user, const char* pass) {}

    int GET() { return HTTP_CODE_OK; }
    int POST() { return HTTP_CODE_OK; }
    int POST(const char* body) { return HTTP_CODE_OK; }
    int PUT(const char* body) { return HTTP_CODE_OK; }
    int PUT(const String& body) { return HTTP_CODE_OK; }

    String getString() { return String("mock response"); }
    int getSize() { return 0; }
    int writeToStream(Stream* stream) { return 0; }

    void end() {}
};