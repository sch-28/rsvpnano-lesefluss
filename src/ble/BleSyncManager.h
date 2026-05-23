#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <functional>
#include <vector>

#include "storage/BleDataStore.h"

// NimBLE GATT server exposing the multibook BLE schema. Single-connection
// by design; chunked transfer state lives on the manager.
class BleSyncManager {
 public:
  // Fired on the Arduino loop task when a peer writes a position update for
  // the device's currently-active book. Listener should seek the live reader.
  using PositionListener = std::function<void(const String &hash, uint32_t wordIndex)>;
  // Fired on the Arduino loop task when a peer writes a new active hash.
  // Listener should open the book on the device's reader.
  using ActiveListener = std::function<void(const String &hash)>;

  bool begin(BleDataStore &dataStore);
  void update();
  // Returns immediately; disconnect + NimBLE deinit run across multiple
  // update() ticks. Safe to call from any context. Poll active() to know
  // when teardown completes.
  void end();
  bool active() const { return active_.load(std::memory_order_acquire); }

  void setPositionListener(PositionListener listener) { positionListener_ = std::move(listener); }
  void setActiveListener(ActiveListener listener) { activeListener_ = std::move(listener); }

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

 public:
  // Emit a position update over the position characteristic so subscribed
  // peers see device-side reader advances in real time. No-op when inactive.
  void notifyPosition(const String &hash, uint32_t wordIndex);

 private:
  // Library notify stream is split across multiple update() ticks to avoid
  // blocking the reader loop. start: build payload + CRC, send HDR. advance:
  // one DATA frame per tick, then END.
  void startLibraryStream(uint16_t connHandle);
  void advanceLibraryStream();
  // Multi-tick teardown driver. No vTaskDelay; uses millis() so the loop
  // task never blocks.
  void tickShutdown();

  BleDataStore *dataStore_ = nullptr;
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
  // Pending writes captured by NimBLE host callbacks; drained on the Arduino
  // loop task in update() so SD/NVS/listener work runs off the BLE host task.
  // Release/acquire pairs publish the associated data fields safely.
  std::atomic<bool> pendingDelete_{false};
  String pendingDeleteHash_;
  std::atomic<bool> pendingPosition_{false};
  String pendingPositionHash_;
  uint32_t pendingPositionWord_ = 0;
  PositionListener positionListener_;
  std::atomic<bool> pendingActive_{false};
  String pendingActiveHash_;
  ActiveListener activeListener_;
  std::atomic<bool> pendingLibraryFetch_{false};
  std::atomic<uint16_t> pendingLibraryConnHandle_{0xFFFF};
  // Rate-limit position notifies; reader saves can fire at WPM rate, but the
  // peer doesn't need every advance. Caps wire traffic + reduces contention
  // with library/transfer notifies on the NimBLE mbuf pool.
  uint32_t lastPositionNotifyMs_ = 0;
  struct LibraryStream {
    bool active = false;
    String payload;
    uint32_t len = 0;
    uint32_t crc = 0;
    uint32_t totalChunks = 0;
    uint32_t nextSeq = 0;
    uint32_t chunkSize = 0;
    uint32_t lastEmitMs = 0;
    bool endPending = false;
  };
  LibraryStream libStream_;
  enum class ShutdownPhase : uint8_t { Idle, DisconnectIssued, AwaitingDrain, Deinit };
  ShutdownPhase shutdownPhase_ = ShutdownPhase::Idle;
  uint32_t shutdownPhaseStartedMs_ = 0;
  std::atomic<bool> active_{false};
  uint32_t lastStatusLogMs_ = 0;
  // Without this critical section the drain's read-then-clear races with the
  // NimBLE host task's mid-insert, silently dropping body bytes: chunks get
  // counted in bytesReceived (ACK:END fires) but never land on SD.
  portMUX_TYPE uploadMux_ = portMUX_INITIALIZER_UNLOCKED;
};
