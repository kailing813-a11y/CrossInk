#include "ClippingStore.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <uzlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <functional>

#include "activities/reader/ReadingStatsUtils.h"
#include "clippings/ClippingIdentity.h"
#include "clippings/ClippingPreview.h"
#include "clippings/ClippingSyncPlan.h"

namespace {
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t TEXT_OFFSET_VERSION = 2;
constexpr uint8_t LAYOUT_SIGNATURE_VERSION = 3;
constexpr uint8_t VERSION = 4;
constexpr size_t INITIAL_CLIPPING_RESERVE = 4;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";
constexpr char DELETIONS_DIR[] = "/.crosspoint/clippings-deleted";
constexpr size_t TEXT_COPY_BUFFER_SIZE = 128;
// "<16 hex id> <8 hex key>\n"
constexpr size_t DELETION_LINE_SIZE = 16 + 1 + 8 + 1;
// Bound the file read during sync; far above any realistic backlog.
constexpr size_t DELETIONS_MAX = 1024;

// /.crosspoint/clippings/epub_123.bin -> /.crosspoint/clippings-deleted/epub_123.txt
std::string deletionLogPath(const std::string& storePath) {
  const size_t slash = storePath.find_last_of('/');
  std::string name = slash == std::string::npos ? storePath : storePath.substr(slash + 1);
  const size_t dot = name.rfind(".bin");
  if (dot != std::string::npos) name.resize(dot);
  return std::string(DELETIONS_DIR) + "/" + name + ".txt";
}

bool parseHex(const char* text, const size_t digits, uint32_t& out) {
  out = 0;
  for (size_t i = 0; i < digits; ++i) {
    const char c = text[i];
    uint32_t v;
    if (c >= '0' && c <= '9')
      v = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      v = static_cast<uint32_t>(c - 'a' + 10);
    else
      return false;
    out = (out << 4) | v;
  }
  return true;
}

uint32_t creationTimeUtc() {
  const time_t now = time(nullptr);
  if (now >= 946684800LL && static_cast<uint64_t>(now) <= UINT32_MAX) return static_cast<uint32_t>(now);
  uint16_t year;
  uint8_t month, day, hour, minute;
  if (halClock.getDateTime(year, month, day, hour, minute)) {
    const ReadingStatsDate date{year, month, day};
    if (date.isValid() && hour < 24 && minute < 60) {
      const uint64_t utc =
          946684800ULL + static_cast<uint64_t>(readingStatsDayIndex(date)) * 86400 + hour * 3600U + minute * 60U;
      if (utc <= UINT32_MAX) return static_cast<uint32_t>(utc);
    }
  }
  return 0;  // No clock: preserve an unknown date instead of inventing one.
}

struct ClippingFileHeader {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType;
  uint16_t count = 0;
};

std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
  dst[utf8SafeTruncateBuffer(dst, static_cast<int>(strlen(dst)))] = '\0';
}

bool readClippingFileHeader(const std::string& fullPath, const char* name, ClippingFileHeader& header) {
  FsFile f;
  if (!Storage.openFileForRead("CLIP", fullPath, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, version) ||
      (version != LEGACY_VERSION && version != TEXT_OFFSET_VERSION && version != LAYOUT_SIGNATURE_VERSION &&
       version != VERSION) ||
      !serialization::tryReadPod(f, count) || !serialization::tryReadString(f, header.title) ||
      !serialization::tryReadString(f, header.author) || !serialization::tryReadString(f, header.path)) {
    f.close();
    return false;
  }
  f.close();

  if (count > CLIPPING_MAX_PER_BOOK) {
    return false;
  }
  header.count = count;
  header.bookType = "epub";
  const std::string nameStr = name ? name : "";
  const size_t underscorePos = nameStr.find('_');
  if (underscorePos != std::string::npos) {
    header.bookType = nameStr.substr(0, underscorePos);
  }
  return true;
}

bool copyBytes(FsFile& in, FsFile& out, uint16_t length) {
  std::array<uint8_t, TEXT_COPY_BUFFER_SIZE> buffer{};
  while (length > 0) {
    const size_t chunk = std::min<size_t>(length, buffer.size());
    if (in.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      return false;
    }
    if (out.write(buffer.data(), chunk) != chunk) {
      return false;
    }
    length = static_cast<uint16_t>(length - chunk);
  }
  return true;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub") {
    LOG_ERR("CLIP", "Unknown clipping book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  clippings.clear();
  if (clippings.capacity() < INITIAL_CLIPPING_RESERVE) {
    clippings.reserve(INITIAL_CLIPPING_RESERVE);
  }

  storeFilePath = storeFilePathForBook(filePath, bookType);
  if (!Storage.exists(storeFilePath.c_str())) {
    return true;
  }

  if (!readFromFile()) return false;
  // Sync loads a book by path only; keep the saved title and author so a
  // rewrite does not blank them in the clipped-books list.
  if (bookTitle.empty() && bookAuthor.empty()) {
    ClippingFileHeader header;
    if (readClippingFileHeader(storeFilePath, nullptr, header)) {
      bookTitle = std::move(header.title);
      bookAuthor = std::move(header.author);
    }
  }
  return true;
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  std::vector<Clipping>().swap(clippings);
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text,
                                                    const uint16_t tableSelection, const uint32_t layoutSignature) {
  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = pageCount;
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = paragraphIndex;
  clipping.timestamp = creationTimeUtc();
  clipping.layoutSignature = layoutSignature;
  clipping.tableSelection = tableSelection;
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  return appendRecord(std::move(clipping), text);
}

ClippingStore::AddResult ClippingStore::addSyncedClipping(const Clipping& record, const std::string& text) {
  Clipping clipping = record;
  clipping.resolvedLayoutBoundaries = 0;
  clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
  clipping.chapterTitle[utf8SafeTruncateBuffer(clipping.chapterTitle,
                                               static_cast<int>(strlen(clipping.chapterTitle)))] = '\0';
  return appendRecord(std::move(clipping), text);
}

ClippingStore::AddResult ClippingStore::appendRecord(Clipping clipping, const std::string& text) {
  if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }
  clipping.pageCount = std::max<uint16_t>(1, clipping.pageCount);
  const size_t cappedLength = std::min(text.size(), CLIPPING_TEXT_MAX);
  clipping.textLength = static_cast<uint16_t>(utf8SafeTruncateBuffer(text.data(), static_cast<int>(cappedLength)));

  clippings.push_back(std::move(clipping));
  dirty = true;
  if (!writeToFile(&text, clippings.size() - 1)) {
    clippings.pop_back();
    dirty = true;
    return AddResult::SaveFailed;
  }
  dirty = false;
  return AddResult::Added;
}

bool ClippingStore::removeClippingAt(const size_t index, const bool recordDeletion) {
  if (index >= clippings.size()) return false;
  // Record before the text is gone. A failed log write keeps the local delete:
  // the clipping can still be deleted elsewhere, and the user asked for it gone.
  if (recordDeletion && !appendPendingDeletion(clippings[index])) {
    LOG_ERR("CLIP", "Clipping deletion will not sync: %s", storeFilePath.c_str());
  }
  Clipping clipping = std::move(clippings[index]);
  clippings.erase(clippings.begin() + index);
  dirty = true;
  if (!saveToFile()) {
    clippings.insert(clippings.begin() + index, std::move(clipping));
    dirty = true;
    return false;
  }
  return true;
}

bool ClippingStore::clippingSyncKeys(const size_t index, char (&id)[17], uint32_t& key) const {
  const Clipping* clipping = clippingAt(index);
  std::string text;
  if (!clipping || !readClippingText(*clipping, text)) return false;
  key = clippingSyncPlan::textKey(clipping->spineIndex, text.data(), text.size());
  return clippingIdentity::compute(clippingUnixTimestamp(clipping->timestamp), text, id);
}

bool ClippingStore::appendPendingDeletion(const Clipping& clipping) const {
  std::string text;
  if (!readClippingText(clipping, text)) return false;
  char id[17];
  if (!clippingIdentity::compute(clippingUnixTimestamp(clipping.timestamp), text, id)) return false;
  const uint32_t key = clippingSyncPlan::textKey(clipping.spineIndex, text.data(), text.size());
  char line[DELETION_LINE_SIZE + 1];
  snprintf(line, sizeof(line), "%s %08lx\n", id, static_cast<unsigned long>(key));
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(DELETIONS_DIR);
  const std::string path = deletionLogPath(storeFilePath);
  FsFile f = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_AT_END);
  if (!f) {
    LOG_ERR("CLIP", "Failed to open clipping deletion log: %s", path.c_str());
    return false;
  }
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(line), DELETION_LINE_SIZE) == DELETION_LINE_SIZE;
  f.close();
  if (!ok) LOG_ERR("CLIP", "Failed to write clipping deletion log: %s", path.c_str());
  return ok;
}

bool ClippingStore::readPendingDeletions(const std::string& filePath, std::vector<PendingDeletion>& out) {
  out.clear();
  const std::string path = deletionLogPath(storeFilePathForBook(filePath, "epub"));
  if (!Storage.exists(path.c_str())) return true;
  FsFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) return false;
  char line[DELETION_LINE_SIZE];
  // Lines are fixed-width; a torn final line from a power loss is ignored.
  while (out.size() < DELETIONS_MAX &&
         f.read(reinterpret_cast<uint8_t*>(line), DELETION_LINE_SIZE) == static_cast<int>(DELETION_LINE_SIZE)) {
    PendingDeletion entry;
    uint32_t ignored = 0;
    if (line[16] != ' ' || line[DELETION_LINE_SIZE - 1] != '\n' || !parseHex(line, 16, ignored) ||
        !parseHex(line + 17, 8, entry.key)) {
      LOG_ERR("CLIP", "Skipping malformed clipping deletion entry: %s", path.c_str());
      continue;
    }
    std::memcpy(entry.id, line, 16);
    entry.id[16] = '\0';
    out.push_back(entry);
  }
  f.close();
  return true;
}

bool ClippingStore::clearPendingDeletions(const std::string& filePath) {
  const std::string path = deletionLogPath(storeFilePathForBook(filePath, "epub"));
  return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
}

void ClippingStore::movePendingDeletions(const std::string& oldStorePath, const std::string& newStorePath) {
  const std::string from = deletionLogPath(oldStorePath);
  const std::string to = deletionLogPath(newStorePath);
  if (from == to || !Storage.exists(from.c_str())) return;
  if (Storage.exists(to.c_str())) Storage.remove(to.c_str());
  if (!Storage.rename(from.c_str(), to.c_str())) {
    LOG_ERR("CLIP", "Failed to move clipping deletion log: %s", from.c_str());
  }
}

const Clipping* ClippingStore::clippingAt(const size_t index) const {
  if (index >= clippings.size()) return nullptr;
  return &clippings[index];
}

bool ClippingStore::cacheResolvedLayoutRange(const size_t index, const uint16_t page, const uint16_t startWord,
                                             const uint16_t endWord, const uint32_t layoutSignature) {
  if (index >= clippings.size()) return false;
  if (!cacheClippingResolvedLayoutRange(clippings[index], page, startWord, endWord, layoutSignature)) {
    return false;
  }
  dirty = true;
  return true;
}

bool ClippingStore::readClippingPreview(const size_t index, std::string& out) const {
  out.clear();
  const Clipping* clipping = clippingAt(index);
  if (!clipping || storeFilePath.empty()) {
    LOG_ERR("CLIP", "Invalid clipping preview index: %u", static_cast<unsigned>(index));
    return false;
  }
  if (clipping->textLength == 0) return true;

  FsFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) return false;
  if (!f.seek(clipping->textOffset)) {
    f.close();
    LOG_ERR("CLIP", "Failed to seek clipping preview at %u", clipping->textOffset);
    return false;
  }
  const bool ok = clippingPreview::read(f, clipping->textLength, out);
  f.close();
  if (!ok) LOG_ERR("CLIP", "Failed to read clipping preview at %u", clipping->textOffset);
  return ok;
}

bool ClippingStore::readClippingText(const size_t index, std::string& out) const {
  const Clipping* clipping = clippingAt(index);
  if (!clipping) return false;
  return readClippingText(*clipping, out);
}

bool ClippingStore::readClippingText(const Clipping& clipping, std::string& out) const {
  out.clear();
  if (clipping.textLength == 0) return true;
  if (storeFilePath.empty()) return false;

  FsFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) {
    return false;
  }
  if (!f.seek(clipping.textOffset)) {
    f.close();
    LOG_ERR("CLIP", "Failed to seek clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
    return false;
  }
  out.resize(clipping.textLength);
  const int expected = static_cast<int>(clipping.textLength);
  const bool ok = f.read(&out[0], clipping.textLength) == expected;
  f.close();
  if (!ok) {
    out.clear();
    LOG_ERR("CLIP", "Failed to read clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
  }
  return ok;
}

bool ClippingStore::saveToFile() {
  if (!dirty) return true;
  if (writeToFile()) {
    dirty = false;
    return true;
  }
  return false;
}

void ClippingStore::clearAll(const bool recordDeletions) {
  if (recordDeletions) {
    for (const auto& clipping : clippings) {
      if (!appendPendingDeletion(clipping)) {
        LOG_ERR("CLIP", "Clipping deletion will not sync: %s", storeFilePath.c_str());
      }
    }
  }
  std::vector<Clipping>().swap(clippings);
  dirty = false;
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    Storage.remove(storeFilePath.c_str());
  }
}

bool ClippingStore::readFromFile() { return readFromFile(storeFilePath, clippings); }

bool ClippingStore::readFromFile(const std::string& path, std::vector<Clipping>& out, const size_t onlyIndex) const {
  out.clear();
  FsFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string storedPath;
  if (!serialization::tryReadPod(f, version) ||
      (version != LEGACY_VERSION && version != TEXT_OFFSET_VERSION && version != LAYOUT_SIGNATURE_VERSION &&
       version != VERSION) ||
      !serialization::tryReadPod(f, count) || !serialization::tryReadString(f, title) ||
      !serialization::tryReadString(f, author) || !serialization::tryReadString(f, storedPath)) {
    f.close();
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }

  if (count > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    f.close();
    return false;
  }

  if (onlyIndex != SIZE_MAX && onlyIndex >= count) {
    f.close();
    return true;
  }
  out.reserve(onlyIndex == SIZE_MAX ? count : 1);
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!serialization::tryReadPod(f, clipping.spineIndex) || !serialization::tryReadPod(f, clipping.startPage) ||
        !serialization::tryReadPod(f, clipping.endPage) || !serialization::tryReadPod(f, clipping.pageCount) ||
        !serialization::tryReadPod(f, clipping.startWordIndex) ||
        !serialization::tryReadPod(f, clipping.endWordIndex) || !serialization::tryReadPod(f, clipping.wordCount) ||
        !serialization::tryReadPod(f, clipping.paragraphIndex) || !serialization::tryReadPod(f, clipping.timestamp)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= LAYOUT_SIGNATURE_VERSION && !serialization::tryReadPod(f, clipping.layoutSignature)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at layout signature, record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= VERSION && !serialization::tryReadPod(f, clipping.tableSelection)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at table selection, record %u: %s", i, path.c_str());
      return false;
    }
    if (f.read(reinterpret_cast<uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
        sizeof(clipping.chapterTitle)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at chapter title, record %u: %s", i, path.c_str());
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    const int safeTitleLength =
        utf8SafeTruncateBuffer(clipping.chapterTitle, static_cast<int>(strlen(clipping.chapterTitle)));
    clipping.chapterTitle[safeTitleLength] = '\0';
    if (version == LEGACY_VERSION) {
      uint32_t textLen = 0;
      if (!serialization::tryReadPod(f, textLen)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      clipping.textLength = static_cast<uint16_t>(std::min<uint32_t>(textLen, CLIPPING_TEXT_MAX));
      if (textLen > 0 && !f.seekCur(textLen)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    } else {
      if (!serialization::tryReadPod(f, clipping.textLength)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      if (clipping.textLength > CLIPPING_TEXT_MAX) {
        f.close();
        LOG_ERR("CLIP", "Clipping text length %u exceeds max, record %u: %s", clipping.textLength, i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      if (clipping.textLength > 0 && !f.seekCur(clipping.textLength)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    }
    if (onlyIndex == SIZE_MAX || onlyIndex == i) out.push_back(std::move(clipping));
    if (onlyIndex == i) break;
  }

  f.close();
  return true;
}

bool ClippingStore::readForUpload(const std::string& filePath, const size_t ordinal, Clipping& clipping,
                                  std::string& text, bool& done) {
  done = false;
  ClippingStore reader;
  reader.storeFilePath = storeFilePathForBook(filePath, "epub");
  if (!Storage.exists(reader.storeFilePath.c_str())) {
    reader.storeFilePath += ".bak";
    if (!Storage.exists(reader.storeFilePath.c_str())) {
      done = true;
      return true;
    }
  }
  if (!reader.readFromFile(reader.storeFilePath, reader.clippings, ordinal)) return false;
  if (reader.clippings.empty()) {
    done = true;
    return true;
  }
  clipping = reader.clippings.front();
  return reader.readClippingText(clipping, text);
}

bool ClippingStore::writeToFile(const std::string* replacementText, const size_t replacementIndex,
                                const std::string* sourcePathOverride) {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);

  const std::string tmpPath = storeFilePath + ".tmp";
  const std::string backupPath = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover clipping backup: %s", backupPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Recovered clipping backup: %s", storeFilePath.c_str());
  }
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  if (Storage.exists(backupPath.c_str()) && Storage.exists(storeFilePath.c_str())) Storage.remove(backupPath.c_str());

  FsFile source;
  const std::string& sourcePath = sourcePathOverride ? *sourcePathOverride : storeFilePath;
  const bool hasSource = Storage.exists(sourcePath.c_str());
  if (hasSource && !Storage.openFileForRead("CLIP", sourcePath, source)) {
    LOG_ERR("CLIP", "Failed to open clipping source for rewrite: %s", sourcePath.c_str());
    return false;
  }

  FsFile f = Storage.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    if (source) source.close();
    LOG_ERR("CLIP", "Failed to open clipping temp file for write: %s", tmpPath.c_str());
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(clippings.size(), CLIPPING_MAX_PER_BOOK));
  std::vector<uint32_t> newTextOffsets;
  newTextOffsets.reserve(count);
  std::vector<uint16_t> newTextLengths;
  newTextLengths.reserve(count);
  if (!serialization::tryWritePod(f, VERSION) || !serialization::tryWritePod(f, count) ||
      !serialization::tryWriteString(f, bookTitle) || !serialization::tryWriteString(f, bookAuthor) ||
      !serialization::tryWriteString(f, bookFilePath)) {
    LOG_ERR("CLIP", "Failed to write clipping header: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = clippings[i];
    if (!serialization::tryWritePod(f, clipping.spineIndex) || !serialization::tryWritePod(f, clipping.startPage) ||
        !serialization::tryWritePod(f, clipping.endPage) || !serialization::tryWritePod(f, clipping.pageCount) ||
        !serialization::tryWritePod(f, clipping.startWordIndex) ||
        !serialization::tryWritePod(f, clipping.endWordIndex) || !serialization::tryWritePod(f, clipping.wordCount) ||
        !serialization::tryWritePod(f, clipping.paragraphIndex) || !serialization::tryWritePod(f, clipping.timestamp) ||
        !serialization::tryWritePod(f, clipping.layoutSignature) ||
        !serialization::tryWritePod(f, clipping.tableSelection) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, storeFilePath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const bool useReplacement = replacementText && i == replacementIndex;
    const uint16_t textLen = clipping.textLength;
    if (!serialization::tryWritePod(f, textLen)) {
      LOG_ERR("CLIP", "Failed to write clipping text length %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const uint32_t newTextOffset = static_cast<uint32_t>(f.position());
    bool wroteText = true;
    if (textLen > 0 && useReplacement) {
      wroteText = f.write(reinterpret_cast<const uint8_t*>(replacementText->data()), textLen) == textLen;
    } else if (textLen > 0) {
      wroteText = source && source.seek(clipping.textOffset) && copyBytes(source, f, textLen);
    }
    if (!wroteText) {
      LOG_ERR("CLIP", "Failed to write clipping text %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    newTextOffsets.push_back(newTextOffset);
    newTextLengths.push_back(textLen);
  }

  if (!f.sync()) {
    LOG_ERR("CLIP", "Failed to sync clipping file: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.close();
  if (source) source.close();

  const bool replacingDestination = Storage.exists(storeFilePath.c_str());
  if (replacingDestination && !Storage.rename(storeFilePath.c_str(), backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to back up clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to replace clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    if (replacingDestination) Storage.rename(backupPath.c_str(), storeFilePath.c_str());
    return false;
  }
  if (replacingDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  for (uint16_t i = 0; i < count; ++i) {
    clippings[i].textOffset = newTextOffsets[i];
    clippings[i].textLength = newTextLengths[i];
  }
  return true;
}

bool ClippingStore::hasAnyClippings() {
  if (!Storage.exists(CLIPPINGS_DIR)) return false;
  return !Storage.listFiles(CLIPPINGS_DIR, 1).empty();
}

bool ClippingStore::getAllClippedBooks(std::vector<ClippedBookEntry>& out) {
  if (!Storage.exists(CLIPPINGS_DIR)) return true;

  const auto files = Storage.listFiles(CLIPPINGS_DIR);
  for (const auto& name : files) {
    ClippingFileHeader header;
    const std::string fullPath = std::string(CLIPPINGS_DIR) + "/" + name.c_str();
    if (!readClippingFileHeader(fullPath, name.c_str(), header)) continue;
    if (header.path.empty() || header.count == 0 || !Storage.exists(header.path.c_str())) continue;

    auto existing = std::find_if(out.begin(), out.end(), [&](const ClippedBookEntry& entry) {
      return entry.bookPath == header.path && entry.bookType == header.bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, header.count);
      continue;
    }
    out.push_back({std::move(header.title), std::move(header.author), std::move(header.path),
                   std::move(header.bookType), header.count});
  }
  return true;
}

void ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
  const std::string deletions = deletionLogPath(path);
  if (Storage.exists(deletions.c_str())) Storage.remove(deletions.c_str());
}

bool ClippingStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  const std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  if (!Storage.exists(oldStorePath.c_str())) {
    return true;
  }

  ClippingStore reader;
  std::vector<Clipping> migratedClippings;
  if (!reader.readFromFile(oldStorePath, migratedClippings)) {
    return false;
  }

  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = oldStorePath;
  writer.clippings = std::move(migratedClippings);
  if (!writer.writeToFile()) {
    return false;
  }

  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);
  if (oldStorePath == newStorePath) {
    return true;
  }

  const std::string backupPath = newStorePath + ".bak";
  const bool hasDestination = Storage.exists(newStorePath.c_str());
  if (hasDestination) {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to remove stale clipping migration backup: %s", backupPath.c_str());
      return false;
    }
    if (!Storage.rename(newStorePath.c_str(), backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to back up destination clippings: %s", newStorePath.c_str());
      return false;
    }
  }
  if (!Storage.rename(oldStorePath.c_str(), newStorePath.c_str())) {
    LOG_ERR("CLIP", "Failed to rename migrated clippings: %s -> %s", oldStorePath.c_str(), newStorePath.c_str());
    if (hasDestination && !Storage.rename(backupPath.c_str(), newStorePath.c_str())) {
      LOG_ERR("CLIP", "Failed to restore destination clipping backup: %s", backupPath.c_str());
    }
    return false;
  }
  if (hasDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  movePendingDeletions(oldStorePath, newStorePath);
  return true;
}

bool ClippingStore::hasStoredStateForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  constexpr std::array<const char*, 4> suffixes = {"", ".bak", ".tmp", ".rename.bak"};
  return std::any_of(suffixes.begin(), suffixes.end(),
                     [&path](const char* suffix) { return Storage.exists((path + suffix).c_str()); });
}

bool ClippingStore::beginRenameMigration(const std::string& oldFilePath, const std::string& newFilePath,
                                         const std::string& title, const std::string& author,
                                         const std::string& bookType, RenameMigration& migration) {
  migration = {};
  if (bookType != "epub") {
    LOG_ERR("CLIP", "Unknown clipping book type for rename migration: %s", bookType.c_str());
    return false;
  }
  if (oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) return true;

  migration.sourcePath = storeFilePathForBook(oldFilePath, bookType);
  migration.destinationPath = storeFilePathForBook(newFilePath, bookType);
  migration.destinationBackupPath = migration.destinationPath + ".rename.bak";
  if (!Storage.exists(migration.destinationPath.c_str()) && Storage.exists(migration.destinationBackupPath.c_str())) {
    if (!Storage.rename(migration.destinationBackupPath.c_str(), migration.destinationPath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover interrupted clipping rename backup: %s",
              migration.destinationBackupPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Recovered interrupted clipping rename backup: %s", migration.destinationPath.c_str());
  }
  if (!Storage.exists(migration.sourcePath.c_str())) return true;
  if (migration.sourcePath == migration.destinationPath) {
    LOG_ERR("CLIP", "Clipping storage hash collision during rename migration");
    return false;
  }

  ClippingStore reader;
  std::vector<Clipping> migratedClippings;
  if (!reader.readFromFile(migration.sourcePath, migratedClippings)) {
    LOG_ERR("CLIP", "Failed to load source clippings for rename: %s", migration.sourcePath.c_str());
    return false;
  }

  const std::string ordinaryBackupPath = migration.destinationPath + ".bak";
  if (!Storage.exists(migration.destinationPath.c_str()) && Storage.exists(ordinaryBackupPath.c_str()) &&
      !Storage.rename(ordinaryBackupPath.c_str(), migration.destinationPath.c_str())) {
    LOG_ERR("CLIP", "Failed to recover destination clippings before rename: %s", ordinaryBackupPath.c_str());
    return false;
  }
  if (Storage.exists(migration.destinationPath.c_str()) && Storage.exists(ordinaryBackupPath.c_str()) &&
      !Storage.remove(ordinaryBackupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove stale destination clipping backup: %s", ordinaryBackupPath.c_str());
    return false;
  }
  if (Storage.exists(migration.destinationBackupPath.c_str()) &&
      !Storage.remove(migration.destinationBackupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove stale clipping rename backup: %s", migration.destinationBackupPath.c_str());
    return false;
  }
  if (Storage.exists(migration.destinationPath.c_str())) {
    if (!Storage.rename(migration.destinationPath.c_str(), migration.destinationBackupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to preserve destination clippings: %s", migration.destinationPath.c_str());
      return false;
    }
    migration.destinationBackedUp = true;
  }

  migration.active = true;
  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = migration.destinationPath;
  writer.clippings = std::move(migratedClippings);
  if (!writer.writeToFile(nullptr, SIZE_MAX, &migration.sourcePath)) {
    LOG_ERR("CLIP", "Failed to write transactional clipping rename: %s", migration.destinationPath.c_str());
    rollbackRenameMigration(migration);
    return false;
  }
  return true;
}

bool ClippingStore::commitRenameMigration(RenameMigration& migration) {
  if (!migration.active) return true;
  bool ok = true;
  if (Storage.exists(migration.sourcePath.c_str()) && !Storage.remove(migration.sourcePath.c_str())) {
    LOG_ERR("CLIP", "Failed to delete renamed clipping source: %s", migration.sourcePath.c_str());
    ok = false;
  }
  if (migration.destinationBackedUp && Storage.exists(migration.destinationBackupPath.c_str()) &&
      !Storage.remove(migration.destinationBackupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to delete clipping rename backup: %s", migration.destinationBackupPath.c_str());
    ok = false;
  }
  movePendingDeletions(migration.sourcePath, migration.destinationPath);
  migration.active = false;
  return ok;
}

bool ClippingStore::rollbackRenameMigration(RenameMigration& migration) {
  if (!migration.active) return true;
  bool ok = true;
  if (Storage.exists(migration.destinationPath.c_str()) && !Storage.remove(migration.destinationPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove rolled-back clipping destination: %s", migration.destinationPath.c_str());
    ok = false;
  }
  if (migration.destinationBackedUp &&
      !Storage.rename(migration.destinationBackupPath.c_str(), migration.destinationPath.c_str())) {
    LOG_ERR("CLIP", "Failed to restore clipping rename backup: %s", migration.destinationBackupPath.c_str());
    ok = false;
  }
  if (ok) migration.active = false;
  return ok;
}
