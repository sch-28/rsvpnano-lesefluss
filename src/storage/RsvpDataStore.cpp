#include "storage/RsvpDataStore.h"

#include <SD_MMC.h>
#include <cstdio>

namespace {

constexpr const char *kPrefsNamespace = "rsvp";
constexpr const char *kPrefActiveBook = "active";
constexpr size_t kMaxMetadataLineChars = 160;

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

RsvpDataStore::StorageInfo RsvpDataStore::storage() {
  StorageInfo info;
  info.totalBytes = SD_MMC.totalBytes();
  const uint64_t used = SD_MMC.usedBytes();
  info.freeBytes = info.totalBytes > used ? info.totalBytes - used : 0;
  info.bookCount = static_cast<uint32_t>(listBooks().size());
  return info;
}
