#include "ble/BleSyncManager.h"

#include <esp_crc.h>

#include "ble/ble_config.h"

#if CORE_DEBUG_LEVEL >= 3
#define BLE_LOG(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#define BLE_LOG_LN(s) Serial.println(s)
#else
#define BLE_LOG(...) ((void)0)
#define BLE_LOG_LN(s) ((void)0)
#endif

namespace {

using rsvpnano::ble::ACTIVE_CHAR_UUID;
using rsvpnano::ble::DEVICE_NAME;
using rsvpnano::ble::INFO_CHAR_UUID;
using rsvpnano::ble::LIBRARY_CHAR_UUID;
using rsvpnano::ble::POSITION_CHAR_UUID;
using rsvpnano::ble::PROTOCOL_VERSION;
using rsvpnano::ble::SERVICE_UUID;
using rsvpnano::ble::SETTINGS_CHAR_UUID;
using rsvpnano::ble::DELETE_CHAR_UUID;
using rsvpnano::ble::STORAGE_CHAR_UUID;
using rsvpnano::ble::TRANSFER_CHAR_UUID;

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 2);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Minimal JSON value extractors. Only used for short header frames and
// position writes, where the payload shape is fixed and tiny.
bool findJsonKey(const String &body, const char *key, int &colonIndex) {
  const String needle = String("\"") + key + "\"";
  const int idx = body.indexOf(needle);
  if (idx < 0) {
    return false;
  }
  colonIndex = body.indexOf(':', idx + needle.length());
  return colonIndex >= 0;
}

int skipJsonWs(const String &body, int idx) {
  while (idx < static_cast<int>(body.length()) &&
         isspace(static_cast<unsigned char>(body[idx]))) {
    ++idx;
  }
  return idx;
}

bool readJsonString(const String &body, const char *key, String &out) {
  int colon = -1;
  if (!findJsonKey(body, key, colon)) {
    return false;
  }
  int i = skipJsonWs(body, colon + 1);
  if (i >= static_cast<int>(body.length()) || body[i] != '"') {
    return false;
  }
  ++i;
  out = "";
  while (i < static_cast<int>(body.length()) && body[i] != '"') {
    if (body[i] == '\\' && i + 1 < static_cast<int>(body.length())) {
      const char nxt = body[i + 1];
      if (nxt == 'n') {
        out += '\n';
      } else if (nxt == 't') {
        out += '\t';
      } else if (nxt == 'r') {
        out += '\r';
      } else {
        out += nxt;
      }
      i += 2;
      continue;
    }
    out += body[i++];
  }
  return i < static_cast<int>(body.length());
}

bool readJsonUInt(const String &body, const char *key, uint32_t &out) {
  int colon = -1;
  if (!findJsonKey(body, key, colon)) {
    return false;
  }
  int i = skipJsonWs(body, colon + 1);
  if (i >= static_cast<int>(body.length()) ||
      !isdigit(static_cast<unsigned char>(body[i]))) {
    return false;
  }
  uint32_t v = 0;
  while (i < static_cast<int>(body.length()) &&
         isdigit(static_cast<unsigned char>(body[i]))) {
    v = v * 10 + static_cast<uint32_t>(body[i] - '0');
    ++i;
  }
  out = v;
  return true;
}

}  // namespace

// Forward-declared friends. Each callback owns a pointer back into the
// manager so its handlers can call private member functions.
class BleInfoCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleInfoCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    chr->setValue(mgr_->buildInfoJson().c_str());
  }

 private:
  BleSyncManager *mgr_;
};

class BleLibraryCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleLibraryCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  // Any write triggers a fetch; the byte content is ignored. Heavy work
  // happens on the loop task in startLibraryStream.
  void onWrite(NimBLECharacteristic *, NimBLEConnInfo &connInfo) override {
    // Handle store (relaxed) must happen-before the flag release-store so
    // the loop task reads a consistent (handle, flag=true) pair.
    mgr_->pendingLibraryConnHandle_.store(connInfo.getConnHandle(),
                                          std::memory_order_relaxed);
    mgr_->pendingLibraryFetch_.store(true, std::memory_order_release);
  }

 private:
  BleSyncManager *mgr_;
};

class BleActiveCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleActiveCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    const String hash = mgr_->dataStore_->activeBookHash();
    chr->setValue((String("{\"hash\":\"") + hash + "\"}").c_str());
  }
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    const String body = String(chr->getValue().c_str());
    String hash;
    if (!readJsonString(body, "hash", hash)) {
      return;
    }
    String error;
    mgr_->applyActiveHash(hash, error);
  }

 private:
  BleSyncManager *mgr_;
};

class BlePositionCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BlePositionCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    chr->setValue(mgr_->buildPositionJson().c_str());
  }
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    const String body = String(chr->getValue().c_str());
    String error;
    mgr_->applyPositionJson(body, error);
  }

 private:
  BleSyncManager *mgr_;
};

class BleDeleteCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleDeleteCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    const auto value = chr->getValue();
    mgr_->onDeleteWrite(reinterpret_cast<const uint8_t *>(value.data()), value.size());
  }

 private:
  BleSyncManager *mgr_;
};

class BleTransferCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleTransferCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    const auto value = chr->getValue();
    mgr_->onTransferWrite(reinterpret_cast<const uint8_t *>(value.data()), value.size());
  }

 private:
  BleSyncManager *mgr_;
};

class BleSettingsCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleSettingsCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    if (mgr_->dataStore_ == nullptr) {
      chr->setValue("{\"ok\":false,\"error\":\"data store not ready\"}");
      return;
    }
    chr->setValue(mgr_->dataStore_->settingsJson().c_str());
  }
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    if (mgr_->dataStore_ == nullptr) {
      chr->setValue("{\"ok\":false,\"error\":\"data store not ready\"}");
      return;
    }
    const String body = String(chr->getValue().c_str());
    String error;
    if (!mgr_->dataStore_->applySettingsJson(body, error)) {
      const String envelope = String("{\"ok\":false,\"error\":\"") + error + "\"}";
      chr->setValue(envelope.c_str());
      return;
    }
    chr->setValue(mgr_->dataStore_->settingsJson().c_str());
  }

 private:
  BleSyncManager *mgr_;
};

class BleStorageCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleStorageCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    chr->setValue(mgr_->buildStorageJson().c_str());
  }

 private:
  BleSyncManager *mgr_;
};

class BleServerCallbacks : public NimBLEServerCallbacks {
 public:
  explicit BleServerCallbacks(BleSyncManager *mgr) : mgr_(mgr) {}
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &, int reason) override {
    mgr_->resetUpload();
    // Skip re-advertise during teardown. Calling startAdvertising on an
    // in-flight deinit crashes NimBLE.
    if (mgr_->active_.load(std::memory_order_acquire)) {
      NimBLEDevice::startAdvertising();
    }
  }

 private:
  BleSyncManager *mgr_;
};

bool BleSyncManager::begin(BleDataStore &dataStore) {
  if (active_.load(std::memory_order_acquire)) {
    return true;
  }
  // Teardown still owns the NimBLE handles; caller can retry next tick.
  if (shutdownPhase_ != ShutdownPhase::Idle) {
    return false;
  }
  dataStore_ = &dataStore;

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517);

  server_ = NimBLEDevice::createServer();
  server_->setCallbacks(new BleServerCallbacks(this));

  NimBLEService *service = server_->createService(SERVICE_UUID);

  infoChar_ = service->createCharacteristic(INFO_CHAR_UUID, NIMBLE_PROPERTY::READ);
  infoChar_->setCallbacks(new BleInfoCallbacks(this));

  // Write + notify stream. Single READ truncated at MTU-3, so the client
  // writes a 1-byte trigger and consumes a tag-framed notify stream instead.
  libraryChar_ = service->createCharacteristic(
      LIBRARY_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  libraryChar_->setCallbacks(new BleLibraryCallbacks(this));

  activeChar_ = service->createCharacteristic(
      ACTIVE_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  activeChar_->setCallbacks(new BleActiveCallbacks(this));

  positionChar_ = service->createCharacteristic(
      POSITION_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  positionChar_->setCallbacks(new BlePositionCallbacks(this));

  transferChar_ = service->createCharacteristic(
      TRANSFER_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  transferChar_->setCallbacks(new BleTransferCallbacks(this));

  settingsChar_ = service->createCharacteristic(
      SETTINGS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  settingsChar_->setCallbacks(new BleSettingsCallbacks(this));

  storageChar_ = service->createCharacteristic(STORAGE_CHAR_UUID, NIMBLE_PROPERTY::READ);
  storageChar_->setCallbacks(new BleStorageCallbacks(this));

  deleteChar_ = service->createCharacteristic(DELETE_CHAR_UUID, NIMBLE_PROPERTY::WRITE);
  deleteChar_->setCallbacks(new BleDeleteCallbacks(this));

  service->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setName(DEVICE_NAME);
  adv->start();

  active_.store(true, std::memory_order_release);
  BLE_LOG("[ble] advertising as %s\n", DEVICE_NAME);
  return true;
}

void BleSyncManager::update() {
  // Drive the multi-tick shutdown state machine even when !active_. end()
  // flipped active_ false on entry to signal "don't re-advertise", but the
  // actual disconnect → drain → deinit work needs to proceed across ticks.
  if (shutdownPhase_ != ShutdownPhase::Idle) {
    tickShutdown();
    return;
  }
  if (!active_.load(std::memory_order_acquire)) {
    return;
  }

  // acquire pairs with release in onDeleteWrite: guarantees the hash field
  // is visible when the flag is observed true.
  if (pendingDelete_.load(std::memory_order_acquire)) {
    const String hash = pendingDeleteHash_;
    pendingDelete_.store(false, std::memory_order_relaxed);
    pendingDeleteHash_ = "";
    const bool ok = dataStore_->deleteBook(hash);
    BLE_LOG("[ble-delete] hash=%s ok=%d\n", hash.c_str(), ok);
  }

  if (pendingPosition_.load(std::memory_order_acquire)) {
    const String hash = pendingPositionHash_;
    const uint32_t wordIndex = pendingPositionWord_;
    pendingPosition_.store(false, std::memory_order_relaxed);
    pendingPositionHash_ = "";
    pendingPositionWord_ = 0;
    const bool ok = dataStore_->writePosition(hash, wordIndex);
    BLE_LOG("[ble-pos] write hash=%s word=%u ok=%d\n", hash.c_str(),
                  static_cast<unsigned>(wordIndex), ok);
    if (ok && positionListener_) {
      positionListener_(hash, wordIndex);
    }
  }

  if (pendingActive_.load(std::memory_order_acquire)) {
    const String hash = pendingActiveHash_;
    pendingActive_.store(false, std::memory_order_relaxed);
    pendingActiveHash_ = "";
    BLE_LOG("[ble-active] open hash=%s\n", hash.c_str());
    if (activeListener_) {
      activeListener_(hash);
    }
  }

  // Kick-off on trigger tick (HDR), then one notify per subsequent tick.
  // `else if` enforces a one-frame-per-tick cadence; back-to-back notifies
  // cause clients to drop the leading packet ("END before HDR" symptom).
  if (pendingLibraryFetch_.load(std::memory_order_acquire)) {
    pendingLibraryFetch_.store(false, std::memory_order_relaxed);
    const uint16_t handle =
        pendingLibraryConnHandle_.load(std::memory_order_relaxed);
    startLibraryStream(handle);
  } else if (libStream_.active) {
    advanceLibraryStream();
  }

  // Drain any SD work captured by the NimBLE write callback.
  if (upload_.pendingHeader) {
    upload_.pendingHeader = false;
    String error;
    if (!dataStore_->beginUpload(upload_.category, upload_.filename, error)) {
      BLE_LOG("[ble-xfer] beginUpload failed: %s\n", error.c_str());
      String msg = String("NACK:START:") + error;
      notifyTransfer(msg.c_str());
      resetUpload();
    } else {
      upload_.inProgress = true;
      BLE_LOG("[ble-xfer] begin %s (%s) size=%u\n", upload_.filename.c_str(),
                    upload_.category.c_str(), static_cast<unsigned>(upload_.bytesExpected));
      notifyTransfer("ACK:START");
    }
  }

  if (upload_.inProgress) {
    // Swap-and-drain + finish detection under a single critical-section
    // invariant: `finish` is set only when the swap returned an empty buffer
    // AT THE SAME mutex acquisition as the pendingFinish read. The callback
    // sets pendingBytes insert + bytesReceived + pendingFinish inside one
    // critical section, so if pendingFinish is true we either have the last
    // chunk in this swap (finish=false this iteration, true next) or the
    // last chunk was already drained previously (pendingBytes empty,
    // finish=true now). Without the empty-check the finishUpload below
    // could close the file while the tail chunk still sat in pendingBytes,
    // silently dropping the last bytes of every upload.
    bool finish = false;
    bool writeFailed = false;
    while (true) {
      std::vector<uint8_t> drain;
      portENTER_CRITICAL(&uploadMux_);
      drain.swap(upload_.pendingBytes);
      if (drain.empty() && upload_.pendingFinish) {
        upload_.pendingFinish = false;
        finish = true;
      }
      portEXIT_CRITICAL(&uploadMux_);
      if (drain.empty()) break;
      const uint32_t beforeWrite = millis();
      const bool ok = dataStore_->appendUpload(drain.data(), drain.size());
      const uint32_t writeMs = millis() - beforeWrite;
      if (writeMs > 20) {
        BLE_LOG("[ble-xfer] slow SD write %u ms\n", static_cast<unsigned>(writeMs));
      }
      if (!ok) {
        BLE_LOG("[ble-xfer] appendUpload failed at offset %u\n",
                      static_cast<unsigned>(upload_.bytesReceived));
        dataStore_->finishUpload(false);
        resetUpload();
        notifyTransfer("NACK:WRITE:io");
        writeFailed = true;
        break;
      }
    }
    if (writeFailed) return;
    if (finish) {
      const bool ok = dataStore_->finishUpload(true);
      BLE_LOG("[ble-xfer] finishUpload ok=%d\n", ok);
      resetUpload();
      notifyTransfer(ok ? "ACK:END" : "NACK:END:rename");
    }
  }

  const uint32_t now = millis();
  if (now - lastStatusLogMs_ >= 5000) {
    lastStatusLogMs_ = now;
    const size_t connected = server_ != nullptr ? server_->getConnectedCount() : 0;
    BLE_LOG("[ble] advertising=%s connected=%u\n",
                  NimBLEDevice::getAdvertising()->isAdvertising() ? "yes" : "no",
                  static_cast<unsigned>(connected));
  }
}

void BleSyncManager::end() {
  if (!active_.load(std::memory_order_acquire) &&
      shutdownPhase_ == ShutdownPhase::Idle) {
    return;
  }
  if (shutdownPhase_ != ShutdownPhase::Idle) {
    return;
  }

  // Flip active_ release-store FIRST so the disconnect callback skips its
  // re-advertise path while the deinit is in flight.
  active_.store(false, std::memory_order_release);

  resetUpload();
  pendingLibraryFetch_.store(false, std::memory_order_relaxed);
  pendingPosition_.store(false, std::memory_order_relaxed);
  pendingActive_.store(false, std::memory_order_relaxed);
  pendingDelete_.store(false, std::memory_order_relaxed);
  libStream_ = LibraryStream{};

  // Schedule across ticks: vTaskDelay here would freeze the reader and
  // deadlock if end() ran from a NimBLE/HTTP callback context.
  if (server_ != nullptr) {
    const auto peers = server_->getPeerDevices();
    for (const uint16_t handle : peers) {
      server_->disconnect(handle);
    }
  }
  NimBLEDevice::stopAdvertising();
  shutdownPhase_ = ShutdownPhase::DisconnectIssued;
  shutdownPhaseStartedMs_ = millis();
  BLE_LOG_LN("[ble] end() scheduled; draining...");
}

void BleSyncManager::tickShutdown() {
  const uint32_t now = millis();
  switch (shutdownPhase_) {
    case ShutdownPhase::Idle:
      return;
    case ShutdownPhase::DisconnectIssued:
      shutdownPhase_ = ShutdownPhase::AwaitingDrain;
      shutdownPhaseStartedMs_ = now;
      return;
    case ShutdownPhase::AwaitingDrain:
      // Guard band so in-flight notifies + disconnect events flush before
      // deinit. NimBLE deinit on a live connection crashes the chip.
      if (now - shutdownPhaseStartedMs_ < 200) {
        return;
      }
      shutdownPhase_ = ShutdownPhase::Deinit;
      return;
    case ShutdownPhase::Deinit:
      NimBLEDevice::deinit(true);
      server_ = nullptr;
      infoChar_ = nullptr;
      libraryChar_ = nullptr;
      activeChar_ = nullptr;
      positionChar_ = nullptr;
      transferChar_ = nullptr;
      settingsChar_ = nullptr;
      storageChar_ = nullptr;
      deleteChar_ = nullptr;
      shutdownPhase_ = ShutdownPhase::Idle;
      BLE_LOG_LN("[ble] end() complete");
      return;
  }
}

String BleSyncManager::buildInfoJson() {
  String body = "{\"deviceName\":\"";
  body += jsonEscape(DEVICE_NAME);
  body += "\",\"fwVersion\":\"0.0.0\",\"protoVersion\":";
  body += String(PROTOCOL_VERSION);
  body += "}";
  return body;
}

String BleSyncManager::buildLibraryJson() {
  String body = "[";
  if (dataStore_ != nullptr) {
    const auto books = dataStore_->listBooks();
    bool first = true;
    for (const auto &book : books) {
      if (!first) {
        body += ",";
      }
      first = false;
      body += "{\"hash\":\"" + book.hash + "\",";
      body += "\"title\":\"" + jsonEscape(book.title) + "\",";
      body += "\"author\":\"" + jsonEscape(book.author) + "\",";
      body += "\"words\":" + String(book.words) + ",";
      body += "\"progressWords\":" + String(book.progressWords) + ",";
      body += "\"category\":\"" + book.category + "\"}";
    }
  }
  body += "]";
  return body;
}

String BleSyncManager::buildStorageJson() {
  BleDataStore::StorageInfo info;
  if (dataStore_ != nullptr) {
    info = dataStore_->storage();
  }
  String body = "{\"freeBytes\":";
  body += String(static_cast<uint32_t>(info.freeBytes));
  body += ",\"totalBytes\":";
  body += String(static_cast<uint32_t>(info.totalBytes));
  body += ",\"bookCount\":";
  body += String(info.bookCount);
  body += "}";
  return body;
}

String BleSyncManager::buildPositionJson() {
  if (dataStore_ == nullptr) {
    return "{\"hash\":\"\",\"wordIndex\":0}";
  }
  const String hash = dataStore_->activeBookHash();
  uint32_t wordIndex = 0;
  uint32_t wordCount = 0;
  if (!hash.isEmpty()) {
    dataStore_->readPosition(hash, wordIndex, wordCount);
  }
  String body = "{\"hash\":\"" + hash + "\",\"wordIndex\":";
  body += String(wordIndex);
  body += "}";
  return body;
}

bool BleSyncManager::applyActiveHash(const String &hash, String &error) {
  if (dataStore_ == nullptr) {
    error = "Data store not ready";
    return false;
  }
  // Skip SD-touching validation: writing to NVS is fast, and resolvePathByHash
  // would call listBooks() which scans the SD card. After a heavy upload the SD
  // bus may still be flushing, blocking the NimBLE host task and timing out
  // the next BLE write. Caller is trusted to provide a valid hash; on next
  // reader-open the device will surface "book not found" if it isn't.
  dataStore_->setActiveBookHash(hash);
  // Defer to loop task; opening a book runs an SD index build that would
  // block the BLE host. Release-store publishes the hash to the reader.
  pendingActiveHash_ = hash;
  pendingActive_.store(true, std::memory_order_release);
  return true;
}

bool BleSyncManager::applyPositionJson(const String &body, String &error) {
  if (dataStore_ == nullptr) {
    error = "Data store not ready";
    return false;
  }
  String hash;
  uint32_t wordIndex = 0;
  if (!readJsonString(body, "hash", hash) || hash.isEmpty()) {
    error = "Missing hash";
    return false;
  }
  if (!readJsonUInt(body, "wordIndex", wordIndex)) {
    error = "Missing wordIndex";
    return false;
  }
  // Capture; loop task does the NVS write + listener via update() so the
  // BLE host task isn't stalled by reader-seek work.
  const bool coalesced = pendingPosition_.load(std::memory_order_relaxed);
  pendingPositionHash_ = hash;
  pendingPositionWord_ = wordIndex;
  pendingPosition_.store(true, std::memory_order_release);
  BLE_LOG("[ble-pos] queued hash=%s word=%u coalesced=%d\n", hash.c_str(),
                static_cast<unsigned>(wordIndex), coalesced);
  return true;
}

void BleSyncManager::onTransferWrite(const uint8_t *bytes, size_t len) {
  if (dataStore_ == nullptr || bytes == nullptr || len == 0) {
    return;
  }

  if (!upload_.inProgress && !upload_.pendingHeader) {
    String body;
    body.reserve(len);
    for (size_t i = 0; i < len; ++i) {
      body += static_cast<char>(bytes[i]);
    }
    String filename;
    String category;
    uint32_t sizeBytes = 0;
    if (!readJsonString(body, "filename", filename) ||
        !readJsonString(body, "category", category) ||
        !readJsonUInt(body, "sizeBytes", sizeBytes)) {
      notifyTransfer("NACK:START:bad_header");
      return;
    }
    upload_.filename = filename;
    upload_.category = category;
    upload_.bytesExpected = sizeBytes;
    upload_.bytesReceived = 0;
    upload_.pendingHeader = true;
    return;
  }

  if (upload_.inProgress) {
    portENTER_CRITICAL(&uploadMux_);
    upload_.pendingBytes.insert(upload_.pendingBytes.end(), bytes, bytes + len);
    upload_.bytesReceived += len;
    if (upload_.bytesExpected > 0 && upload_.bytesReceived >= upload_.bytesExpected) {
      upload_.pendingFinish = true;
    }
    portEXIT_CRITICAL(&uploadMux_);
  }
}

void BleSyncManager::onDeleteWrite(const uint8_t *bytes, size_t len) {
  if (bytes == nullptr || len == 0) {
    return;
  }
  String body;
  body.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    body += static_cast<char>(bytes[i]);
  }
  String hash;
  if (!readJsonString(body, "hash", hash) || hash.isEmpty()) {
    BLE_LOG("[ble-delete] bad payload: %s\n", body.c_str());
    return;
  }
  pendingDeleteHash_ = hash;
  pendingDelete_.store(true, std::memory_order_release);
}

void BleSyncManager::resetUpload() {
  if (upload_.inProgress) {
    dataStore_->finishUpload(false);
  }
  upload_ = UploadState{};
}

void BleSyncManager::notifyTransfer(const char *msg) {
  if (transferChar_ == nullptr) {
    return;
  }
  BLE_LOG("[ble-xfer] notify-> %s\n", msg);
  transferChar_->setValue(reinterpret_cast<const uint8_t *>(msg), strlen(msg));
  BLE_LOG_LN("[ble-xfer] setValue done");
  transferChar_->notify();
  BLE_LOG_LN("[ble-xfer] notify done");
}

void BleSyncManager::notifyPosition(const String &hash, uint32_t wordIndex) {
  if (!active_.load(std::memory_order_acquire) || positionChar_ == nullptr) {
    return;
  }
  // Rate-limit to ~5 Hz. Reader saves can run faster at high WPM; library +
  // transfer notifies share the same NimBLE mbuf pool so we shouldn't flood.
  const uint32_t now = millis();
  if (lastPositionNotifyMs_ != 0 && now - lastPositionNotifyMs_ < 200) {
    return;
  }
  String body = "{\"hash\":\"";
  body += hash;
  body += "\",\"wordIndex\":";
  body += String(wordIndex);
  body += "}";
  positionChar_->setValue(reinterpret_cast<const uint8_t *>(body.c_str()),
                          body.length());
  if (!positionChar_->notify()) {
    // No subscriber or mbuf exhausted; harmless. App falls back to the
    // connect-time read on next session.
    return;
  }
  lastPositionNotifyMs_ = now;
}

void BleSyncManager::startLibraryStream(uint16_t connHandle) {
  if (libraryChar_ == nullptr || server_ == nullptr) {
    return;
  }
  if (libStream_.active) {
    // Drop overlapping trigger; in-flight stream still carries the same
    // data, next refresh picks up any changes.
    BLE_LOG_LN("[ble-lib] trigger while busy; ignored");
    return;
  }

  libStream_.payload = buildLibraryJson();
  libStream_.len = static_cast<uint32_t>(libStream_.payload.length());

  // NimBLE notify payload max = peerMTU - 3 (ATT opcode + handle). DATA frame
  // adds 3 bytes (tag + u16 seq). Cap at 240 for clients that negotiate a
  // smaller MTU.
  uint16_t peerMtu = server_->getPeerMTU(connHandle);
  if (peerMtu < 23) {
    peerMtu = 23;
  }
  uint32_t chunkSize = static_cast<uint32_t>(peerMtu) - 6;
  if (chunkSize > 240) {
    chunkSize = 240;
  }
  if (chunkSize < 16) {
    chunkSize = 16;
  }
  libStream_.chunkSize = chunkSize;
  libStream_.totalChunks =
      libStream_.len == 0 ? 0 : (libStream_.len + chunkSize - 1) / chunkSize;

  if (libStream_.totalChunks > 0xFFFF) {
    // ERR frame. Per-call buffer sized to fit any future reason without
    // touching the stack frame size constant.
    const char reason[] = "payload too large";
    constexpr size_t kReasonLen = sizeof(reason) - 1;
    uint8_t err[1 + kReasonLen];
    err[0] = 0x7F;
    memcpy(err + 1, reason, kReasonLen);
    libraryChar_->setValue(err, sizeof(err));
    libraryChar_->notify();
    BLE_LOG("[ble-lib] ERR: %s (payload=%u)\n", reason,
                  static_cast<unsigned>(libStream_.len));
    libStream_.payload = "";
    return;
  }

  libStream_.crc = esp_crc32_le(
      0, reinterpret_cast<const uint8_t *>(libStream_.payload.c_str()),
      libStream_.len);

  BLE_LOG(
      "[ble-lib] stream start payload=%u chunkSize=%u totalChunks=%u mtu=%u crc=%08x\n",
      static_cast<unsigned>(libStream_.len),
      static_cast<unsigned>(libStream_.chunkSize),
      static_cast<unsigned>(libStream_.totalChunks),
      static_cast<unsigned>(peerMtu), static_cast<unsigned>(libStream_.crc));

  // HDR frame: [0x01][totalChunks:u16 BE][totalBytes:u32 BE]
  uint8_t hdr[1 + 2 + 4];
  hdr[0] = 0x01;
  hdr[1] = static_cast<uint8_t>((libStream_.totalChunks >> 8) & 0xFF);
  hdr[2] = static_cast<uint8_t>(libStream_.totalChunks & 0xFF);
  hdr[3] = static_cast<uint8_t>((libStream_.len >> 24) & 0xFF);
  hdr[4] = static_cast<uint8_t>((libStream_.len >> 16) & 0xFF);
  hdr[5] = static_cast<uint8_t>((libStream_.len >> 8) & 0xFF);
  hdr[6] = static_cast<uint8_t>(libStream_.len & 0xFF);
  libraryChar_->setValue(hdr, sizeof(hdr));
  libraryChar_->notify();

  libStream_.nextSeq = 0;
  // First DATA waits the inter-emit gap so the client has time to process
  // subscribe + HDR before the next packet.
  libStream_.lastEmitMs = millis();
  libStream_.active = true;
}

void BleSyncManager::advanceLibraryStream() {
  if (!libStream_.active || libraryChar_ == nullptr) {
    return;
  }

  // Rate-limit: Android BLE stacks silently drop DATA notifies arriving
  // faster than the client can drain. 30 ms gap (~33 pkt/s) is safe under
  // the platform's notification throughput ceiling.
  const uint32_t now = millis();
  if (libStream_.lastEmitMs != 0 && now - libStream_.lastEmitMs < 30) {
    return;
  }

  if (libStream_.nextSeq < libStream_.totalChunks) {
    const uint32_t offset = libStream_.nextSeq * libStream_.chunkSize;
    const uint32_t remaining = libStream_.len - offset;
    const uint32_t take =
        remaining < libStream_.chunkSize ? remaining : libStream_.chunkSize;
    uint8_t frame[1 + 2 + 240];
    frame[0] = 0x02;
    frame[1] = static_cast<uint8_t>((libStream_.nextSeq >> 8) & 0xFF);
    frame[2] = static_cast<uint8_t>(libStream_.nextSeq & 0xFF);
    memcpy(frame + 3,
           reinterpret_cast<const uint8_t *>(libStream_.payload.c_str()) + offset,
           take);
    libraryChar_->setValue(frame, 3 + take);
    // notify() returns false on no subscriber or mbuf exhaustion. On
    // failure, leave seq alone and retry next tick.
    const bool ok = libraryChar_->notify();
    if (!ok) {
      BLE_LOG("[ble-lib] notify failed seq=%u, retry next tick\n",
                    static_cast<unsigned>(libStream_.nextSeq));
      return;
    }
    libStream_.lastEmitMs = now;
    libStream_.nextSeq++;
    return;
  }

  // END frame. Empty-library case (totalChunks==0) needs the inter-emit
  // gap too; the top-of-function rate-limit already enforces it.
  uint8_t end[1 + 4];
  end[0] = 0x03;
  end[1] = static_cast<uint8_t>((libStream_.crc >> 24) & 0xFF);
  end[2] = static_cast<uint8_t>((libStream_.crc >> 16) & 0xFF);
  end[3] = static_cast<uint8_t>((libStream_.crc >> 8) & 0xFF);
  end[4] = static_cast<uint8_t>(libStream_.crc & 0xFF);
  libraryChar_->setValue(end, sizeof(end));
  const bool endOk = libraryChar_->notify();
  if (!endOk) {
    BLE_LOG_LN("[ble-lib] END notify failed, retry next tick");
    return;
  }

  BLE_LOG("[ble-lib] stream done chunks=%u bytes=%u\n",
                static_cast<unsigned>(libStream_.totalChunks),
                static_cast<unsigned>(libStream_.len));

  libStream_.active = false;
  libStream_.payload = "";  // free String backing storage
  libStream_.lastEmitMs = 0;
}
