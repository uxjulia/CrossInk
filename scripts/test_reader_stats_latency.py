#!/usr/bin/env python3
"""Exercise production reader interval methods with slow storage and a fake clock."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor] + '\n'


FIXTURE = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include "ReadingSessionStats.h"
#include <CrossPointSettings.h>
#include <HalClock.h>
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
unsigned long nowMs=1000;
unsigned long millis(){return nowMs;}
int storageCalls=0, panelCalls=0;
bool failCheckpoint=false;
bool* inputAfterWrite=nullptr;
BookReadingStats savedBook;
GlobalReadingStats savedGlobal;
extern bool statsLocked;
void storage(){assert(statsLocked);if(inputAfterWrite)*inputAfterWrite=true;++storageCalls;nowMs+=3000;halClock.second+=3;}
bool BookReadingStats::save(const std::string&)const {
  storage(); if(failCheckpoint)return false; savedBook=*this;return true;
}
bool GlobalReadingStats::save()const {storage();savedGlobal=*this;return true;}
bool GlobalReadingStats::loadForUpload(GlobalReadingStats& s){s=savedGlobal;return true;}
GlobalReadingStats GlobalReadingStats::load(){return savedGlobal;}
uint32_t GlobalReadingStats::localResetRevision(){return 0;}
struct IntervalSink {
  ReadingStatsDateTime begin, lastEnd;
  uint8_t offset=255,lastOffset=255;
  uint32_t accepted=0,rev=0;
  void start(const ReadingStatsDateTime& t,uint8_t o){begin=t;offset=o;}
  bool accept(const ReadingStatsDateTime& end,uint8_t o,uint32_t seconds,uint32_t,bool allowCheckpoint){
    assert(isValidReadingStatsInterval(begin,end,offset,o,seconds));
    lastEnd=end;lastOffset=o;accepted+=seconds;return !allowCheckpoint || checkpointIfDue();
  }
  bool accept(const char*,const ReadingStatsDateTime& end,uint8_t o,uint32_t seconds,bool allowCheckpoint){
    return accept(end,o,seconds,0,allowCheckpoint);
  }
  uint32_t revision()const{return rev;}
  uint32_t checkpointed=0;
  bool checkpointIfDue(){
    if(accepted-checkpointed<10)return true; // Lower threshold for timing tests.
    storage();checkpointed=accepted;++rev;return true;
  }
};
struct Book {std::string getPath()const{return "/book";}std::string getCachePath()const{return "/cache";}};
bool statsLocked=false;
int statsLockAcquires=0;
namespace DailyReadingStats {bool checkpointIfDue(){assert(statsLocked);return true;}}
struct Input {bool pending=false;bool hasPendingActivity()const{return pending;}};
struct Queue {bool pending=false;bool hasPending()const{return pending;}};
struct RenderLock {
  enum class Mode {Try};
  bool owns=false;
  explicit RenderLock(Mode){owns=!statsLocked;if(owns)statsLocked=true;}
  bool ownsLock()const{return owns;}
  template<class T> explicit RenderLock(T&){assert(!statsLocked);statsLocked=true;owns=true;++statsLockAcquires;}
  ~RenderLock(){if(owns)statsLocked=false;}
  static bool heldByCurrentTask(){return statsLocked;}
};
struct Reader {
  Book book;Book* epub=&book;Book* xtc=&book;
  BookReadingStats stats;GlobalReadingStats globalStats;
  ReadingSessionDates readingSessionDates;
  DeferredReadingStatsInterval deferredReadingStats;
  ReadingStatsSaveDelay statsSaveDelay;
  Input mappedInput;
  Queue pendingManualPageTurns;
  bool shortcutPageTurnPending=false,shortcutPreviousPagePending=false;
  IntervalSink dailyReadingSession,readingSessionJournal;
  uint32_t sessionReadingSeconds=0,statsCheckpointRevision=0;
  unsigned long pageShownAtMs=1000;
  bool activeFootnotePreview=false,acceptTime=true;
  bool currentPageReadingSecondsForStats(uint32_t& seconds,const char*)const {
    seconds=pageShownAtMs && acceptTime ? (nowMs-pageShownAtMs)/1000 : 0;return seconds>0;
  }
  void recordCurrentPageReadingTime(const char*,bool=false);
  void flushDeferredReadingStats(bool=true);
  void checkpointReadingStatsWhenIdle();
  void startDailyReadingInterval();
  void render(bool error=false);
};
'''

TESTS = r'''
int main(){
  Reader r;
  halClock.second=0;r.startDailyReadingInterval();
  nowMs=11000;halClock.second=10;
  r.recordCurrentPageReadingTime("touch",true);
  assert(storageCalls==0 && r.sessionReadingSeconds==10);
  r.render();
  assert(panelCalls==1 && r.dailyReadingSession.accepted==10 && storageCalls==0);
  assert(r.readingSessionJournal.lastEnd.second==10 && r.dailyReadingSession.begin.second==10);
  // Touch/button held or queued input keeps even an overdue checkpoint waiting.
  r.mappedInput.pending=true;nowMs=13000;r.checkpointReadingStatsWhenIdle();
  assert(storageCalls==0);
  r.mappedInput.pending=false;nowMs=13999;r.checkpointReadingStatsWhenIdle();
  assert(storageCalls==0);
  // A second accepted turn coalesces without storage, preserving all seconds.
  nowMs=14000;halClock.second=13;r.recordCurrentPageReadingTime("side",true);r.render();
  assert(storageCalls==0 && r.dailyReadingSession.accepted==13);
  r.pendingManualPageTurns.pending=true;r.shortcutPageTurnPending=true;
  nowMs=16000;r.checkpointReadingStatsWhenIdle();assert(storageCalls==0);
  r.pendingManualPageTurns.pending=false;r.shortcutPageTurnPending=false;
  nowMs=16999;r.checkpointReadingStatsWhenIdle();assert(storageCalls==0);
  // Rendering owns the lock: idle processing must neither block nor save.
  nowMs=17000;{RenderLock lock(r);r.checkpointReadingStatsWhenIdle();}assert(storageCalls==0);
  inputAfterWrite=&r.mappedInput.pending;
  nowMs=18000;r.checkpointReadingStatsWhenIdle();
  assert(storageCalls==1 && r.statsCheckpointRevision==0);
  inputAfterWrite=nullptr;r.mappedInput.pending=false;
  nowMs+=999;r.checkpointReadingStatsWhenIdle();assert(storageCalls==1);
  nowMs+=1;r.checkpointReadingStatsWhenIdle();
  assert(storageCalls==3 && r.statsCheckpointRevision==1);
  const int written=storageCalls;
  r.checkpointReadingStatsWhenIdle();assert(storageCalls==written);
  // Error/end-screen paths accept the interval, but defer the periodic write.
  nowMs=1000;halClock.second=0;storageCalls=0;
  Reader error;error.startDailyReadingInterval();nowMs=11000;halClock.second=10;
  error.recordCurrentPageReadingTime("front",true);error.render(true);
  assert(error.deferredReadingStats.seconds==0 && error.dailyReadingSession.accepted==10 && storageCalls==0);
  nowMs=12000;error.checkpointReadingStatsWhenIdle();assert(storageCalls==3);
  // Restart/exit drains before render and bypasses the quiet window.
  nowMs=1000;halClock.second=0;storageCalls=0;
  Reader exit;exit.startDailyReadingInterval();nowMs=11000;halClock.second=10;
  exit.recordCurrentPageReadingTime("side",true);exit.mappedInput.pending=true;
  exit.recordCurrentPageReadingTime("restart_checkpoint");
  assert(exit.dailyReadingSession.accepted==10 && storageCalls==4);
  exit.recordCurrentPageReadingTime("session_commit");assert(storageCalls==4);
  // Preserve captured timezone and leave failed book/global snapshots retryable.
  nowMs=1000;halClock.second=0;storageCalls=0;
  Reader failed;failed.startDailyReadingInterval();nowMs=11000;halClock.second=10;
  failed.recordCurrentPageReadingTime("touch",true);SETTINGS.clockUtcOffsetQ=52;
  failed.render(true);assert(failed.dailyReadingSession.lastOffset==48);SETTINGS.clockUtcOffsetQ=48;
  failCheckpoint=true;nowMs=12000;failed.checkpointReadingStatsWhenIdle();
  assert(failed.statsCheckpointRevision==0);failCheckpoint=false;
  nowMs+=1000;failed.checkpointReadingStatsWhenIdle();assert(failed.statsCheckpointRevision==1);
  Reader idle;idle.acceptTime=false;storageCalls=0;
  idle.recordCurrentPageReadingTime("front",true);idle.render(true);
  assert(storageCalls==0 && idle.sessionReadingSeconds==0);
  ReadingStatsSaveDelay wrap;wrap.defer(UINT32_MAX-499);
  assert(!wrap.ready(499));assert(wrap.ready(500));
  std::cout<<"quiet-window reading stats checks passed\n";
}
'''

for kind in ('Epub', 'Xtc'):
    source = (ROOT / f'src/activities/reader/{kind}ReaderActivity.cpp').read_text()
    methods = ''.join(method(source, f'void {kind}ReaderActivity::{name}(')
                      for name in ('recordCurrentPageReadingTime', 'flushDeferredReadingStats',
                                   'startDailyReadingInterval', 'checkpointReadingStatsWhenIdle'))
    render = method(source, f'void {kind}ReaderActivity::render(')
    # Execute the production early-return guard around a fake display operation.
    guard = render[render.index('{')+1:render.index('  } flushStats{*this};')+len('  } flushStats{*this};')]
    methods += 'void Reader::render(bool error){ RenderLock renderLock(*this);' + guard + r'''
      ++panelCalls;
      if(error)return;
      pageShownAtMs=millis();startDailyReadingInterval();
    }
    '''
    methods = methods.replace(f'{kind}ReaderActivity', 'Reader')
    # Keep page-turn integration covered as well as the extracted methods.
    if kind == 'Epub':
        turn = method(source, 'void EpubReaderActivity::pageTurn(')
        assert turn.count('recordCurrentPageReadingTime(source, /*deferStorage=*/true)') == 2
        assert source.index('startDailyReadingInterval();', source.index('lastRenderCompleteMs = millis();')) > source.index('renderContents(std::move(p)')
    else:
        for reason in ('side_long_press', 'page_back', 'page_forward'):
            assert f'recordCurrentPageReadingTime("{reason}", /*deferStorage=*/true)' in source
        assert render.index('renderPage(pageToRender)') < render.index('startDailyReadingInterval()')
    with tempfile.TemporaryDirectory(prefix='reader-stats-latency-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        cpp.write_text(FIXTURE + methods + TESTS)
        exe = Path(tmp) / 'test'
        includes = ['test/daily_reading_stats/stubs', 'test/sync_files/stubs', 'lib/Memory',
                    'src/activities/reader', 'lib']
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        *[f'-I{ROOT / p}' for p in includes], str(cpp),
                        str(ROOT / 'src/activities/reader/ReadingSessionStats.cpp'),
                        str(ROOT / 'src/activities/reader/ReadingStatsUtils.cpp'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print(f'{kind}: production page-turn persistence order passed')

# Inspecting optional-work readiness must not run stateful tap routing. Compile
# the production peek in touch/button-only and hardware/simulator variants.
INPUT_FIXTURE = r'''
#include <array>
#include <cassert>
#include <cstddef>
struct Gpio {
  bool pending=false,pressed=false,released=false,touchEdge=false,touchHeld=false;
  bool hasPendingButtonInput()const{return pending;}
  bool wasAnyPressed()const{return pressed;}
  bool wasAnyReleased()const{return released;}
  bool wasTouchActivity()const{return touchEdge;}
  bool isTouchHeldAt(float&,float&)const{return touchHeld;}
};
struct MappedInputManager {
  static constexpr size_t BUTTON_COUNT=9;
  enum class Button {Back};
  Gpio gpio;
  std::array<bool,BUTTON_COUNT> held{},injectedReleases{},simulatorPressed{},simulatorReleased{};
  struct {bool pressed=false,pressedThisFrame=false,releasedThisFrame=false,longPressFired=false;} simulatorTouch;
  bool isPhysicalPressed(Button b)const{return held[size_t(b)];}
  bool wasAnyPressed()const{assert(false && "peek must not route mapped input");return false;}
  bool wasAnyReleased()const{assert(false && "peek must not consume tap suppression");return false;}
  bool hasPendingActivity()const;
};
'''
INPUT_TEST = r'''
int main(){
  MappedInputManager m;assert(!m.hasPendingActivity());
  for(bool* state:{&m.gpio.pressed,&m.gpio.released,&m.held[4],&m.injectedReleases[6]}){
    *state=true;assert(m.hasPendingActivity());assert(m.hasPendingActivity());assert(*state);*state=false;
  }
#ifndef SIMULATOR
  m.gpio.pending=true;assert(m.hasPendingActivity());m.gpio.pending=false;
#else
  m.simulatorReleased[2]=true;assert(m.hasPendingActivity());m.simulatorReleased[2]=false;
#if CROSSINK_APP_CAP_TOUCH
  m.simulatorTouch.releasedThisFrame=true;m.simulatorTouch.longPressFired=true;
  assert(m.hasPendingActivity());assert(m.hasPendingActivity());
  assert(m.simulatorTouch.releasedThisFrame && m.simulatorTouch.longPressFired);
  m.simulatorTouch.releasedThisFrame=false;
#endif
#endif
#if CROSSINK_APP_CAP_TOUCH
  m.gpio.touchEdge=true;assert(m.hasPendingActivity());m.gpio.touchEdge=false;
  m.gpio.touchHeld=true;assert(m.hasPendingActivity());m.gpio.touchHeld=false;
#endif
  assert(!m.hasPendingActivity());
}
'''
peek = method((ROOT / 'src/MappedInputManager.cpp').read_text(),
              'bool MappedInputManager::hasPendingActivity(')
with tempfile.TemporaryDirectory(prefix='reader-input-peek-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(INPUT_FIXTURE + peek + INPUT_TEST)
    for touch in (0, 1):
        for sim in (False, True):
            exe = Path(tmp) / f'test-{touch}-{sim}'
            subprocess.run(['c++', '-std=c++20', f'-DCROSSINK_APP_CAP_TOUCH={touch}',
                            *(['-DSIMULATOR'] if sim else []), str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
print('Input readiness peek: four non-consuming configurations passed')
