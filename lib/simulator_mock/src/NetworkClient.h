#pragma once

#include "WString.h"
#include <cstddef>
#include <cstdint>

class Stream;

class NetworkClient {
public:
    NetworkClient() {}
    virtual ~NetworkClient() {}
    virtual int connect(const char* host, uint16_t port) { return 1; }
    virtual size_t write(const uint8_t* buf, size_t size) { return size; }
    virtual size_t write(const char* str) { return write((const uint8_t*)str, strlen(str)); }
    virtual size_t write(uint8_t c) { return write(&c, 1); }
    virtual size_t write(Stream& stream) { return 0; }  // Dummy implementation
    virtual int available() { return 0; }
    virtual int read() { return -1; }
    virtual void stop() {}
    virtual uint8_t connected() { return 1; }
    operator bool() { return true; }
};

class NetworkClientSecure : public NetworkClient {
public:
    void setInsecure() {}
};