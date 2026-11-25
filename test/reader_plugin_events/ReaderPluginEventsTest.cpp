#include <Arduino.h>

#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "activities/RenderLock.h"
#include "activities/reader/PluginReaderEvents.h"
#include "util/PluginEvents.h"

namespace {
bool renderBusy = false;
unsigned int lockAttempts = 0;
std::vector<pluginevents::Event> emitted;
}  // namespace
// Model the real RenderLock contract: a blocking acquisition while the renderer
// owns it is an error in this test, rather than hanging the test indefinitely.
RenderLock::RenderLock(Mode mode) {
  ++lockAttempts;
  if (renderBusy && mode == Mode::Blocking) throw std::runtime_error("input waited for rendering");
  isLocked = !renderBusy;
  if (isLocked) renderBusy = true;
}
RenderLock::~RenderLock() {
  if (isLocked) renderBusy = false;
}
namespace pluginevents {
bool anySubscriber(Event) { return true; }
void emit(Event event, const Var*, size_t) { emitted.push_back(event); }
}  // namespace pluginevents

int main() {
  const std::string book = "/Books/example.epub";
  {
    PluginReaderEvents events;
    events.pollWhenIdle(book);
    assert(emitted.empty());  // Don't announce an unread/unrendered book.
    events.rendered(100);
    renderBusy = true;
    events.pollWhenIdle(book);  // Input must continue while a prior page renders.
    assert(emitted.empty());
    renderBusy = false;
    events.pollWhenIdle(book);
    assert(emitted.size() == 1 && emitted[0] == pluginevents::Event::ReaderOpen);
    const auto attempts = lockAttempts;
    renderBusy = true;
    events.pollWhenIdle(book);  // Already announced: no repeated lock or event.
    assert(lockAttempts == attempts && emitted.size() == 1);
    renderBusy = false;
    RenderLock exitLock;
    events.exit(book, 10);
    assert(emitted.size() == 2 && emitted[1] == pluginevents::Event::ReaderExit);
  }
  emitted.clear();
  {
    PluginReaderEvents events;
    events.rendered(200);
    renderBusy = true;
    events.pollWhenIdle(book);
    renderBusy = false;
    // Leaving before a deferred poll still delivers open before exit.
    RenderLock exitLock;
    events.exit(book, 20);
    assert(emitted.size() == 2);
    assert(emitted[0] == pluginevents::Event::ReaderOpen);
    assert(emitted[1] == pluginevents::Event::ReaderExit);
  }
  emitted.clear();
  {
    PluginReaderEvents events;
    events.rendered(300);
    events.pollWhenIdle("");
    assert(emitted.empty());
    // Sleep preparation already owns the render lock and must not try-lock it
    // recursively or drop the first open event while flushing the session.
    RenderLock sleepLock;
    events.poll(book);
    assert(emitted.size() == 1 && emitted[0] == pluginevents::Event::ReaderOpen);
  }
  std::puts("Reader plugin deferred polling: passed");
}
