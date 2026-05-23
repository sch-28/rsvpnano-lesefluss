#pragma once

#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>

#include <atomic>
#include <vector>

// Shared data layer for HTTP companion sync and BLE GATT sync.
//
// Both transports must agree on: NVS namespace, key formats, SD book paths,
// and book-hash derivation. This module owns those invariants. The HTTP path
// currently keeps its own copies of the same logic; both implementations
// read and write the same on-disk and NVS state, so they stay consistent
// by sharing constants rather than by sharing code.
class BleDataStore {
 public:
  static constexpr const char *kBooksPath = "/books";
  static constexpr const char *kBookFilesPath = "/books/books";
  static constexpr const char *kArticleFilesPath = "/books/articles";

  struct BookEntry {
    String path;
    String filename;
    String category;
    String title;
    String author;
    uint32_t bytes = 0;
    uint32_t words = 0;
    uint32_t progressWords = 0;
    String hash;
  };

  struct StorageInfo {
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    uint32_t bookCount = 0;
  };

  bool begin();
  void end();

  std::vector<BookEntry> listBooks();

  // 8-char lowercase hex FNV-1a of the absolute SD path. Stable across reboots.
  static String hashBookPath(const String &path);

  // Resolve a hash back to an SD path by enumerating the library. Returns
  // empty string when not found. O(N) over the on-disk book set.
  String resolvePathByHash(const String &hash);

  bool readPosition(const String &hash, uint32_t &wordIndex, uint32_t &wordCount);
  bool writePosition(const String &hash, uint32_t wordIndex);

  String activeBookHash();
  bool setActiveBookHash(const String &hash);

  StorageInfo storage();

  // Atomic upload commit. Caller invokes beginUpload() once with target
  // metadata, streams arbitrary-length byte runs via appendUpload(), then
  // finishUpload(true) on success or finishUpload(false) to abort.
  // The temp file is removed on abort; on success it is renamed atomically
  // over any prior file at the same path. Filename is sanitized in-place.
  bool beginUpload(const String &category, const String &filename, String &error);
  bool appendUpload(const uint8_t *bytes, size_t len);
  bool finishUpload(bool success);
  String lastUploadedPath() const { return lastUploadedPath_; }

  // Remove the book identified by `hash` from SD and clear its NVS
  // position/word-count keys. Idempotent: returns false when the hash does
  // not resolve to a known book, true when the file was successfully removed.
  // If the deleted book was the device's active book, clears the active key.
  bool deleteBook(const String &hash);

  // Settings JSON wire format matches the HTTP /api/settings endpoint, so
  // either transport can read and patch the same NVS-backed store.
  // `settingsJson()` always returns a fully-populated envelope on success.
  // `applySettingsJson()` accepts a partial patch; missing fields are left
  // untouched. Validation errors set `error` and return false.
  String settingsJson();
  bool applySettingsJson(const String &body, String &error);

  bool bleEnabled();
  void setBleEnabled(bool enabled);

 private:
  static String bookPositionKey(const String &hash);
  static String bookWordCountKey(const String &hash);
  static String sanitizeFilename(const String &name);

  struct RsvpMetadata {
    String title;
    String author;
  };

  RsvpMetadata readRsvpMetadata(const String &path);

  Preferences preferences_;
  bool began_ = false;
  // Atomic mirror of kPrefBleEnabled so callers can poll the flag without
  // touching NVS. Concurrent NVS reads from the loop task while the NimBLE
  // host task writes other keys (e.g. setActiveBookHash) hangs the loop.
  std::atomic<bool> bleEnabledCache_{false};

  File uploadFile_;
  String uploadFinalPath_;
  String uploadTmpPath_;
  String lastUploadedPath_;
};
