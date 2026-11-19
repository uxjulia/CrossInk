#!/usr/bin/env python3
"""Exercise the production USB HTTP relay with checked framing and bounded SD IO."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--arduinojson', type=Path, required=True)
args = parser.parse_args()
source = (ROOT / 'src/network/UsbSerialFileTransfer.cpp').read_text()
relay = source[source.index('int relaySyncHttp('):source.index('void registerUsbCdcOverflowHandler()')]
harness = r'''
#include <ArduinoJson.h>
#include <SyncJsonAllocator.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
constexpr uint32_t SHORT_TIMEOUT_MS=1000, CHUNK_TIMEOUT_MS=45000;
constexpr uint8_t ACK=6;
uint8_t transferBuffer[256];
std::vector<uint8_t> incoming,outgoing;
size_t offset=0;int acks=0;bool writeOk=true,closeOk=true,syncOk=true;
std::map<std::string,std::string> files;
struct FsFile {
 std::string path;size_t position=0;bool opened=false;
 size_t size() {return files[path].size();}
 int read(void* out,size_t n) {n=std::min(n,files[path].size()-position);memcpy(out,files[path].data()+position,n);position+=n;return n;}
 size_t write(const void* data,size_t n) {if(!writeOk)return 0;files[path].append(static_cast<const char*>(data),n);return n;}
 bool sync() {return syncOk;}
 bool close() {bool result=opened&&closeOk;opened=false;return result;}
};
struct StorageMock {
 bool openFileForRead(const char*,const char* path,FsFile& file) {if(!files.count(path))return false;file.path=path;file.opened=true;return true;}
 bool openFileForWrite(const char*,const char* path,FsFile& file) {file.path=path;file.opened=true;files[path].clear();return true;}
} Storage;
template<class F> struct ScopedCleanup {F f;~ScopedCleanup(){f();}};
template<class F> ScopedCleanup(F)->ScopedCleanup<F>;
bool writeRaw(const uint8_t* p,size_t n) {outgoing.insert(outgoing.end(),p,p+n);return true;}
bool writeLine(const char* p) {return writeRaw(reinterpret_cast<const uint8_t*>(p),strlen(p));}
bool writeAck() {++acks;return true;}
bool readExact(uint8_t* p,size_t n,uint32_t) {if(n>incoming.size()-offset)return false;memcpy(p,incoming.data()+offset,n);offset+=n;return true;}
uint32_t readLe32(const uint8_t* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
bool writeLe32(uint32_t v) {for(int i=0;i<4;i++)outgoing.push_back(v>>(8*i));return true;}
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* p,size_t n) {crc=~crc;while(n--) {crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}return ~crc;}
void reply(unsigned status,const std::string& body,bool corrupt=false) {
 incoming={uint8_t(status),uint8_t(status>>8)};
 for(int i=0;i<4;i++)incoming.push_back(body.size()>>(8*i));
 incoming.insert(incoming.end(),body.begin(),body.end());
 const auto crc=esp_rom_crc32_le(0,incoming.data(),incoming.size())+(corrupt?1:0);
 for(int i=0;i<4;i++)incoming.push_back(crc>>(8*i));
 outgoing.clear();offset=acks=0;writeOk=closeOk=syncOk=true;
}
void flow(size_t requestBytes) { incoming.insert(incoming.begin(), 1+(requestBytes+255)/256, ACK); }

'''
tests = r'''
int main() {
 bool failed=false;char buffer[256];
 reply(200,R"({"until":1,"counter_ids":1})");
 flow(2);
 assert(relaySyncHttp(&failed,"PUT","/api/v1/stats/counters/global","{}",nullptr,buffer,sizeof(buffer),nullptr)==200);
 assert(!failed&&std::string(buffer)==R"({"until":1,"counter_ids":1})"&&acks==2);
 const auto nl=std::find(outgoing.begin(),outgoing.end(),'\n')-outgoing.begin();
 const auto metaSize=std::stoul(std::string(outgoing.begin()+5,outgoing.begin()+nl));
 JsonDocument meta;assert(!deserializeJson(meta,outgoing.data()+nl+1,metaSize));
 assert(meta["bytes"]==2 && meta["max_response"]==255);
 const size_t start=nl+1;
 assert(readLe32(outgoing.data()+start+metaSize+2)==esp_rom_crc32_le(0,outgoing.data()+start,metaSize+2));
 files["/upload.tmp"]=std::string(25000,'x');
 reply(200,R"({"until":1,"accepted":1})");
 flow(25000);
 assert(relaySyncHttp(&failed,"PUT","/api/v1/clippings/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",nullptr,"/upload.tmp",buffer,sizeof(buffer),nullptr)==200);
 assert(outgoing.size()>25000);
 const std::string large(26624,'y');reply(200,large);
 flow(0);
 assert(relaySyncHttp(&failed,"GET","/api/v1/clippings/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",nullptr,nullptr,nullptr,26624,"/download.tmp")==200);
 assert(files["/download.tmp"]==large&&acks==105);
 reply(200,large+"x");
 flow(0);
 assert(relaySyncHttp(&failed,"GET","/api/v1/clippings/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",nullptr,nullptr,nullptr,26624,"/download.tmp")==0&&failed&&acks==0);
 failed=false;reply(200,"{}",true);
 flow(2);
 assert(relaySyncHttp(&failed,"PUT","/api/v1/stats/counters/global","{}",nullptr,buffer,sizeof(buffer),nullptr)==0&&failed);
 const size_t sent=outgoing.size();
 flow(2);
 assert(relaySyncHttp(&failed,"PUT","/api/v1/stats/counters/global","{}",nullptr,buffer,sizeof(buffer),nullptr)==0&&outgoing.size()==sent);
 failed=false;reply(0,"");
 flow(2);
 assert(relaySyncHttp(&failed,"PUT","/api/v1/stats/counters/global","{}",nullptr,buffer,sizeof(buffer),nullptr)==0&&failed);
 failed=false;reply(404,"");
 flow(0);
 assert(relaySyncHttp(&failed,"GET","/api/v1/stats/streak?tz=0",nullptr,nullptr,buffer,sizeof(buffer),nullptr)==404&&!failed);
 for(int fault=0;fault<3;fault++) {
 failed=false;reply(200,"data");
 if(fault==0)writeOk=false;if(fault==1)closeOk=false;if(fault==2)syncOk=false;
 flow(0);
 assert(relaySyncHttp(&failed,"GET","/api/v1/clippings/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",nullptr,nullptr,nullptr,26624,"/download.tmp")==0&&failed);
 }
 failed=false;reply(200,"{}");incoming.pop_back();
 flow(2);
 assert(relaySyncHttp(&failed,"PUT","/api/v1/stats/counters/global","{}",nullptr,buffer,sizeof(buffer),nullptr)==0&&failed);
 puts("USB HTTP relay checks passed (large files, bounds, CRC/status, interruption, storage failure)");
}
'''
with tempfile.TemporaryDirectory(prefix='crossink-usb-relay-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.cpp').write_text(harness + relay + tests)
    binary = tmp / 'test'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I'+str(args.arduinojson), '-I'+str(ROOT / 'lib/KOReaderSync'), str(tmp / 'test.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
