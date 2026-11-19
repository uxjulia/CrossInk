#!/usr/bin/env python3
"""Exercise the production offline progress handler with real ArduinoJson and fake storage/mapping."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--arduinojson", type=Path, required=True, help="ArduinoJson src directory")
args = parser.parse_args()
STUBS = r'''
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "activities/reader/ProgressRecord.h"
#define LOG_ERR(...) ((void)0)
#include <ArduinoJson.h>
class StatsUploadClient {
 public:
 enum class Result {Ok, Network, Auth, Unsupported, InvalidResponse, LowMemory, Server, Skipped, Cancelled};
 struct Relay {
 const char* server; const char* username; void* context;
 int (*request)(void*,const char*,const char*,const char*,const char*,char*,size_t,const char*);
 char response[256]{};
 };
 static bool settled(Result result) { return result==Result::Ok || result==Result::Skipped; }
 inline static int lastHttpCode=200;
};
inline int statsCalls=0,clippingCalls=0;
namespace ClippingsUpload {
inline StatsUploadClient::Result sync(const std::string&,const std::string&,bool(*)(void*),void*,StatsUploadClient::Relay* relay) {
 assert(relay->username[0]); ++clippingCalls; return StatsUploadClient::Result::Ok;
}
}
namespace UsbSerialFileTransfer {
inline int relaySyncHttp(void*,const char*,const char*,const char*,const char*,char*,size_t,const char*) {return 0;}
}
class GfxRenderer {};
inline int utf8SafeTruncateBuffer(const char*,int length) { return length; }
inline bool settleOk=true, credentials=true; inline int retired=0,cleared=0; inline std::string locator="/body/p[1]/text().42";
inline bool loadOk=true, mapOk=true, saveOk=true, cardOk=true, metadata=true;
inline int saves=0;
inline uint32_t clockMs=0;
inline uint32_t millis() { return clockMs; }
inline std::vector<std::string> paths={"/Book.epub", "/Unread.epub", "/Note.txt"};
struct StorageStub { bool exists(const char*) { return true; } };
inline StorageStub Storage;
namespace FsHelpers {
inline bool hasXtcExtension(const std::string& p) { return p.size()>=4 && p.substr(p.size()-4)==".xtc"; }
inline bool hasEpubExtension(const std::string& p) { return p.size()>=5 && p.substr(p.size()-5)==".epub"; }
std::string normalisePath(const std::string& p);
}
class Epub {
 public:
 enum class XLocationLoadMode { Immediate };
 Epub(const std::string&, const char*) {}
 void setupCacheDir() {}
 static std::string resolveCachePathForFilePath(const std::string& path, const char*) { return path; }
 bool load(bool,bool,XLocationLoadMode,bool) { return loadOk; }
 int getSpineItemsCount() { return 5; }
 std::string getTitle() { return "Títle"; }
 std::string getAuthor() { return "Author"; }
};
namespace EpubReaderUtils {
struct Progress { int spineIndex=1,pageNumber=2,pageCount=10; bool hasPageCount=true; uint32_t visibleTextOffset=42; bool hasVisibleTextOffset=true; };
inline Progress saved;
inline bool loadProgressPath(const std::string& p,Progress& out,const char*) { out=saved; return p.find("Unread")==std::string::npos; }
inline bool saveProgress(Epub&,int,int,int,std::optional<uint32_t>) { if(saveOk) ++saves; return saveOk; }
}
enum class DocumentMatchMethod { FILENAME,BINARY };
enum class KOReaderSyncBehavior {SMART,ASK};
struct Store {
 DocumentMatchMethod method=DocumentMatchMethod::FILENAME;
 KOReaderSyncBehavior behavior=KOReaderSyncBehavior::SMART;
 KOReaderSyncBehavior getSyncBehavior() { return behavior; }
 bool supportsRichProgress() { return true; }
 DocumentMatchMethod getMatchMethod() { return method; }
 bool getSendMetadata() { return metadata; }
 bool hasCredentials() { return credentials; }
 std::string getBaseUrl() { return "https://sync.example.com/"; }
 std::string getUsername() { return "test-user"; }
};
inline Store KOREADER_STORE;
class KOReaderDocumentId {
 public:
 static std::string calculateFromFilename(const std::string&) { return std::string(32,'a'); }
 static std::string calculate(const std::string&) { return std::string(32,'b'); }
};
struct KOReaderMetadata { std::string filename, title, authors; };
struct KOReaderRichPosition { uint32_t pctQ=0; uint16_t spineIndex=0,pageNumber=0,totalPages=1; std::optional<uint16_t> paragraphIndex; std::string xpath; };
struct KOReaderProgress { std::string document, progress,device; float percentage; std::optional<KOReaderMetadata> metadata; std::optional<KOReaderRichPosition> position; };
class KOReaderSyncClient {
 public:
 static bool readerId(char* p,size_t n) { snprintf(p,n,"reader-1"); return true; }
};
struct CrossPointPosition {
 int spineIndex,pageNumber,totalPages;
 bool valid=true,hasVisibleTextOffset=false;
 uint32_t visibleTextOffset=0;
};
enum class PositionCoordinateSpace { SourceDocument,CurrentDocument };
struct KOReaderPosition { std::string xpath="/body/p[1]/text().42"; float percentage=.25; bool valid=true; };
class ProgressMapper {
 public:
 static KOReaderPosition toKOReader(const std::shared_ptr<Epub>&,const CrossPointPosition&,PositionCoordinateSpace) { KOReaderPosition p; p.valid=mapOk; return p; }
};
class KOReaderSyncActivity {
 public:
 static bool sameReadingProgress(float a,float b) { return std::fabs(a-b)<=0.001f; }
 static CrossPointPosition mapRemotePosition(const std::shared_ptr<Epub>&,GfxRenderer&,const KOReaderProgress& remote,DocumentMatchMethod,int,int,bool recovery=false) {
  CrossPointPosition p{2,4,20};
  p.valid=mapOk && (!recovery || !remote.progress.empty() || remote.position.has_value());
  return p;
 }
};
namespace ReadingSyncUpload {
inline StatsUploadClient::Result globalStats(void*,StatsUploadClient::Relay*) {++statsCalls;return StatsUploadClient::Result::Ok;}
inline StatsUploadClient::Result stats(const std::string&,const std::string&,StatsUploadClient::Relay*) {++statsCalls;return StatsUploadClient::Result::Ok;}

inline bool prepareProgress(const std::string&, KOReaderProgress& p, bool* unavailable=nullptr) {
 if(EpubReaderUtils::saved.spineIndex>=5) {
  if(unavailable)*unavailable=true;
  return false;
 }
 if(!loadOk || !mapOk || EpubReaderUtils::saved.pageCount<1)return false;
 p.document=KOREADER_STORE.method==DocumentMatchMethod::FILENAME?std::string(32,'a'):std::string(32,'b');
 p.progress=locator;p.percentage=.25;p.device="EEGO";
 if(metadata) p.metadata=KOReaderMetadata{"Book.epub","Títle","Author"};
 return true;
}
}
namespace SyncParked { inline bool remove(const std::string&) { ++retired;return settleOk; } }
namespace SyncPending {
constexpr uint8_t PROGRESS_BIT=1, STATS_BIT=2, CLIPPINGS_BIT=4;
inline uint8_t clearedBits=0;
inline bool clearExisting(const std::string&,uint8_t bits) { clearedBits=bits; ++cleared; return settleOk; }
}
namespace RecentBookProgress { inline void saveCachedEpubPercent(const Epub&,int,int,int) {} }
struct Settings { const char* getEffectiveDeviceName() { return "EEGO"; } };
inline Settings SETTINGS;
class FolderBookIterator {
 public:
 enum class Result { Entry, Book, Done, Error };
 explicit FolderBookIterator(const std::string& root) { assert(root=="/"); }
 Result next(std::string& path) {
  if(!cardOk) return Result::Error;
  if(offset==paths.size()) return Result::Done;
  path=paths[offset++];
  if(!FsHelpers::hasXtcExtension(path) && !FsHelpers::hasEpubExtension(path)) return Result::Entry;
  return Result::Book;
 }
 private:
 size_t offset=0;
};
template<class T,class... A> std::unique_ptr<T> makeUniqueNoThrow(A&&... a) { return std::unique_ptr<T>(new(std::nothrow) T(std::forward<A>(a)...)); }
'''
TEST = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include "Common.h"
#include "network/UsbProgressSync.h"
GfxRenderer renderer;
int main() {
 JsonDocument req,reply;
 auto run=[&](const char* json) { req.clear(); reply.clear(); assert(!deserializeJson(req,json)); UsbProgressSync::handle(req.as<JsonVariantConst>(),reply,renderer); };
 run(R"({"op":"books","cursor":0})");
 assert(reply["books"].size()==1 && reply["done"]==true && reply["next"]==1);
 cardOk=false; run(R"({"op":"books","cursor":0})"); assert(reply["error"]=="book_scan_failed"); cardOk=true;
 run(R"({"op":"books","cursor":-1})"); assert(reply["error"]=="invalid_cursor");
 run(R"({"op":"books","cursor":4})"); assert(reply["error"]=="invalid_cursor");
 for(auto path : {"/.crosspoint/book.epub","/../book.epub","//Book.epub","/Books//Book.epub","relative.epub","/Book.txt"}) {
   req.clear();reply.clear();req["op"]="export";req["path"]=path;
   UsbProgressSync::handle(req.as<JsonVariantConst>(),reply,renderer);assert(reply["error"]=="invalid_book");
 }
 run(R"({"op":"export","path":"/Unread.epub"})"); assert(reply["error"]=="no_saved_progress");
 run(R"({"op":"export","path":"/Book.epub"})");
 assert(reply["document"]==std::string(32,'a') && reply["device_id"]=="reader-1");
 assert(reply["progress"]=="/body/p[1]/text().42" && reply["metadata"]["title"]=="Títle");
 const auto revision=reply["revision"].as<uint32_t>();
 ++EpubReaderUtils::saved.pageNumber;
 run(R"({"op":"export","path":"/Book.epub"})"); assert(reply["revision"].as<uint32_t>()!=revision);
 --EpubReaderUtils::saved.pageNumber;
 auto apply=[&](uint32_t rev,const char* id="reader-1",double percent=.5) {
   req.clear();reply.clear();req["op"]="apply";req["path"]="/Book.epub";
   req["document"]=std::string(32,'a'); req["revision"]=rev;req["device_id"]=id;
   req["server"]="https://sync.example.com";req["username"]="test-user";
   req["progress"]="/body/p[1]/text().42"; req["percentage"]=percent;
   UsbProgressSync::handle(req.as<JsonVariantConst>(),reply,renderer);
 };
 apply(revision+1);assert(reply["error"]=="reader_changed" && saves==0);
 apply(revision,"other-reader");assert(reply["error"]=="reader_changed" && saves==0);
 apply(revision,"reader-1",2);assert(reply["error"]=="invalid_remote_progress" && saves==0);
 mapOk=false;apply(revision);assert(reply["error"]=="position_mapping_failed" && saves==0);mapOk=true;
 saveOk=false;apply(revision);assert(reply["error"]=="save_failed" && saves==0);saveOk=true;
 apply(revision);assert(reply["applied"]==true && saves==1 && retired==1 && cleared==1 && reply["settled"]==true);
 credentials=false;apply(revision);assert(reply["applied"]==true && reply["settled"]==false && retired==1);credentials=true;
 // Successful uploads/unchanged positions can settle without remapping or saving.
 auto acknowledge=[&](const char* user) {
   req.clear();reply.clear();req["op"]="settle";req["path"]="/Book.epub";
   req["document"]=std::string(32,'a');req["device_id"]="reader-1";req["revision"]=revision;
   req["server"]="https://sync.example.com";req["username"]=user;
   UsbProgressSync::handle(req.as<JsonVariantConst>(),reply,renderer);
 };
 acknowledge("another-user");assert(reply["settled"]==false && retired==1);
 acknowledge("test-user");assert(reply["settled"]==true && retired==2 && cleared==2);
 settleOk=false;acknowledge("test-user");assert(reply["settlement_error"]==true);settleOk=true;
 locator="/body/DocFragment[1]/body/"+std::string(300,'x')+"[1]";
 run(R"({"op":"export","path":"/Book.epub"})");assert(reply["progress"]==locator);
 locator="/body/p[1]/text().42";
 loadOk=false;run(R"({"op":"export","path":"/Book.epub"})");assert(reply["error"]=="position_mapping_failed");loadOk=true;
 EpubReaderUtils::saved.pageCount=0;run(R"({"op":"export","path":"/Book.epub"})");assert(reply["error"]=="position_mapping_failed");
 EpubReaderUtils::saved.pageCount=10;metadata=false;KOREADER_STORE.method=DocumentMatchMethod::BINARY;
 run(R"({"op":"export","path":"/Book.epub"})");assert(reply["document"]==std::string(32,'b') && reply["metadata"].isNull());
 paths.push_back("/New.epub");
 run(R"({"op":"books","cursor":0})");
 assert(reply["books"].size()==2 && reply["books"][1]=="/New.epub");
 paths.resize(40,"/Note.txt");
 run(R"({"op":"books","cursor":0})"); assert(reply["done"]==false && reply["next"]==1);
 run(R"({"op":"books","cursor":1})"); assert(reply["done"]==true);
 run(R"({"op":"books","cursor":0})"); assert(reply["done"]==false);
 clockMs+=30000;
 UsbProgressSync::expireBookList(true);
 run(R"({"op":"books","cursor":1})"); assert(reply["error"]=="invalid_cursor");
 run(R"({"op":"books","cursor":0})"); assert(reply["done"]==false);
 UsbProgressSync::expireBookList(false);
 run(R"({"op":"books","cursor":1})"); assert(reply["error"]=="invalid_cursor");
 paths.resize(3);
 credentials=false;
 run(R"({"op":"extras_books","cursor":0})");assert(reply["books"].size()==2);
 run(R"({"op":"extras","kind":"global","server":"https://sync.example.com","username":"usb-user","stats":true,"clippings":true})");
 assert(reply["stats"]["result"]=="ok" && reply["device_id"]=="reader-1" && statsCalls==1);
 run(R"({"op":"extras","kind":"book","path":"/Unread.epub","server":"https://sync.example.com","username":"usb-user","stats":true,"clippings":true})");
 assert(reply["stats"]["result"]=="ok" && reply["clippings"]["result"]=="ok" && clippingCalls==1);
 run(R"({"op":"extras","kind":"book","path":"/.crosspoint/book.epub","server":"https://sync.example.com","username":"usb-user","stats":true,"clippings":true})");
 assert(reply["error"]=="invalid_book" && clippingCalls==1);
 run(R"({"op":"extras","kind":"global","device_id":"other-reader","server":"https://sync.example.com","username":"usb-user","stats":true,"clippings":true})");
 assert(reply["error"]=="reader_changed");
 run(R"({"op":"extras","kind":"global","server":"","username":"usb-user","stats":true,"clippings":true})");
 assert(reply["error"]=="invalid_account");
 credentials=true;
 run(R"({"op":"extras","kind":"global","server":"https://other.example.com","username":"usb-user","stats":true,"clippings":true})");
 assert(reply["stats"]["result"]=="account_mismatch");
 run(R"({"op":"extras","kind":"book","path":"/Book.epub","server":"https://sync.example.com","username":"test-user","stats":true,"clippings":true})");
 assert(reply["stats"]["result"]=="ok" && reply["clippings"]["result"]=="ok");
 assert(SyncPending::clearedBits==(SyncPending::STATS_BIT | SyncPending::CLIPPINGS_BIT));
 KOREADER_STORE.method=DocumentMatchMethod::FILENAME;
 run(R"({"op":"export","path":"/Unread.epub","allow_recovery":true})");
 assert(reply["local_unavailable"]==true && reply["has_saved"]==false && reply["revision"]==0);
 auto compare=[&](const char* operation,const char* path,uint32_t rev,bool hasSaved,double remote,bool hasLocator=true) {
   req.clear();reply.clear();req["op"]=operation;req["path"]=path;
   req["document"]=std::string(32,'a');req["device_id"]="reader-1";req["revision"]=rev;
   req["allow_recovery"]=true;req["has_saved"]=hasSaved;req["percentage"]=remote;
   if(hasSaved) req["local_percentage"]=0.25;
   if(hasLocator) req["progress"]="/body/p[1]/text().42";
   UsbProgressSync::handle(req.as<JsonVariantConst>(),reply,renderer);
 };
 compare("compare","/Unread.epub",0,false,.5);assert(reply["choice"]=="ask");
 compare("apply","/Unread.epub",0,true,.5);assert(reply["error"]=="reader_changed");
 compare("apply","/Unread.epub",0,false,.5,false);assert(reply["error"]=="position_mapping_failed");
 compare("apply","/Unread.epub",0,false,.5);assert(reply["applied"]==true);
 EpubReaderUtils::saved.spineIndex=5;
 run(R"({"op":"export","path":"/Book.epub","allow_recovery":true})");
 assert(reply["local_unavailable"]==true && reply["has_saved"]==true);
 compare("apply","/Book.epub",reply["revision"].as<uint32_t>(),true,.5);assert(reply["applied"]==true);
 EpubReaderUtils::saved.spineIndex=1;
 compare("compare","/Book.epub",revision,true,.2505);assert(reply["choice"]=="match");
 compare("compare","/Book.epub",revision,true,.4);assert(reply["choice"]=="server");
 compare("compare","/Book.epub",revision,true,.1);assert(reply["choice"]=="reader");
 KOREADER_STORE.behavior=KOReaderSyncBehavior::ASK;
 compare("compare","/Book.epub",revision,true,.1);assert(reply["choice"]=="ask");
 puts("USB progress handler checks passed");
}
'''
with tempfile.TemporaryDirectory(prefix="crossink-usb-progress-") as tmp:
    tmp = Path(tmp)
    (tmp / "Common.h").write_text(STUBS)
    fs_helpers = (ROOT / "lib/FsHelpers/FsHelpers.cpp").read_text()
    normalize = fs_helpers[fs_helpers.index("std::string normalisePath("):fs_helpers.index("bool naturalLess(")]
    (tmp / "FsHelpers.cpp").write_text('#include "Common.h"\nnamespace FsHelpers {\n' + normalize + '}\n')
    for name in ["Epub.h", "FsHelpers.h", "KOReaderCredentialStore.h", "KOReaderDocumentId.h", "KOReaderSyncClient.h", "Logging.h", "Memory.h", "ProgressMapper.h", "Utf8.h", "CrossPointSettings.h", "network/ReadingSyncUpload.h", "network/ClippingsUpload.h", "network/UsbSerialFileTransfer.h", "sync/SyncParked.h", "sync/SyncPending.h", "util/FolderBookIterator.h", "activities/home/RecentBookProgress.h", "activities/reader/EpubReaderUtils.h", "activities/reader/KOReaderSyncActivity.h"]:
        stub = tmp / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text('#include "Common.h"\n')
    (tmp / "test.cpp").write_text(TEST)
    binary = tmp / "test"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I"+str(tmp), "-I"+str(args.arduinojson), "-I"+str(ROOT / "src"), str(ROOT / "src/network/UsbProgressSync.cpp"), str(tmp / "FsHelpers.cpp"), str(tmp / "test.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
