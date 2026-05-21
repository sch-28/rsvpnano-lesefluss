#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <functional>
#include <vector>

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
  // Fired on the Arduino loop task (not the NimBLE host task) whenever the
  // app pushes a position write that names the device's currently-active
  // book hash. Listener should seek the live reader to wordIndex.
  using PositionListener = std::function<void(const String &hash, uint32_t wordIndex)>;

  bool begin(RsvpDataStore &dataStore);
  void update();
  void end();
  bool active() const { return active_; }

  void setPositionListener(PositionListener listener) { positionListener_ = std::move(listener); }

 private:
  // SD-touching steps run on the Arduino loop task, not the NimBLE host task.
  // Callbacks only push data into these fields; update() drains them.
  struct UploadState {
    bool inProgress = false;       // beginUpload succeeded, file open on SD
    bool pendingHeader = false;    // header arrived, beginUpload not yet called
    bool pendingFinish = false;    // total reached, finishUpload not yet called
    uint32_t bytesReceived = 0;
    uint32_t bytesExpected = 0;
    String filename;
    String category;
    std::vector<uint8_t> pendingBytes;  // body chunks awaiting SD write
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
  friend class BleDeleteCallbacks;
  friend class BleServerCallbacks;

  String buildInfoJson();
  String buildLibraryJson();
  String buildStorageJson();
  String buildPositionJson();
  bool applyActiveHash(const String &hash, String &error);
  bool applyPositionJson(const String &body, String &error);

  void onTransferWrite(const uint8_t *bytes, size_t len);
  void onDeleteWrite(const uint8_t *bytes, size_t len);
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
  NimBLECharacteristic *deleteChar_ = nullptr;

  UploadState upload_;
  // Pending delete request captured by the NimBLE write callback; drained on
  // the Arduino loop task by update() so SD remove + NVS writes don't run on
  // the BLE host task.
  bool pendingDelete_ = false;
  String pendingDeleteHash_;
  // Pending position write captured by the NimBLE host task. Drained on the
  // Arduino loop task so the NVS write + reader-seek listener fire away from
  // BLE callback context.
  bool pendingPosition_ = false;
  String pendingPositionHash_;
  uint32_t pendingPositionWord_ = 0;
  PositionListener positionListener_;
  bool active_ = false;
  uint32_t lastStatusLogMs_ = 0;
};
