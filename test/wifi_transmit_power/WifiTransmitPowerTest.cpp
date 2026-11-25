#include <WifiTransmitPower.h>
#include <esp_wifi.h>

#include <cassert>
#include <string>
#include <vector>

namespace {
std::vector<std::string> calls;
int8_t radioPower = 80;
esp_err_t startResult = ESP_OK;
esp_err_t readResult = ESP_OK;
esp_err_t setResult = ESP_OK;
bool running = false;
}  // namespace

extern "C" esp_err_t __wrap_esp_wifi_start();

extern "C" esp_err_t __real_esp_wifi_start() {
  calls.emplace_back("start");
  running = startResult == ESP_OK;
  return startResult;
}

extern "C" esp_err_t esp_wifi_get_max_tx_power(int8_t* power) {
  calls.emplace_back("get");
  assert(running);
  if (readResult == ESP_OK) *power = radioPower;
  return readResult;
}

extern "C" esp_err_t esp_wifi_set_max_tx_power(int8_t power) {
  calls.emplace_back("set");
  assert(running);
  if (setResult == ESP_OK) radioPower = power;
  return setResult;
}

extern "C" esp_err_t esp_wifi_stop() {
  calls.emplace_back("stop");
  running = false;
  return ESP_OK;
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string test = argv[1];
  if (test == "default") {
    assert(WifiTransmitPower::getLimit() == 0);
    assert(__wrap_esp_wifi_start() == ESP_OK);
    assert(radioPower == 80);
    assert(calls == std::vector<std::string>{"start"});
  } else if (test == "before-scan") {
    WifiTransmitPower::setLimit(34);
    if (__wrap_esp_wifi_start() == ESP_OK) {
      assert(radioPower == 34);
      calls.emplace_back("scan");
    }
    assert((calls == std::vector<std::string>{"start", "get", "set", "scan"}));
  } else if (test == "restore") {
    for (const uint8_t power : {34, 60, 20, 8, 0}) {
      WifiTransmitPower::setLimit(power);
      assert(__wrap_esp_wifi_start() == ESP_OK);
      assert(radioPower == (power == 0 ? 80 : power));
      // Model the observed driver behavior: stopping does not reset TX power.
      assert(esp_wifi_stop() == ESP_OK);
    }
  } else if (test == "board-ceiling") {
    radioPower = 44;
    WifiTransmitPower::setLimit(60);
    assert(__wrap_esp_wifi_start() == ESP_OK);
    assert(radioPower == 44);
    WifiTransmitPower::setLimit(34);
    assert(__wrap_esp_wifi_start() == ESP_OK);
    assert(radioPower == 34);
    WifiTransmitPower::setLimit(0);
    assert(__wrap_esp_wifi_start() == ESP_OK);
    assert(radioPower == 44);
  } else if (test == "profile-default") {
    assert(__wrap_esp_wifi_start() == ESP_OK);
    assert(radioPower == 60);
    assert((calls == std::vector<std::string>{"start", "get", "set"}));
  } else if (test == "profile-restore") {
    for (const uint8_t power : {0, 34, 0, 60, 8, 0}) {
      WifiTransmitPower::setLimit(power);
      assert(__wrap_esp_wifi_start() == ESP_OK);
      assert(radioPower == (power == 0 ? 60 : power));
      assert(esp_wifi_stop() == ESP_OK);
    }
  } else if (test == "profile-lower-radio") {
    radioPower = 44;
    assert(__wrap_esp_wifi_start() == ESP_OK);
    assert(radioPower == 44);
    WifiTransmitPower::setLimit(60);
    assert(__wrap_esp_wifi_start() == ESP_OK);
    assert(radioPower == 44);
  } else if (test == "profile-read-failure") {
    readResult = ESP_FAIL;
    assert(__wrap_esp_wifi_start() == ESP_FAIL);
    assert(!running);
    assert((calls == std::vector<std::string>{"start", "get", "stop"}));
  } else if (test == "bad-setting") {
    for (const uint8_t value : {1, 15, 33, 80, 255}) {
      WifiTransmitPower::setLimit(value);
      assert(WifiTransmitPower::getLimit() == 0);
      assert(__wrap_esp_wifi_start() == ESP_OK);
    }
    assert(calls.size() == 5);
    for (const auto& call : calls) assert(call == "start");
  } else if (test == "start-failure") {
    WifiTransmitPower::setLimit(34);
    startResult = ESP_FAIL;
    assert(__wrap_esp_wifi_start() == ESP_FAIL);
    assert(calls == std::vector<std::string>{"start"});
  } else if (test == "read-failure" || test == "bad-driver-power") {
    WifiTransmitPower::setLimit(34);
    readResult = test == "read-failure" ? ESP_FAIL : ESP_OK;
    radioPower = 0;
    assert(__wrap_esp_wifi_start() != ESP_OK);
    assert(!running);
    assert((calls == std::vector<std::string>{"start", "get", "stop"}));
  } else if (test == "set-failure") {
    WifiTransmitPower::setLimit(34);
    setResult = ESP_FAIL;
    assert(__wrap_esp_wifi_start() == ESP_FAIL);
    assert(!running);
    assert((calls == std::vector<std::string>{"start", "get", "set", "stop"}));
  } else {
    assert(false && "Unknown test");
  }
}
