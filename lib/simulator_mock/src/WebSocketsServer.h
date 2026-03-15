#pragma once
#include "WString.h"

// Dummy WebSockets object types
enum WStype_t {
    WStype_DISCONNECTED,
    WStype_CONNECTED,
    WStype_TEXT,
    WStype_BIN,
    WStype_ERROR,
    WStype_FRAGMENT_TEXT_START,
    WStype_FRAGMENT_BIN_START,
    WStype_FRAGMENT,
    WStype_FRAGMENT_FIN,
    WStype_PING,
    WStype_PONG,
};

class WebSocketsServer {
public:
    WebSocketsServer(int port) {}
    void begin() {}
    void loop() {}
    template<typename T>
    void onEvent(T) {}
    void broadcastTXT(const String& txt) {}
    void broadcastTXT(const char* txt) {}
    void sendTXT(uint8_t num, const String& txt) {}
    void sendTXT(uint8_t num, const char* txt) {}
    void close() {}
};
