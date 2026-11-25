#!/usr/bin/env python3
"""Execute Picco's production charge overlay with a nonrecursive render mutex."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def definition(source, signature):
    start = source.index(signature)
    at = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[start:at]


FIXTURE = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#define LOG_ERR(...) ((void)0)
constexpr int SMALL_FONT_ID=0;
namespace HalDisplay {constexpr int FAST_REFRESH=1;}
bool held=false, usb=true, wake=false, allocation=true;
unsigned locks=0, paints=0, restores=0, holds=0;
struct RenderLock {
 RenderLock(){assert(!held);held=true;++locks;}
 ~RenderLock(){assert(held);held=false;}
 static bool heldByCurrentTask(){return held;}
};
struct Rect {int x,y,width,height;};
using HeapByteBuffer=std::unique_ptr<uint8_t[]>;
HeapByteBuffer makePsramByteBufferNoThrow(size_t size){return allocation?std::make_unique<uint8_t[]>(size):nullptr;}
struct Metrics {int batteryWidth=20,batteryHeight=10;};
struct UITheme {
 static UITheme& getInstance(){static UITheme t;return t;}
 const Metrics& getMetrics(){static Metrics m;return m;}
};
struct Renderer {
 std::array<uint8_t,16> buffer{};
 uint8_t* getFrameBuffer(){return buffer.data();}
 size_t getBufferSize(){return buffer.size();}
 void fillRect(int,int,int,int,bool){assert(held);}
 void displayBuffer(int){assert(held);}
 void drawText(int,int,int,const char*){assert(held);}
 int getTextWidth(int,const char*){return 20;}
 int getLineHeight(int){return 12;}
 int getScreenWidth(){return 480;}
 int getScreenHeight(){return 800;}
 void getOrientedViewableTRBL(int*t,int*r,int*b,int*l){*t=*r=*b=*l=0;}
 bool displayRetainedGrayscaleOverlay(int,int,int,int,bool restore=false){
   assert(held);if(restore)++restores;else ++paints;return true;
 }
} renderer;
struct BaseTheme {
 static constexpr int batteryPercentSpacing=4;
 static void drawBatteryOutline(Renderer&,int,int,int,int){assert(held);}
 static void drawBatteryLightningBolt(Renderer&,int,int,bool){assert(held);}
};
struct Gpio {bool isUsbConnected(){return usb;}} gpio;
struct Power {
 bool holdWhileUsbPowered(Gpio&,void(*draw)(void*,bool,uint16_t)=nullptr,void*context=nullptr){
  ++holds;if(draw){draw(context,true,63);draw(context,false,64);}return wake;
 }
} powerManager;
'''

CASES = r'''
int main(){
 for(bool outer:{false,true})for(bool button:{false,true})for(bool memory:{false,true}){
  assert(!held);wake=button;allocation=memory;locks=paints=restores=holds=0;
  {
   std::optional<RenderLock> owner;if(outer)owner.emplace();
   assert(holdSleepWhileUsbPowered()==button);
   assert(held==outer);
   assert(holds==1 && paints==(memory?2u:0u));
   assert(restores==(!button&&memory?1u:0u));
   assert(locks==(outer?1u:memory?(button?2u:3u):0u));
  }
  assert(!held);
 }
 usb=false;holds=0;assert(!holdSleepWhileUsbPowered());assert(holds==0);
 std::puts("PASS: Picco USB sleep callbacks borrow render lock; wake, restore, no-memory and disconnect paths");
}
'''


def main():
    source = (ROOT / "src/main.cpp").read_text()
    production = definition(source, "struct SleepChargeOverlay {") + ";\n"
    production += definition(source, "static bool holdPiccoUsbSleep(") + "\n"
    production += definition(source, "static bool holdSleepWhileUsbPowered(bool monochrome = false) {")
    with tempfile.TemporaryDirectory(prefix="crossink-picco-sleep-lock-") as temp:
        path = Path(temp)
        (path / "test.cpp").write_text(FIXTURE + production + CASES)
        subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(path / "test.cpp"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
