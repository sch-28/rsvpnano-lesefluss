#include "storage/RsvpDataStore.h"

#include <SD_MMC.h>
#include <cstdio>

namespace {

constexpr const char *kPrefsNamespace = "rsvp";
constexpr const char *kPrefActiveBook = "active";
constexpr size_t kMaxMetadataLineChars = 160;

// Settings NVS keys + bounds. Mirror of the constants in
// CompanionSyncManager.cpp; both modules must agree on these strings since
// they read and write the same NVS namespace. TASK-140 will consolidate.
constexpr const char *kPrefWpm = "wpm";
constexpr const char *kPrefBrightness = "bright";
constexpr const char *kPrefDarkMode = "dark";
constexpr const char *kPrefNightMode = "night";
constexpr const char *kPrefUiLanguage = "ui_lang";
constexpr const char *kPrefReaderMode = "read_mode";
constexpr const char *kPrefHandedness = "handed";
constexpr const char *kPrefPhantomWords = "phantom_on";
constexpr const char *kPrefFooterMetricMode = "prog_md";
constexpr const char *kPrefBatteryLabelMode = "bat_md";
constexpr const char *kPrefReaderBatteryVisible = "read_bat";
constexpr const char *kPrefReaderChapterVisible = "read_ch";
constexpr const char *kPrefReaderProgressVisible = "read_pct";
constexpr const char *kPrefReaderFontSize = "font_size";
constexpr const char *kPrefReaderTypeface = "typeface";
constexpr const char *kPrefTypographyFocusHighlight = "type_hlt";
constexpr const char *kPrefPacingLongMs = "pace_lms";
constexpr const char *kPrefPacingComplexMs = "pace_cms";
constexpr const char *kPrefPacingPunctuationMs = "pace_pms";
constexpr const char *kPrefPauseMode = "pause_md";
constexpr const char *kPrefAccurateTime = "time_est_a";
constexpr const char *kPrefTypographyTracking = "type_trk";
constexpr const char *kPrefTypographyAnchor = "type_anc";
constexpr const char *kPrefTypographyGuideWidth = "type_wid";
constexpr const char *kPrefTypographyGuideGap = "type_gap";

constexpr uint16_t kDefaultWpm = 300;
constexpr uint16_t kMinWpm = 10;
constexpr uint16_t kMaxWpm = 1000;
constexpr uint8_t kDefaultBrightness = 3;
constexpr uint8_t kMaxBrightness = 4;
constexpr uint8_t kMaxUiLanguage = 1;
constexpr uint8_t kMaxReaderMode = 1;
constexpr uint8_t kMaxHandedness = 1;
constexpr uint8_t kMaxFooterMetric = 2;
constexpr uint8_t kMaxBatteryLabel = 2;
constexpr uint8_t kMaxReaderFontSize = 2;
constexpr uint8_t kMaxReaderTypeface = 2;
constexpr uint8_t kMaxPauseMode = 1;
constexpr uint16_t kDefaultPacingDelayMs = 200;
constexpr uint16_t kMaxPacingDelayMs = 600;
constexpr int8_t kMinTypographyTracking = -2;
constexpr int8_t kMaxTypographyTracking = 3;
constexpr uint8_t kMinTypographyAnchor = 30;
constexpr uint8_t kMaxTypographyAnchor = 40;
constexpr uint8_t kDefaultTypographyAnchor = 30;
constexpr uint8_t kMinTypographyGuideWidth = 12;
constexpr uint8_t kMaxTypographyGuideWidth = 30;
constexpr uint8_t kDefaultTypographyGuideWidth = 30;
constexpr uint8_t kMinTypographyGuideGap = 2;
constexpr uint8_t kMaxTypographyGuideGap = 8;
constexpr uint8_t kDefaultTypographyGuideGap = 5;

uint16_t clampU16(uint16_t value, uint16_t minValue, uint16_t maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

int clampInt(int value, int minValue, int maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

String enumLabel(uint8_t value, const char *const *labels, size_t count, uint8_t fallback = 0) {
  if (value >= count) {
    value = fallback;
  }
  return labels[value];
}

int enumValue(const String &value, const char *const *labels, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (value == labels[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool findJsonKey(const String &body, const char *key, int &colonIndex) {
  const String needle = String("\"") + key + "\"";
  const int keyIndex = body.indexOf(needle);
  if (keyIndex < 0) return false;
  colonIndex = body.indexOf(':', keyIndex + needle.length());
  return colonIndex >= 0;
}

int skipJsonWhitespace(const String &body, int index) {
  while (index < static_cast<int>(body.length()) &&
         isspace(static_cast<unsigned char>(body[index]))) {
    ++index;
  }
  return index;
}

bool readJsonInt(const String &body, const char *key, int &value) {
  int colonIndex = -1;
  if (!findJsonKey(body, key, colonIndex)) return false;
  int index = skipJsonWhitespace(body, colonIndex + 1);
  bool negative = false;
  if (index < static_cast<int>(body.length()) && body[index] == '-') {
    negative = true;
    ++index;
  }
  if (index >= static_cast<int>(body.length()) ||
      !isdigit(static_cast<unsigned char>(body[index]))) {
    return false;
  }
  int result = 0;
  while (index < static_cast<int>(body.length()) &&
         isdigit(static_cast<unsigned char>(body[index]))) {
    result = result * 10 + (body[index] - '0');
    ++index;
  }
  value = negative ? -result : result;
  return true;
}

bool readJsonBool(const String &body, const char *key, bool &value) {
  int colonIndex = -1;
  if (!findJsonKey(body, key, colonIndex)) return false;
  const int index = skipJsonWhitespace(body, colonIndex + 1);
  if (body.substring(index, index + 4) == "true") {
    value = true;
    return true;
  }
  if (body.substring(index, index + 5) == "false") {
    value = false;
    return true;
  }
  return false;
}

bool readJsonStringValue(const String &body, const char *key, String &value) {
  int colonIndex = -1;
  if (!findJsonKey(body, key, colonIndex)) return false;
  int index = skipJsonWhitespace(body, colonIndex + 1);
  if (index >= static_cast<int>(body.length()) || body[index] != '"') return false;
  ++index;
  String result;
  while (index < static_cast<int>(body.length())) {
    const char c = body[index++];
    if (c == '"') {
      value = result;
      return true;
    }
    if (c == '\\' && index < static_cast<int>(body.length())) {
      const char next = body[index++];
      switch (next) {
        case 'n': result += '\n'; break;
        case 't': result += '\t'; break;
        case 'r': result += '\r'; break;
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        default: result += next; break;
      }
      continue;
    }
    result += c;
  }
  return false;
}

bool isSupportedBookName(const String &loweredName) {
  return loweredName.endsWith(".rsvp") || loweredName.endsWith(".txt") ||
         loweredName.endsWith(".epub");
}

String displayNameForPath(const String &path) {
  const int separator = path.lastIndexOf('/');
  if (separator < 0) {
    return path;
  }
  return path.substring(separator + 1);
}

String libraryCategoryForPath(const String &path) {
  if (path.startsWith(String(RsvpDataStore::kArticleFilesPath) + "/")) {
    return "article";
  }
  if (path.startsWith(String(RsvpDataStore::kBookFilesPath) + "/")) {
    return "book";
  }
  return "book";  // legacy /books/<file> defaults to book
}

bool directiveMatches(const String &loweredLine, const char *directive) {
  if (!loweredLine.startsWith(directive)) {
    return false;
  }
  const size_t directiveLength = strlen(directive);
  return loweredLine.length() == directiveLength ||
         isspace(static_cast<unsigned char>(loweredLine[directiveLength]));
}

String directiveValue(const String &line, const char *directive) {
  String value = line.substring(strlen(directive));
  value.trim();
  return value;
}

}  // namespace

bool RsvpDataStore::begin() {
  if (began_) {
    return true;
  }
  if (!preferences_.begin(kPrefsNamespace, false)) {
    return false;
  }
  began_ = true;
  return true;
}

void RsvpDataStore::end() {
  if (began_) {
    preferences_.end();
    began_ = false;
  }
}

String RsvpDataStore::hashBookPath(const String &path) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < path.length(); ++i) {
    hash ^= static_cast<uint8_t>(path[i]);
    hash *= 16777619UL;
  }
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hash));
  return String(buf);
}

String RsvpDataStore::bookPositionKey(const String &hash) {
  return String("p") + hash;
}

String RsvpDataStore::bookWordCountKey(const String &hash) {
  return String("c") + hash;
}

RsvpDataStore::RsvpMetadata RsvpDataStore::readRsvpMetadata(const String &path) {
  RsvpMetadata metadata;
  String loweredPath = path;
  loweredPath.toLowerCase();
  if (!loweredPath.endsWith(".rsvp")) {
    return metadata;
  }

  File file = SD_MMC.open(path);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return metadata;
  }

  String line;
  bool pastDirectives = false;
  const auto consumeLine = [&](const String &rawLine) {
    String trimmed = rawLine;
    trimmed.trim();
    if (trimmed.isEmpty()) {
      return;
    }
    String lowered = trimmed;
    lowered.toLowerCase();
    if (!lowered.startsWith("@")) {
      pastDirectives = true;
      return;
    }
    if (metadata.title.isEmpty() && directiveMatches(lowered, "@title")) {
      metadata.title = directiveValue(trimmed, "@title");
    } else if (metadata.author.isEmpty() && directiveMatches(lowered, "@author")) {
      metadata.author = directiveValue(trimmed, "@author");
    }
  };

  while (file.available()) {
    const char c = static_cast<char>(file.read());
    if (c == '\r') {
      continue;
    }
    if (c != '\n') {
      line += c;
      if (line.length() > kMaxMetadataLineChars) {
        pastDirectives = true;
        line = "";
        break;
      }
      continue;
    }
    consumeLine(line);
    if (pastDirectives) {
      break;
    }
    if (!metadata.title.isEmpty() && !metadata.author.isEmpty()) {
      break;
    }
    line = "";
  }
  if (!line.isEmpty() && !pastDirectives) {
    consumeLine(line);
  }

  file.close();
  return metadata;
}

std::vector<RsvpDataStore::BookEntry> RsvpDataStore::listBooks() {
  std::vector<BookEntry> entries;
  if (!began_) {
    return entries;
  }

  const auto appendDirectory = [&](const char *directoryPath) {
    File dir = SD_MMC.open(directoryPath);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      return;
    }

    File entry = dir.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        const String name = displayNameForPath(String(entry.name()));
        const String path = String(directoryPath) + "/" + name;
        String lowered = name;
        lowered.toLowerCase();
        if (isSupportedBookName(lowered)) {
          BookEntry book;
          book.path = path;
          book.filename = name;
          book.category = libraryCategoryForPath(path);
          book.bytes = static_cast<uint32_t>(entry.size());
          book.hash = hashBookPath(path);

          const RsvpMetadata metadata = readRsvpMetadata(path);
          book.title = metadata.title;
          book.author = metadata.author;

          const String posKey = bookPositionKey(book.hash);
          const String cntKey = bookWordCountKey(book.hash);
          if (preferences_.isKey(cntKey.c_str())) {
            book.words = preferences_.getUInt(cntKey.c_str(), 0);
          }
          if (preferences_.isKey(posKey.c_str())) {
            book.progressWords = preferences_.getUInt(posKey.c_str(), 0);
          }

          entries.push_back(std::move(book));
        }
      }
      entry.close();
      entry = dir.openNextFile();
    }
    dir.close();
  };

  appendDirectory(kBooksPath);
  appendDirectory(kBookFilesPath);
  appendDirectory(kArticleFilesPath);
  return entries;
}

String RsvpDataStore::resolvePathByHash(const String &hash) {
  if (hash.isEmpty()) {
    return String();
  }
  const auto books = listBooks();
  for (const auto &book : books) {
    if (book.hash == hash) {
      return book.path;
    }
  }
  return String();
}

bool RsvpDataStore::deleteBook(const String &hash) {
  if (!began_ || hash.isEmpty()) {
    return false;
  }
  const String path = resolvePathByHash(hash);
  if (path.isEmpty()) {
    return false;
  }
  if (!SD_MMC.remove(path)) {
    return false;
  }
  const String posKey = bookPositionKey(hash);
  const String cntKey = bookWordCountKey(hash);
  if (preferences_.isKey(posKey.c_str())) {
    preferences_.remove(posKey.c_str());
  }
  if (preferences_.isKey(cntKey.c_str())) {
    preferences_.remove(cntKey.c_str());
  }
  if (activeBookHash() == hash) {
    preferences_.putString(kPrefActiveBook, "");
  }
  return true;
}

bool RsvpDataStore::readPosition(const String &hash, uint32_t &wordIndex, uint32_t &wordCount) {
  if (!began_ || hash.isEmpty()) {
    return false;
  }
  const String posKey = bookPositionKey(hash);
  const String cntKey = bookWordCountKey(hash);
  wordIndex = preferences_.isKey(posKey.c_str()) ? preferences_.getUInt(posKey.c_str(), 0) : 0;
  wordCount = preferences_.isKey(cntKey.c_str()) ? preferences_.getUInt(cntKey.c_str(), 0) : 0;
  return true;
}

bool RsvpDataStore::writePosition(const String &hash, uint32_t wordIndex) {
  if (!began_ || hash.isEmpty()) {
    return false;
  }
  const String posKey = bookPositionKey(hash);
  preferences_.putUInt(posKey.c_str(), wordIndex);
  return true;
}

String RsvpDataStore::activeBookHash() {
  if (!began_) {
    return String();
  }
  return preferences_.getString(kPrefActiveBook, "");
}

bool RsvpDataStore::setActiveBookHash(const String &hash) {
  if (!began_) {
    return false;
  }
  preferences_.putString(kPrefActiveBook, hash);
  return true;
}

String RsvpDataStore::sanitizeFilename(const String &name) {
  String trimmed = name;
  trimmed.trim();
  String out;
  out.reserve(trimmed.length());
  for (size_t i = 0; i < trimmed.length(); ++i) {
    const char c = trimmed[i];
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ' ';
    out += safe ? c : '_';
  }
  while (out.startsWith(".") || out.startsWith("/")) {
    out.remove(0, 1);
  }
  return out;
}

bool RsvpDataStore::beginUpload(const String &category, const String &filename, String &error) {
  if (uploadFile_) {
    error = "Upload already in progress";
    return false;
  }
  const String safeName = sanitizeFilename(filename);
  if (safeName.isEmpty()) {
    error = "Missing filename";
    return false;
  }
  String lowered = safeName;
  lowered.toLowerCase();
  String finalName = safeName;
  if (!lowered.endsWith(".rsvp") && !lowered.endsWith(".txt") && !lowered.endsWith(".epub")) {
    finalName += ".rsvp";
  }

  String categoryLower = category;
  categoryLower.toLowerCase();
  const char *targetDir = categoryLower == "article" ? kArticleFilesPath : kBookFilesPath;

  SD_MMC.mkdir(kBooksPath);
  SD_MMC.mkdir(targetDir);

  uploadFinalPath_ = String(targetDir) + "/" + finalName;
  uploadTmpPath_ = uploadFinalPath_ + ".tmp";
  SD_MMC.remove(uploadTmpPath_);
  uploadFile_ = SD_MMC.open(uploadTmpPath_, FILE_WRITE);
  if (!uploadFile_) {
    error = "Could not create file";
    uploadFinalPath_ = "";
    uploadTmpPath_ = "";
    return false;
  }
  return true;
}

bool RsvpDataStore::appendUpload(const uint8_t *bytes, size_t len) {
  if (!uploadFile_ || bytes == nullptr || len == 0) {
    return false;
  }
  const size_t written = uploadFile_.write(bytes, len);
  return written == len;
}

bool RsvpDataStore::finishUpload(bool success) {
  if (uploadFile_) {
    uploadFile_.close();
  }
  if (uploadTmpPath_.isEmpty()) {
    return false;
  }

  bool ok = false;
  if (success) {
    SD_MMC.remove(uploadFinalPath_);
    if (SD_MMC.rename(uploadTmpPath_, uploadFinalPath_)) {
      lastUploadedPath_ = uploadFinalPath_;
      ok = true;
    } else {
      SD_MMC.remove(uploadTmpPath_);
    }
  } else {
    SD_MMC.remove(uploadTmpPath_);
  }

  uploadFinalPath_ = "";
  uploadTmpPath_ = "";
  return ok;
}

String RsvpDataStore::settingsJson() {
  static const char *const readerModeLabels[] = {"rsvp", "scroll"};
  static const char *const handednessLabels[] = {"right", "left"};
  static const char *const footerMetricLabels[] = {"percentage", "chapter_time", "book_time"};
  static const char *const batteryLabelLabels[] = {"percent", "time_remaining", "voltage"};
  static const char *const typefaceLabels[] = {"standard", "open_dyslexic", "atkinson"};
  static const char *const pauseModeLabels[] = {"sentence_end", "instant"};

  if (!began_) {
    return String("{\"ok\":false,\"error\":\"store not initialized\"}");
  }

  const uint16_t wpm =
      clampU16(preferences_.getUShort(kPrefWpm, kDefaultWpm), kMinWpm, kMaxWpm);
  const uint8_t readerMode = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefReaderMode, 0), 0, kMaxReaderMode));
  const uint8_t pauseMode = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefPauseMode, 0), 0, kMaxPauseMode));
  const uint16_t longDelay =
      clampU16(preferences_.getUShort(kPrefPacingLongMs, kDefaultPacingDelayMs), 0,
               kMaxPacingDelayMs);
  const uint16_t complexDelay =
      clampU16(preferences_.getUShort(kPrefPacingComplexMs, kDefaultPacingDelayMs), 0,
               kMaxPacingDelayMs);
  const uint16_t punctuationDelay =
      clampU16(preferences_.getUShort(kPrefPacingPunctuationMs, kDefaultPacingDelayMs), 0,
               kMaxPacingDelayMs);
  const uint8_t brightness = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefBrightness, kDefaultBrightness), 0, kMaxBrightness));
  const uint8_t handedness = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefHandedness, 0), 0, kMaxHandedness));
  const uint8_t footerMetric = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefFooterMetricMode, 0), 0, kMaxFooterMetric));
  const uint8_t batteryLabel = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefBatteryLabelMode, 0), 0, kMaxBatteryLabel));
  const uint8_t language = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefUiLanguage, 0), 0, kMaxUiLanguage));
  const uint8_t fontSize = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefReaderFontSize, 0), 0, kMaxReaderFontSize));
  const uint8_t typeface = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefReaderTypeface, 0), 0, kMaxReaderTypeface));
  const int tracking = clampInt(preferences_.getChar(kPrefTypographyTracking, 0),
                                kMinTypographyTracking, kMaxTypographyTracking);
  const uint8_t anchor = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefTypographyAnchor, kDefaultTypographyAnchor),
               kMinTypographyAnchor, kMaxTypographyAnchor));
  const uint8_t guideWidth = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefTypographyGuideWidth, kDefaultTypographyGuideWidth),
               kMinTypographyGuideWidth, kMaxTypographyGuideWidth));
  const uint8_t guideGap = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefTypographyGuideGap, kDefaultTypographyGuideGap),
               kMinTypographyGuideGap, kMaxTypographyGuideGap));

  String body;
  body.reserve(1250);
  body += "{\"ok\":true,\"version\":1";
  body += ",\"reading\":{";
  body += "\"wpm\":" + String(wpm);
  body += ",\"readerMode\":\"" + enumLabel(readerMode, readerModeLabels, 2) + "\"";
  body += ",\"pauseMode\":\"" + enumLabel(pauseMode, pauseModeLabels, 2) + "\"";
  body += ",\"accurateTimeEstimate\":true";
  body += ",\"pacing\":{\"longWordMs\":" + String(longDelay) +
          ",\"complexWordMs\":" + String(complexDelay) +
          ",\"punctuationMs\":" + String(punctuationDelay) + "}";
  body += "}";
  body += ",\"display\":{";
  body += "\"brightnessIndex\":" + String(brightness);
  body += ",\"darkMode\":" + String(preferences_.getBool(kPrefDarkMode, false) ? "true" : "false");
  body += ",\"nightMode\":" +
          String(preferences_.getBool(kPrefNightMode, false) ? "true" : "false");
  body += ",\"handedness\":\"" + enumLabel(handedness, handednessLabels, 2) + "\"";
  body += ",\"footerMetric\":\"" + enumLabel(footerMetric, footerMetricLabels, 3) + "\"";
  body += ",\"batteryLabel\":\"" + enumLabel(batteryLabel, batteryLabelLabels, 3) + "\"";
  body += ",\"readingBattery\":" +
          String(preferences_.getBool(kPrefReaderBatteryVisible, true) ? "true" : "false");
  body += ",\"readingChapter\":" +
          String(preferences_.getBool(kPrefReaderChapterVisible, false) ? "true" : "false");
  body += ",\"readingProgress\":" +
          String(preferences_.getBool(kPrefReaderProgressVisible, false) ? "true" : "false");
  body += ",\"language\":" + String(language);
  body += ",\"phantomWords\":" +
          String(preferences_.getBool(kPrefPhantomWords, true) ? "true" : "false");
  body += ",\"fontSizeIndex\":" + String(fontSize);
  body += "}";
  body += ",\"typography\":{";
  body += "\"typeface\":\"" + enumLabel(typeface, typefaceLabels, 3) + "\"";
  body += ",\"focusHighlight\":" +
          String(preferences_.getBool(kPrefTypographyFocusHighlight, true) ? "true" : "false");
  body += ",\"tracking\":" + String(tracking);
  body += ",\"anchorPercent\":" + String(anchor);
  body += ",\"guideWidth\":" + String(guideWidth);
  body += ",\"guideGap\":" + String(guideGap);
  body += "}";
  body += ",\"limits\":{";
  body += "\"wpm\":{\"min\":" + String(kMinWpm) + ",\"max\":" + String(kMaxWpm) + "}";
  body += ",\"brightnessIndex\":{\"min\":0,\"max\":" + String(kMaxBrightness) + "}";
  body += ",\"pacingMs\":{\"min\":0,\"max\":" + String(kMaxPacingDelayMs) + "}";
  body += ",\"tracking\":{\"min\":" + String(kMinTypographyTracking) +
          ",\"max\":" + String(kMaxTypographyTracking) + "}";
  body += ",\"anchorPercent\":{\"min\":" + String(kMinTypographyAnchor) +
          ",\"max\":" + String(kMaxTypographyAnchor) + "}";
  body += ",\"guideWidth\":{\"min\":" + String(kMinTypographyGuideWidth) +
          ",\"max\":" + String(kMaxTypographyGuideWidth) + "}";
  body += ",\"guideGap\":{\"min\":" + String(kMinTypographyGuideGap) +
          ",\"max\":" + String(kMaxTypographyGuideGap) + "}";
  body += "}}";
  return body;
}

bool RsvpDataStore::applySettingsJson(const String &body, String &error) {
  if (!began_) {
    error = "store not initialized";
    return false;
  }
  if (body.isEmpty()) {
    error = "Missing settings JSON";
    return false;
  }
  static const char *const readerModeLabels[] = {"rsvp", "scroll"};
  static const char *const handednessLabels[] = {"right", "left"};
  static const char *const footerMetricLabels[] = {"percentage", "chapter_time", "book_time"};
  static const char *const batteryLabelLabels[] = {"percent", "time_remaining", "voltage"};
  static const char *const typefaceLabels[] = {"standard", "open_dyslexic", "atkinson"};
  static const char *const pauseModeLabels[] = {"sentence_end", "instant"};

  int intValue = 0;
  bool boolValue = false;
  String stringValue;

  if (readJsonInt(body, "wpm", intValue)) {
    if (intValue < kMinWpm || intValue > kMaxWpm) {
      error = "wpm must be between 10 and 1000";
      return false;
    }
    preferences_.putUShort(kPrefWpm, static_cast<uint16_t>(intValue));
  }
  if (readJsonStringValue(body, "readerMode", stringValue)) {
    const int v = enumValue(stringValue, readerModeLabels, 2);
    if (v < 0) { error = "readerMode must be rsvp or scroll"; return false; }
    preferences_.putUChar(kPrefReaderMode, static_cast<uint8_t>(v));
  }
  if (readJsonStringValue(body, "pauseMode", stringValue)) {
    const int v = enumValue(stringValue, pauseModeLabels, 2);
    if (v < 0) { error = "pauseMode must be sentence_end or instant"; return false; }
    preferences_.putUChar(kPrefPauseMode, static_cast<uint8_t>(v));
  }
  preferences_.putBool(kPrefAccurateTime, true);
  if (readJsonInt(body, "longWordMs", intValue)) {
    if (intValue < 0 || intValue > kMaxPacingDelayMs) {
      error = "longWordMs must be between 0 and 600"; return false;
    }
    preferences_.putUShort(kPrefPacingLongMs, static_cast<uint16_t>(intValue));
  }
  if (readJsonInt(body, "complexWordMs", intValue)) {
    if (intValue < 0 || intValue > kMaxPacingDelayMs) {
      error = "complexWordMs must be between 0 and 600"; return false;
    }
    preferences_.putUShort(kPrefPacingComplexMs, static_cast<uint16_t>(intValue));
  }
  if (readJsonInt(body, "punctuationMs", intValue)) {
    if (intValue < 0 || intValue > kMaxPacingDelayMs) {
      error = "punctuationMs must be between 0 and 600"; return false;
    }
    preferences_.putUShort(kPrefPacingPunctuationMs, static_cast<uint16_t>(intValue));
  }
  if (readJsonInt(body, "brightnessIndex", intValue)) {
    if (intValue < 0 || intValue > kMaxBrightness) {
      error = "brightnessIndex must be between 0 and 4"; return false;
    }
    preferences_.putUChar(kPrefBrightness, static_cast<uint8_t>(intValue));
  }
  if (readJsonBool(body, "darkMode", boolValue)) preferences_.putBool(kPrefDarkMode, boolValue);
  if (readJsonBool(body, "nightMode", boolValue)) preferences_.putBool(kPrefNightMode, boolValue);
  if (readJsonStringValue(body, "handedness", stringValue)) {
    const int v = enumValue(stringValue, handednessLabels, 2);
    if (v < 0) { error = "handedness must be right or left"; return false; }
    preferences_.putUChar(kPrefHandedness, static_cast<uint8_t>(v));
  }
  if (readJsonStringValue(body, "footerMetric", stringValue)) {
    const int v = enumValue(stringValue, footerMetricLabels, 3);
    if (v < 0) { error = "footerMetric must be percentage, chapter_time, or book_time"; return false; }
    preferences_.putUChar(kPrefFooterMetricMode, static_cast<uint8_t>(v));
  }
  if (readJsonStringValue(body, "batteryLabel", stringValue)) {
    const int v = enumValue(stringValue, batteryLabelLabels, 3);
    if (v < 0) { error = "batteryLabel must be percent, time_remaining, or voltage"; return false; }
    preferences_.putUChar(kPrefBatteryLabelMode, static_cast<uint8_t>(v));
  }
  if (readJsonBool(body, "readingBattery", boolValue)) preferences_.putBool(kPrefReaderBatteryVisible, boolValue);
  if (readJsonBool(body, "readingChapter", boolValue)) preferences_.putBool(kPrefReaderChapterVisible, boolValue);
  if (readJsonBool(body, "readingProgress", boolValue)) preferences_.putBool(kPrefReaderProgressVisible, boolValue);
  if (readJsonInt(body, "language", intValue)) {
    if (intValue < 0 || intValue > kMaxUiLanguage) { error = "language is out of range"; return false; }
    preferences_.putUChar(kPrefUiLanguage, static_cast<uint8_t>(intValue));
  }
  if (readJsonBool(body, "phantomWords", boolValue)) preferences_.putBool(kPrefPhantomWords, boolValue);
  if (readJsonInt(body, "fontSizeIndex", intValue)) {
    if (intValue < 0 || intValue > kMaxReaderFontSize) { error = "fontSizeIndex must be between 0 and 2"; return false; }
    preferences_.putUChar(kPrefReaderFontSize, static_cast<uint8_t>(intValue));
  }
  if (readJsonStringValue(body, "typeface", stringValue)) {
    const int v = enumValue(stringValue, typefaceLabels, 3);
    if (v < 0) { error = "typeface must be standard, open_dyslexic, or atkinson"; return false; }
    preferences_.putUChar(kPrefReaderTypeface, static_cast<uint8_t>(v));
  }
  if (readJsonBool(body, "focusHighlight", boolValue)) preferences_.putBool(kPrefTypographyFocusHighlight, boolValue);
  if (readJsonInt(body, "tracking", intValue)) {
    if (intValue < kMinTypographyTracking || intValue > kMaxTypographyTracking) {
      error = "tracking is out of range"; return false;
    }
    preferences_.putChar(kPrefTypographyTracking, static_cast<int8_t>(intValue));
  }
  if (readJsonInt(body, "anchorPercent", intValue)) {
    if (intValue < kMinTypographyAnchor || intValue > kMaxTypographyAnchor) {
      error = "anchorPercent is out of range"; return false;
    }
    preferences_.putUChar(kPrefTypographyAnchor, static_cast<uint8_t>(intValue));
  }
  if (readJsonInt(body, "guideWidth", intValue)) {
    if (intValue < kMinTypographyGuideWidth || intValue > kMaxTypographyGuideWidth) {
      error = "guideWidth is out of range"; return false;
    }
    preferences_.putUChar(kPrefTypographyGuideWidth, static_cast<uint8_t>(intValue));
  }
  if (readJsonInt(body, "guideGap", intValue)) {
    if (intValue < kMinTypographyGuideGap || intValue > kMaxTypographyGuideGap) {
      error = "guideGap is out of range"; return false;
    }
    preferences_.putUChar(kPrefTypographyGuideGap, static_cast<uint8_t>(intValue));
  }
  return true;
}

RsvpDataStore::StorageInfo RsvpDataStore::storage() {
  StorageInfo info;
  info.totalBytes = SD_MMC.totalBytes();
  const uint64_t used = SD_MMC.usedBytes();
  info.freeBytes = info.totalBytes > used ? info.totalBytes - used : 0;
  info.bookCount = static_cast<uint32_t>(listBooks().size());
  return info;
}
