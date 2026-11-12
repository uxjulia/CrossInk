#!/usr/bin/env python3
"""Run production restart/checkpoint methods with host persistence and lock fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor] + "\n"


FIXTURE = r'''
#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include "ReadingSessionStats.h"
#define LOG_ERR(...) ((void)0)
bool bookFails=false, globalFails=false, dailyFails=false, journalFails=false;
bool s_blockDestructiveSave=false, lockHeld=false;
int bookWrites=0, globalWrites=0, dailyFlushes=0, restarts=0, armed=0, transfers=0;
BookReadingStats savedBook;
GlobalReadingStats savedGlobal;
constexpr char GLOBAL_STATS_PATH[]="global";
namespace StatsStorage { std::string localPath(const std::string& path) { return path; } }
namespace DailyReadingStats { bool flush(){++dailyFlushes;return !dailyFails;} }
bool BookReadingStats::save(const std::string&)const {
  ++bookWrites; if(bookFails)return false; savedBook=*this; return true;
}
bool saveToFile(const GlobalReadingStats& stats,const char*,const char*) {
  ++globalWrites; if(globalFails)return false; savedGlobal=stats; return true;
}
bool GlobalReadingStats::loadForUpload(GlobalReadingStats& out){out=savedGlobal;return true;}
GlobalReadingStats GlobalReadingStats::load(){return savedGlobal;}
uint32_t GlobalReadingStats::localResetRevision(){return 0;}
struct RenderLock {
  template<class T> explicit RenderLock(T&){assert(!lockHeld);lockHeld=true;}
  ~RenderLock(){lockHeld=false;}
};
struct Book {std::string getPath()const{return "/book.epub";} std::string getCachePath()const{return "/cache";}};
struct Journal {bool checkpoint(){assert(lockHeld);return !journalFails;}};
struct Manager {
  void goToCalibreWireless(const std::string&){++transfers;}
  void goToJoinNetworkFileTransfer(const std::string&){++transfers;}
  void goToHotspotFileTransfer(const std::string&){++transfers;}
} activityManager;
void armSilentRestartReaderPageBuild(const std::string&,uint16_t,uint16_t,bool){++armed;}
void silentRestartToReader(){++restarts;}
struct Reader {
  Book book; Book* epub=&book; Book* xtc=&book;
  BookReadingStats stats; GlobalReadingStats globalStats;
  bool statsTrackingActive=true,pendingStatsCommit=false;
  ReadingSessionDates readingSessionDates;
  ReadingStatsDateTime pageStartLocalDateTime;
  uint32_t sessionReadingSeconds=60,currentPageSeconds=5;
  bool progressFails=false,linksFail=false,lowMemoryPartialRestartAttempted=false,automaticPageTurnActive=false;
  int currentSpineIndex=0,resumes=0,updates=0;
  Journal readingSessionJournal;
  Reader(){setReadingInterval({{2026,10,23},12,0,0},60);}
  void setReadingInterval(ReadingStatsDateTime start,uint32_t seconds){
    readingSessionDates.reset();
    readingSessionDates.start(start,48);
    pageStartLocalDateTime=start;
    addSecondsToReadingStatsDateTime(pageStartLocalDateTime,seconds);
    readingSessionDates.accept(pageStartLocalDateTime,48,seconds);
  }
  void recordCurrentPageReadingTime(const char*) {
    assert(lockHeld);
    if(statsTrackingActive){
      sessionReadingSeconds+=currentPageSeconds;
      readingSessionDates.start(pageStartLocalDateTime,48);
      auto end=pageStartLocalDateTime;
      addSecondsToReadingStatsDateTime(end,currentPageSeconds);
      readingSessionDates.accept(end,48,currentPageSeconds);
      pageStartLocalDateTime=end;
    }
    currentPageSeconds=0;
  }
  bool saveReadingProgress(){assert(lockHeld);return !progressFails;}
  bool flushQueuedProgress(){assert(lockHeld);return !progressFails;}
  bool saveProgress(int,int,int){assert(lockHeld);return !progressFails;}
  bool saveLinkStack(){assert(lockHeld);return !linksFail;}
  void flushPendingSyncMark(){}
  void resumeReadingPaceTimer(const char*){++resumes;}
  void resumeReadingStatsTimer(const char*){++resumes;}
  void requestUpdate(){++updates;}
};
struct EpubReaderActivity:Reader {
 bool checkpointReadingStatsBeforeRestart();bool saveProgressBeforeRestart();
 bool retryLayout(int targetPage,int savedPage,int pageCount,const char* stage);
 void network0();void network1();void network2();
};
struct XtcReaderActivity:Reader {
 bool checkpointReadingStatsBeforeRestart();bool saveProgressBeforeRestart();
 bool network0();bool network1();bool network2();bool network3();bool network4();bool network5();
};
void reset(){
 bookFails=globalFails=dailyFails=journalFails=s_blockDestructiveSave=false;
 bookWrites=globalWrites=dailyFlushes=restarts=armed=transfers=0;
 savedBook={};savedGlobal={};assert(!lockHeld);
}
'''

CASES = r'''
template<class T> void checkpoints(){
 reset();T r;
 assert(r.saveProgressBeforeRestart());
 assert(savedBook.totalReadingSeconds==65 && savedGlobal.totalReadingSeconds==65);
 assert(savedGlobal.totalSessions==1 && savedBook.sessionCount==1);
 assert(savedGlobal.readingHistoryAnchorDay==readingStatsDayIndex({2026,10,23}));
 assert(r.stats.totalReadingSeconds==0 && r.globalStats.totalReadingSeconds==0);
 assert(r.sessionReadingSeconds==65);
 assert(r.saveProgressBeforeRestart());
 assert(savedGlobal.totalReadingSeconds==65 && savedGlobal.totalSessions==1);
 // A cancelled handoff followed by ordinary exit must not count the preview twice.
 commitReadingSession(r.stats,r.globalStats,r.sessionReadingSeconds,&r.readingSessionDates);
 assert(r.globalStats.totalReadingSeconds==65 && r.globalStats.totalSessions==1);
 T resumed;resumed.stats=savedBook;resumed.globalStats=savedGlobal;
 resumed.sessionReadingSeconds=10;resumed.currentPageSeconds=5;
 resumed.setReadingInterval({{2026,10,23},12,1,5},10);
 assert(resumed.saveProgressBeforeRestart());
 assert(savedGlobal.totalReadingSeconds==80 && savedGlobal.totalSessions==1);
 for(int failure=0;failure<5;++failure){
  reset();T retry;
  bookFails=failure==0;globalFails=failure==1;dailyFails=failure==2;journalFails=failure==3;
  retry.progressFails=failure==4;
  assert(!retry.saveProgressBeforeRestart());
  assert(retry.sessionReadingSeconds==65 && retry.globalStats.totalReadingSeconds==0);
  bookFails=globalFails=dailyFails=journalFails=false;retry.progressFails=false;
  assert(retry.saveProgressBeforeRestart());
  assert(savedGlobal.totalReadingSeconds==65 && savedGlobal.totalSessions==1);
 }
 // Restart snapshots retain both dates when the current page crosses midnight.
 reset();T midnight;midnight.setReadingInterval({{2026,10,23},23,59,0},60);
 assert(midnight.saveProgressBeforeRestart());
 assert(savedGlobal.dayOfWeekSeconds[4]==60 && savedGlobal.dayOfWeekSeconds[5]==5);
 assert(savedBook.dayOfWeekSeconds==savedGlobal.dayOfWeekSeconds);
 const ReadingStatsDate today{2026,10,24};
 assert(computeReadingHistoryCurrentStreak(savedGlobal.readingHistoryAnchorDay,
                                         savedGlobal.readingHistoryBits,&today)==2);
 reset();T disabled;disabled.statsTrackingActive=false;disabled.sessionReadingSeconds=0;
 assert(disabled.saveProgressBeforeRestart());assert(bookWrites==0 && globalWrites==0);
 disabled.pendingStatsCommit=true;disabled.stats.totalReadingSeconds=50;disabled.globalStats.totalReadingSeconds=50;
 assert(disabled.saveProgressBeforeRestart());assert(savedGlobal.totalReadingSeconds==50);
}
int main(){
 checkpoints<EpubReaderActivity>();checkpoints<XtcReaderActivity>();
 for(int failure=-1;failure<5;++failure){
  reset();EpubReaderActivity reader;
  bookFails=failure==0;globalFails=failure==1;dailyFails=failure==2;journalFails=failure==3;
  reader.progressFails=failure==4;
  {RenderLock held(reader);
   const bool ok=reader.retryLayout(1,0,20,"test");
   assert(ok==(failure<0));assert(restarts==(failure<0));assert(armed==(failure<0));
  }
  if(failure>=0){
   assert(!reader.lowMemoryPartialRestartAttempted);
   bookFails=globalFails=dailyFails=journalFails=false;reader.progressFails=false;
   {RenderLock held(reader);assert(reader.retryLayout(1,0,20,"retry"));}
   assert(restarts==1 && armed==1);
  }
  assert(savedGlobal.totalReadingSeconds==65 && savedGlobal.totalSessions==1);
 }
 // An old snapshot with the same counts must not hide a failed global write.
 reset();EpubReaderActivity stale;assert(stale.saveProgressBeforeRestart());
 globalFails=true;stale.globalStats.totalPagesTurned=9;
 assert(!stale.saveProgressBeforeRestart());
 reset();EpubReaderActivity links;links.linksFail=true;assert(!links.saveProgressBeforeRestart());
 // Checked global saves still attempt the global write when daily persistence fails.
 reset();dailyFails=true;GlobalReadingStats g;g.totalReadingSeconds=100;
 assert(!g.save());assert(globalWrites==1 && savedGlobal.totalReadingSeconds==100);
 reset();s_blockDestructiveSave=true;assert(!g.save());assert(globalWrites==0);
 for(int failure=-1;failure<4;++failure){
  for(auto call:{&EpubReaderActivity::network0,&EpubReaderActivity::network1,&EpubReaderActivity::network2}){
   reset();EpubReaderActivity r;
   bookFails=failure==0;globalFails=failure==1;dailyFails=failure==2;journalFails=failure==3;
   (r.*call)();assert(transfers==(failure<0));assert(r.resumes==(failure>=0));
  }
  for(auto call:{&XtcReaderActivity::network0,&XtcReaderActivity::network1,&XtcReaderActivity::network2,
                &XtcReaderActivity::network3,&XtcReaderActivity::network4,&XtcReaderActivity::network5}){
   reset();XtcReaderActivity r;
   bookFails=failure==0;globalFails=failure==1;dailyFails=failure==2;journalFails=failure==3;
   assert((r.*call)());assert(transfers==(failure<0));assert(r.resumes==(failure>=0));
  }
 }
 std::cout<<"PASS: EPUB/XTC restart snapshots, failure/retry, disabled/pending tracking, "
             "resume without double counting, low-memory lock ownership, and nine network handoff guards\n";
}
'''


def main():
    sources = {reader: (ROOT / f"src/activities/reader/{reader}.cpp").read_text()
               for reader in ("EpubReaderActivity", "XtcReaderActivity")}
    methods = ""
    for reader, source in sources.items():
        for name in ("checkpointReadingStatsBeforeRestart", "saveProgressBeforeRestart"):
            methods += method(source, f"bool {reader}::{name}(")
    epub = sources["EpubReaderActivity"]
    low_memory = method(epub, "  const auto restartForLowMemoryLayout = [this](")
    methods += "bool EpubReaderActivity::retryLayout(int targetPage,int savedPage,int pageCount,const char* stage) "
    methods += low_memory[low_memory.index("{"):]
    for reader, source in sources.items():
        prefixes = (["CrossPointSettings::LONG_MENU_"] if reader == "EpubReaderActivity" else
                    ["CrossPointSettings::SHORT_PWRBTN::", "CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_"])
        index = 0
        for prefix in prefixes:
            for action in ("CALIBRE_WIRELESS", "JOIN_NETWORK", "CREATE_HOTSPOT"):
                # Short-action cases in handleReaderShortcut are the last occurrence;
                # the earlier chord mapper only translates IDs and has no handoff.
                start = source.rindex(f"case {prefix}{action}:")
                start = source.index(":\n", start) + 2
                end = source.index("    case ", start)
                result = "void" if reader == "EpubReaderActivity" else "bool"
                methods += f"{result} {reader}::network{index}() {{ switch(0){{case 0:\n"
                methods += source[start:end] + "}}\n"
                index += 1
    global_stats = (ROOT / "src/activities/reader/GlobalReadingStats.cpp").read_text()
    methods += method(global_stats, "bool GlobalReadingStats::save()")
    with tempfile.TemporaryDirectory(prefix="crossink-restart-stats-") as directory:
        source = Path(directory) / "restart.cpp"
        binary = Path(directory) / "restart"
        source.write_text(FIXTURE + methods + CASES)
        subprocess.run(["c++", "-std=c++20", "-I" + str(ROOT / "src/activities/reader"),
                        "-I" + str(ROOT / "test/daily_reading_stats/stubs"), "-I" + str(ROOT / "lib/Memory"),
                        str(source), str(ROOT / "src/activities/reader/ReadingSessionStats.cpp"),
                        str(ROOT / "src/activities/reader/ReadingStatsUtils.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
