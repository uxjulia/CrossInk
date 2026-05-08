#pragma once

#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEServer.h>
#include <Logging.h>

class BluetoothManager {
 private:
  NimBLEServer* pServer = nullptr;
  NimBLECharacteristic* pCharacteristic = nullptr;
  bool isInitialized = false;
  bool isEnabled = false;

 public:
  BluetoothManager() = default;
  ~BluetoothManager() = default;

  // Initialize Bluetooth system (call once at startup)
  bool init() {
    if (isInitialized) {
      return true;
    }

    LOG_INF("BLE", "Initializing Bluetooth LE");
    
    try {
      NimBLEDevice::init("CrossInk");
      isInitialized = true;
      LOG_INF("BLE", "Bluetooth LE initialized");
      return true;
    } catch (const std::exception& e) {
      LOG_ERR("BLE", "Failed to initialize: %s", e.what());
      return false;
    }
  }

  // Enable/start Bluetooth advertising
  bool enable() {
    if (!isInitialized) {
      LOG_ERR("BLE", "Bluetooth not initialized");
      return false;
    }

    if (isEnabled) {
      LOG_DBG("BLE", "Bluetooth already enabled");
      return true;
    }

    try {
      // Create server if not exists
      if (!pServer) {
        pServer = NimBLEDevice::createServer();
        if (!pServer) {
          LOG_ERR("BLE", "Failed to create BLE server");
          return false;
        }
      }

      // Create service (Device Information Service)
      NimBLEService* pService = pServer->createService("180A");
      if (!pService) {
        LOG_ERR("BLE", "Failed to create BLE service");
        return false;
      }

      // Create characteristic (Manufacturer Name String)
      pCharacteristic = pService->createCharacteristic(
        "2A29",
        NIMBLE_PROPERTY::READ
      );
      if (!pCharacteristic) {
        LOG_ERR("BLE", "Failed to create BLE characteristic");
        return false;
      }

      pCharacteristic->setValue("CrossPoint Reader");
      pService->start();

      // Start advertising
      NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
      pAdvertising->addServiceUUID(pService->getUUID());
      pAdvertising->setScanResponse(true);
      pAdvertising->setMinPreferred(0x06);
      pAdvertising->setMaxPreferred(0x12);
      NimBLEDevice::startAdvertising();

      isEnabled = true;
      LOG_INF("BLE", "Bluetooth LE enabled and advertising");
      return true;
    } catch (const std::exception& e) {
      LOG_ERR("BLE", "Failed to enable: %s", e.what());
      return false;
    }
  }

  // Disable/stop Bluetooth advertising
  bool disable() {
    if (!isEnabled) {
      LOG_DBG("BLE", "Bluetooth already disabled");
      return true;
    }

    try {
      NimBLEDevice::stopAdvertising();
      isEnabled = false;
      LOG_INF("BLE", "Bluetooth LE disabled");
      return true;
    } catch (const std::exception& e) {
      LOG_ERR("BLE", "Failed to disable: %s", e.what());
      return false;
    }
  }

  // Check if Bluetooth is enabled
  bool isEnabledAndAdvertising() const {
    return isEnabled;
  }

  // Check if device is connected
  bool isConnected() const {
    if (!pServer || !isEnabled) return false;
    return pServer->getConnectedCount() > 0;
  }

  // Shutdown Bluetooth
  void shutdown() {
    if (isEnabled) {
      disable();
    }
    if (isInitialized) {
      try {
        NimBLEDevice::deinit();
        isInitialized = false;
        LOG_INF("BLE", "Bluetooth LE shutdown");
      } catch (const std::exception& e) {
        LOG_ERR("BLE", "Error during shutdown: %s", e.what());
      }
    }
  }
};
