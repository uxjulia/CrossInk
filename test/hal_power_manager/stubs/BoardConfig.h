#pragma once
#include <Arduino.h>
#define FREEINK_CAP_TOUCH 0
#define FREEINK_CAP_USB_MSC 0
namespace BoardConfig {
enum class Board { XteinkX3, XteinkX3Uc8279, XteinkX4 };
struct Profile {
  Board board = Board::XteinkX3;
  int batteryAdc = -1;
  struct {
    int8_t power = 2;
  } input;
  struct {
    int gaugeAddr = 1;
  } batteryGauge;
  struct {
    int8_t latch0 = -1;
    int8_t latch1 = -1;
  } power;
};
inline Profile ACTIVE;
inline bool latchConflictsWithBus(int) { return false; }
}  // namespace BoardConfig
