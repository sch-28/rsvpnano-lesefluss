#include "sync/BleSyncManager.h"

#include "ble/ble_config.h"

namespace {

using lesefluss::ble::ACTIVE_CHAR_UUID;
using lesefluss::ble::DEVICE_NAME;
using lesefluss::ble::INFO_CHAR_UUID;
using lesefluss::ble::LIBRARY_CHAR_UUID;
using lesefluss::ble::POSITION_CHAR_UUID;
using lesefluss::ble::PROTOCOL_VERSION;
using lesefluss::ble::SERVICE_UUID;
using lesefluss::ble::SETTINGS_CHAR_UUID;
using lesefluss::ble::STORAGE_CHAR_UUID;
using lesefluss::ble::TRANSFER_CHAR_UUID;

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
  void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    chr->setValue(mgr_->buildLibraryJson().c_str());
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
    // Settings JSON port is deferred. Returns a minimal envelope so the
    // app can detect the characteristic exists without crashing on parse.
    chr->setValue("{\"ok\":false,\"error\":\"settings not yet wired\"}");
  }
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    chr->setValue("{\"ok\":false,\"error\":\"settings not yet wired\"}");
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
    NimBLEDevice::startAdvertising();
  }

 private:
  BleSyncManager *mgr_;
};

bool BleSyncManager::begin(RsvpDataStore &dataStore) {
  if (active_) {
    return true;
  }
  dataStore_ = &dataStore;

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517);

  server_ = NimBLEDevice::createServer();
  server_->setCallbacks(new BleServerCallbacks(this));

  NimBLEService *service = server_->createService(SERVICE_UUID);

  infoChar_ = service->createCharacteristic(INFO_CHAR_UUID, NIMBLE_PROPERTY::READ);
  infoChar_->setCallbacks(new BleInfoCallbacks(this));

  libraryChar_ = service->createCharacteristic(LIBRARY_CHAR_UUID, NIMBLE_PROPERTY::READ);
  libraryChar_->setCallbacks(new BleLibraryCallbacks(this));

  activeChar_ = service->createCharacteristic(
      ACTIVE_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  activeChar_->setCallbacks(new BleActiveCallbacks(this));

  positionChar_ = service->createCharacteristic(
      POSITION_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  positionChar_->setCallbacks(new BlePositionCallbacks(this));

  transferChar_ = service->createCharacteristic(
      TRANSFER_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  transferChar_->setCallbacks(new BleTransferCallbacks(this));

  settingsChar_ = service->createCharacteristic(
      SETTINGS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  settingsChar_->setCallbacks(new BleSettingsCallbacks(this));

  storageChar_ = service->createCharacteristic(STORAGE_CHAR_UUID, NIMBLE_PROPERTY::READ);
  storageChar_->setCallbacks(new BleStorageCallbacks(this));

  service->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setName(DEVICE_NAME);
  adv->start();

  active_ = true;
  Serial.printf("[ble] advertising as %s\n", DEVICE_NAME);
  return true;
}

void BleSyncManager::update() {
  if (!active_) {
    return;
  }

  // Drain any SD work captured by the NimBLE write callback.
  if (upload_.pendingHeader) {
    upload_.pendingHeader = false;
    String error;
    if (!dataStore_->beginUpload(upload_.category, upload_.filename, error)) {
      Serial.printf("[ble-xfer] beginUpload failed: %s\n", error.c_str());
      String msg = String("NACK:START:") + error;
      notifyTransfer(msg.c_str());
      resetUpload();
    } else {
      upload_.inProgress = true;
      Serial.printf("[ble-xfer] begin %s (%s) size=%u\n", upload_.filename.c_str(),
                    upload_.category.c_str(), static_cast<unsigned>(upload_.bytesExpected));
      notifyTransfer("ACK:START");
    }
  }

  if (upload_.inProgress && !upload_.pendingBytes.empty()) {
    const uint32_t beforeWrite = millis();
    const bool ok = dataStore_->appendUpload(upload_.pendingBytes.data(),
                                              upload_.pendingBytes.size());
    const uint32_t writeMs = millis() - beforeWrite;
    if (writeMs > 20) {
      Serial.printf("[ble-xfer] slow SD write %u ms\n", static_cast<unsigned>(writeMs));
    }
    if (!ok) {
      Serial.printf("[ble-xfer] appendUpload failed at offset %u\n",
                    static_cast<unsigned>(upload_.bytesReceived));
      dataStore_->finishUpload(false);
      resetUpload();
      notifyTransfer("NACK:WRITE:io");
      return;
    }
    upload_.pendingBytes.clear();
  }

  if (upload_.pendingFinish) {
    upload_.pendingFinish = false;
    const bool ok = dataStore_->finishUpload(true);
    Serial.printf("[ble-xfer] finishUpload ok=%d\n", ok);
    resetUpload();
    notifyTransfer(ok ? "ACK:END" : "NACK:END:rename");
  }

  const uint32_t now = millis();
  if (now - lastStatusLogMs_ >= 5000) {
    lastStatusLogMs_ = now;
    const size_t connected = server_ != nullptr ? server_->getConnectedCount() : 0;
    Serial.printf("[ble] advertising=%s connected=%u\n",
                  NimBLEDevice::getAdvertising()->isAdvertising() ? "yes" : "no",
                  static_cast<unsigned>(connected));
  }
}

void BleSyncManager::end() {
  if (!active_) {
    return;
  }
  resetUpload();
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);
  active_ = false;
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
  RsvpDataStore::StorageInfo info;
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
  if (dataStore_->resolvePathByHash(hash).isEmpty()) {
    error = "Unknown book hash";
    return false;
  }
  dataStore_->setActiveBookHash(hash);
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
  return dataStore_->writePosition(hash, wordIndex);
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
    upload_.pendingBytes.insert(upload_.pendingBytes.end(), bytes, bytes + len);
    upload_.bytesReceived += len;
    if (upload_.bytesExpected > 0 && upload_.bytesReceived >= upload_.bytesExpected) {
      upload_.pendingFinish = true;
    }
  }
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
  Serial.printf("[ble-xfer] notify-> %s\n", msg);
  transferChar_->setValue(reinterpret_cast<const uint8_t *>(msg), strlen(msg));
  Serial.println("[ble-xfer] setValue done");
  transferChar_->notify();
  Serial.println("[ble-xfer] notify done");
}
