#!/usr/bin/env python3
"""Run the production USB progress framing code against a byte-level serial spy."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--arduinojson", type=Path, required=True)
args = parser.parse_args()
source = (ROOT / "src/network/UsbSerialFileTransfer.cpp").read_text()
handler = source[source.index("void dispatchProgressSync("):source.index("void handleCommand()")]
harness = r'''
#include <ArduinoJson.h>
#include <SyncJsonAllocator.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
constexpr uint32_t SHORT_TIMEOUT_MS=1000, CHUNK_TIMEOUT_MS=45000;
constexpr size_t SERIAL_CHUNK_SIZE=256;
std::vector<uint8_t> incoming,outgoing;
size_t offset=0;bool fileTransferAllowed=true;int calls=0,acks=0;bool large=false;
std::string usbResumeBook;
uint32_t usbBookSession=0;
bool usbSessionAtHome=false;
struct Manager {
 bool active=false, checkpointOk=true;
 std::string reopened;
 bool canSyncOpenEpubOverUsb() const { return active; }
 bool closeOpenEpubForUsbSync(std::string& path) {
  if (!checkpointOk) return false;
  path="/Book.epub"; active=false; return true;
 }
 void goToReader(std::string path) { reopened=std::move(path); }
} activityManager;
struct { std::string openEpubPath; } APP_STATE;
struct { bool exists(const char*) {return true;} } Storage;
namespace FsHelpers { bool hasEpubExtension(const std::string&) { return true; } }
struct GfxRenderer {} renderer;
struct RenderLock {};
bool ensureFileTransferAllowed() { return fileTransferAllowed; }
bool writeRaw(const uint8_t* data,size_t n) { outgoing.insert(outgoing.end(),data,data+n);return true; }
bool writeLine(const char* text) { return writeRaw(reinterpret_cast<const uint8_t*>(text),strlen(text)); }
bool writeAck() { ++acks;return true; }
bool readExact(uint8_t* target,size_t n,uint32_t) {
 if(n>incoming.size()-offset)return false;
 memcpy(target,incoming.data()+offset,n);offset+=n;return true;
}
uint32_t readLe32(const uint8_t* p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
bool writeLe32(uint32_t value) { for(int i=0;i<4;i++)outgoing.push_back(value>>(8*i));return true; }
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* data,size_t n) {
 crc=~crc;for(size_t i=0;i<n;i++) { crc^=data[i];for(int bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0xedb88320u:0); }return ~crc;
}
template<class T> std::unique_ptr<T> makeUniqueNoThrow(size_t n) { return std::unique_ptr<T>(new(std::nothrow) std::remove_extent_t<T>[n]); }
namespace UsbProgressSync {
constexpr size_t MAX_REQUEST=12288,MAX_RESPONSE=12288;
bool readerId(char* out,size_t capacity) { return snprintf(out,capacity,"reader-1")>0; }
void handle(JsonVariantConst,JsonDocument& out,GfxRenderer&) { ++calls;if(large)out["value"]=std::string(13000,'x');else out["applied"]=true; }
}
void request(const std::string& text,bool goodCrc=true) {
 incoming.clear();outgoing.clear();offset=acks=calls=0;
 incoming.push_back(text.size());incoming.push_back(text.size()>>8);
 incoming.insert(incoming.end(),text.begin(),text.end());
 const uint32_t crc=esp_rom_crc32_le(0,reinterpret_cast<const uint8_t*>(text.data()),text.size())+(goodCrc?0:1);
 for(int i=0;i<4;i++)incoming.push_back(crc>>(8*i));
}
'''
tests = r'''
int main() {
 request(R"({"op":"apply"})");handleProgressSync();assert(calls==1&&acks==1);
 const std::string out(outgoing.begin(),outgoing.end());assert(out.rfind("READY\nSYNC:",0)==0);
 auto start=out.find('\n',6)+1;const size_t n=std::stoul(out.substr(11));
 assert(readLe32(outgoing.data()+start+n)==esp_rom_crc32_le(0,outgoing.data()+start,n));
 request(R"({"op":"apply"})",false);handleProgressSync();assert(calls==0);
 assert(std::string(outgoing.begin(),outgoing.end()).find("ERR:sync_crc")!=std::string::npos);
 request("invalid json");handleProgressSync();assert(calls==0);
 request("[]");handleProgressSync();assert(calls==0);
 request(std::string(12289,'x'));handleProgressSync();assert(calls==0&&acks==0&&offset==2);
 fileTransferAllowed=false;request(R"({"op":"apply"})");handleProgressSync();assert(calls==0&&acks==0&&offset==2);fileTransferAllowed=true;
 request(R"({"op":"apply"})");incoming.pop_back();handleProgressSync();assert(calls==0);
 request(std::string("{\"op\":\"")+std::string(700,'x')+"\"}");handleProgressSync();assert(calls==1&&acks==3);
 large=true;request(R"({"op":"apply"})");handleProgressSync();assert(calls==1);
 assert(std::string(outgoing.begin(),outgoing.end()).find("ERR:sync_response_size")!=std::string::npos);
 large=false;
 activityManager.active=true;fileTransferAllowed=false;
 request(R"({"op":"apply"})");handleProgressSync();assert(calls==0 && acks==1);
 request(R"({"op":"begin_current","device_id":"wrong"})");handleProgressSync();assert(activityManager.active);
 activityManager.checkpointOk=false;
 request(R"({"op":"begin_current","device_id":"reader-1"})");handleProgressSync();assert(activityManager.active);
 activityManager.checkpointOk=true;
 request(R"({"op":"begin_current","device_id":"reader-1"})");handleProgressSync();
 assert(!activityManager.active && usbBookSession==1 && usbResumeBook=="/Book.epub");
 fileTransferAllowed=true;
 request(R"({"op":"resume_current","session":2,"device_id":"reader-1"})");handleProgressSync();
 assert(activityManager.reopened.empty());
 request(R"({"op":"resume_current","session":1,"device_id":"reader-1"})");handleProgressSync();
 assert(activityManager.reopened=="/Book.epub" && usbResumeBook.empty());
 puts("USB progress framing checks passed");
}
'''
with tempfile.TemporaryDirectory(prefix="crossink-sync-framing-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.cpp").write_text(harness + handler + tests)
    binary = tmp / "test"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-fsanitize=address,undefined", "-I"+str(args.arduinojson), "-I"+str(ROOT / "lib/KOReaderSync"), str(tmp / "test.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
