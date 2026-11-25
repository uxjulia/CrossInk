#include "PluginReaderEvents.h"

#include <Arduino.h>
#include <KOReaderDocumentId.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "../RenderLock.h"
#include "util/PluginEvents.h"
void PluginReaderEvents::rendered(int progressBasisPoints) {
  renderedPage = true;
  session.onRenderComplete(millis(), trustedtime::trustedNow(), progressBasisPoints);
}
void PluginReaderEvents::pollWhenIdle(const std::string& book) {
  // opened belongs to the main task. Once emitted, avoid even a try-lock.
  if (opened) return;
  RenderLock lock(RenderLock::Mode::Try);
  if (lock.ownsLock()) poll(book);
}
void PluginReaderEvents::poll(const std::string& book) {
  if (!renderedPage || opened || book.empty()) return;
  opened = true;
  const pluginevents::Var vars[] = {{"book", book.c_str()}};
  pluginevents::emit(pluginevents::Event::ReaderOpen, vars, 1);
}
void PluginReaderEvents::exit(const std::string& book, int percentValue) {
  poll(book);
  flush(book);
  if (!opened) return;
  char percent[8];
  snprintf(percent, sizeof(percent), "%d", percentValue);
  const pluginevents::Var vars[] = {{"book", book.c_str()}, {"percent", percent}};
  pluginevents::emit(pluginevents::Event::ReaderExit, vars, 2);
  opened = false;
}
void PluginReaderEvents::flush(const std::string& book) {
  if (!session.isEmitWorthy() || !pluginevents::anySubscriber(pluginevents::Event::ReaderSession)) {
    session.reset();
    return;
  }

  const std::string document = KOReaderDocumentId::calculate(book);
  const bool validDocument =
      document.size() == 32 && std::all_of(document.begin(), document.end(), [](const unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      });
  if (validDocument) {
    char startTime[24];
    char endTime[24];
    char duration[16];
    char startProgress[8];
    char endProgress[8];
    snprintf(startTime, sizeof(startTime), "%lld", static_cast<long long>(session.startTime()));
    snprintf(endTime, sizeof(endTime), "%lld", static_cast<long long>(session.endTime()));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(session.durationSeconds()));
    snprintf(startProgress, sizeof(startProgress), "%u", session.startProgressBp());
    snprintf(endProgress, sizeof(endProgress), "%u", session.endProgressBp());
    const pluginevents::Var vars[] = {{"book", book.c_str()},           {"document", document.c_str()},
                                      {"start_time", startTime},        {"end_time", endTime},
                                      {"duration_seconds", duration},   {"start_progress_bp", startProgress},
                                      {"end_progress_bp", endProgress}, {"progress_scale", "10000"}};
    pluginevents::emit(pluginevents::Event::ReaderSession, vars, 8);
  }
  session.reset();
}
