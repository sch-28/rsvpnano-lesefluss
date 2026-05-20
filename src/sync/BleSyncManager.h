#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "storage/RsvpDataStore.h"

// NimBLE GATT server exposing the multibook BLE schema defined in
// packages/ble-config/config-multibook.json. Acts as a second front-end
// onto RsvpDataStore, alongside the existing WiFi-AP HTTP CompanionSync.
//
// Single-connection by design for v1. Chunked book transfer state lives
// on this manager (not per-connection) since only one peer transfers at
// a time.
class BleSyncManager {
 public:
  bool begin(RsvpDataStore &dataStore);
  void update();
  void end();
  bool active() const { return active_; }

 private:
  struct UploadState {
    bool inProgress = false;
    uint32_t bytesReceived = 0;
    uint32_t bytesExpected = 0;
    String filename;
    String category;
  };

  // Char callback fan-out. NimBLECharacteristicCallbacks subclasses live in
  // the cpp file and forward into these member functions.
  friend class BleInfoCallbacks;
  friend class BleLibraryCallbacks;
  friend class BleActiveCallbacks;
  friend class BlePositionCallbacks;
  friend class BleTransferCallbacks;
  friend class BleSettingsCallbacks;
  friend class BleStorageCallbacks;
  friend class BleServerCallbacks;

  String buildInfoJson();
  String buildLibraryJson();
  String buildStorageJson();
  String buildPositionJson();
  bool applyActiveHash(const String &hash, String &error);
  bool applyPositionJson(const String &body, String &error);

  void onTransferWrite(const uint8_t *bytes, size_t len);
  void resetUpload();
  void notifyTransfer(const char *msg);

  RsvpDataStore *dataStore_ = nullptr;
  NimBLEServer *server_ = nullptr;
  NimBLECharacteristic *infoChar_ = nullptr;
  NimBLECharacteristic *libraryChar_ = nullptr;
  NimBLECharacteristic *activeChar_ = nullptr;
  NimBLECharacteristic *positionChar_ = nullptr;
  NimBLECharacteristic *transferChar_ = nullptr;
  NimBLECharacteristic *settingsChar_ = nullptr;
  NimBLECharacteristic *storageChar_ = nullptr;

  UploadState upload_;
  bool active_ = false;
  uint32_t lastStatusLogMs_ = 0;
};
