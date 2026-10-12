#include "ClippingsUpload.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ClippingStore.h"
#include "clippings/ClippingIdentity.h"
#include "clippings/ClippingSyncPlan.h"

namespace {
using Result = StatsUploadClient::Result;
using RemoteId = std::array<char, clippingSyncPlan::REMOTE_ID_MAX>;

// One item per download page. A 4 KiB quote can grow several-fold when JSON
// escapes it, so try the size upstream CrossPoint uses and fall back to a
// smaller buffer when the heap is fragmented; only very long, heavily escaped
// quotes need the larger one. Allocated once per sync and reused for every
// request and reply.
constexpr size_t DOWNLOAD_BUFFER = 32 * 1024;
constexpr size_t DOWNLOAD_BUFFER_FALLBACK = 12 * 1024;
// Defensive bound on pages per sync.
constexpr unsigned MAX_PAGES = 4096;

bool validDocument(const std::string& document) {
  return document.size() == 32 && document.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

bool validRemoteId(const JsonString& id) {
  if (id.isNull() || id.size() == 0 || id.size() >= clippingSyncPlan::REMOTE_ID_MAX) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (id.c_str()[i] < '!' || id.c_str()[i] > '~') return false;
  }
  return true;
}

uint16_t clampU16(JsonVariantConst value, const uint16_t fallback) {
  if (!value.is<uint32_t>()) return fallback;
  return static_cast<uint16_t>(std::min<uint32_t>(value.as<uint32_t>(), UINT16_MAX));
}

// Builds the clipping record a remote item describes. Position fields are
// hints: the reader re-finds the quote by its text when the layout differs.
bool decodeLive(JsonObjectConst item, Clipping& clipping, const char*& text, size_t& textLength) {
  const JsonString quote = item["text"].as<JsonString>();
  if (quote.isNull() || quote.size() == 0 || quote.size() > CLIPPING_TEXT_MAX ||
      strlen(quote.c_str()) != quote.size() || !item["spine"].is<uint16_t>()) {
    return false;
  }
  clipping.spineIndex = item["spine"].as<uint16_t>();
  clipping.startPage = clampU16(item["start_page"], 0);
  clipping.endPage = std::max(clipping.startPage, clampU16(item["end_page"], clipping.startPage));
  clipping.pageCount = std::max<uint16_t>(1, clampU16(item["pages"], 1));
  clipping.startWordIndex = clampU16(item["start_word"], 0);
  clipping.endWordIndex = clampU16(item["end_word"], 0);
  clipping.wordCount = clampU16(item["words"], 0);
  clipping.paragraphIndex = item["para"].is<uint16_t>() ? item["para"].as<uint16_t>() : UINT16_MAX;
  clipping.timestamp = item["created_at"].is<uint32_t>() ? item["created_at"].as<uint32_t>() : 0;
  clipping.layoutSignature = item["layout_signature"].is<uint32_t>() ? item["layout_signature"].as<uint32_t>() : 0;
  clipping.tableSelection = UINT16_MAX;
  snprintf(clipping.chapterTitle, sizeof(clipping.chapterTitle), "%s", item["chapter"] | "");
  text = quote.c_str();
  textLength = quote.size();
  return true;
}

// Serializes one request body into the shared buffer.
bool serialize(JsonDocument& json, char* buffer, const size_t capacity) {
  if (json.overflowed() || measureJson(json) >= capacity) {
    LOG_ERR("ClippingsSync", "Clipping request exceeds buffer");
    return false;
  }
  serializeJson(json, buffer, capacity);
  json.clear();
  return true;
}
}  // namespace

bool ClippingsUpload::identity(const uint32_t timestamp, const std::string& text, char (&id)[17]) {
  return clippingIdentity::compute(timestamp, text, id);
}

StatsUploadClient::Result ClippingsUpload::upload(const std::string& path, const std::string& document) {
  return sync(path, document);
}

StatsUploadClient::Result ClippingsUpload::sync(const std::string& path, const std::string& document) {
  if (!validDocument(document)) return Result::InvalidResponse;
  const std::string endpoint = "/api/v1/clippings/" + document;

  // The reader is not running during sync (the sync screen replaces it and it
  // reloads its clippings afterwards), so a private store owns the file here.
  ClippingStore store;
  if (!store.loadForBook(path, "", "", "epub")) {
    LOG_ERR("ClippingsSync", "Cannot load clippings: %s", path.c_str());
    return Result::InvalidResponse;
  }
  struct Unload {
    ClippingStore& store;
    ~Unload() { store.unload(); }
  } unload{store};

  clippingSyncPlan::Plan plan;
  plan.local.reserve(store.clippingCount() + 8);
  for (size_t i = 0; i < store.clippingCount(); ++i) {
    clippingSyncPlan::LocalEntry entry;
    if (!store.clippingSyncKeys(i, entry.id, entry.key)) {
      LOG_ERR("ClippingsSync", "Cannot read clipping %u", static_cast<unsigned>(i));
      return Result::InvalidResponse;
    }
    plan.local.push_back(entry);
  }
  std::vector<ClippingStore::PendingDeletion> pending;
  if (!ClippingStore::readPendingDeletions(path, pending)) return Result::InvalidResponse;
  plan.deleted.reserve(pending.size());
  for (const auto& p : pending) {
    clippingSyncPlan::DeletedEntry d;
    std::memcpy(d.id, p.id, sizeof(d.id));
    d.key = p.key;
    plan.deleted.push_back(d);
  }

  size_t capacity = DOWNLOAD_BUFFER;
  auto buffer = makeUniqueNoThrow<char[]>(capacity);
  if (!buffer) {
    capacity = DOWNLOAD_BUFFER_FALLBACK;
    buffer = makeUniqueNoThrow<char[]>(capacity);
  }
  if (!buffer) {
    LOG_ERR("ClippingsSync", "Cannot allocate clipping sync buffer");
    return Result::LowMemory;
  }
  StatsUploadClient client;
  std::vector<RemoteId> deleteRemote;
  unsigned added = 0, removed = 0, sentDeletes = 0, uploaded = 0, seen = 0;

  // 1. Download every server item for this book, oldest revision first, and
  //    apply it here. Tombstones remove; live items are added unless this
  //    device already has them or deleted them.
  uint64_t cursor = 0;
  bool more = true;
  for (unsigned page = 0; more; ++page) {
    if (page >= MAX_PAGES) {
      LOG_ERR("ClippingsSync", "Too many clipping pages");
      return Result::InvalidResponse;
    }
    char query[64];
    snprintf(query, sizeof(query), "?limit=1&format=reader&cursor=%llu", static_cast<unsigned long long>(cursor));
    size_t length = 0;
    const Result got = client.get((endpoint + query).c_str(), buffer.get(), capacity, length);
    if (got != Result::Ok) return got;
    JsonDocument doc;
    // Mutable input lets ArduinoJson reference the reply without copying text.
    const auto error = deserializeJson(doc, buffer.get(), length, DeserializationOption::NestingLimit(4));
    if (error) {
      LOG_ERR("ClippingsSync", "Invalid clipping reply: %s", error.c_str());
      return Result::InvalidResponse;
    }
    if (doc["sync_version"].as<unsigned>() != 2 || !doc["cursor"].is<uint64_t>() || !doc["more"].is<bool>() ||
        !doc["items"].is<JsonArrayConst>()) {
      LOG_ERR("ClippingsSync", "Server does not support clipping download");
      return Result::Unsupported;
    }
    const uint64_t next = doc["cursor"].as<uint64_t>();
    more = doc["more"].as<bool>();
    const JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    if (items.size() > 1 || next < cursor || (items.size() > 0 && next <= cursor) || (more && items.size() == 0)) {
      LOG_ERR("ClippingsSync", "Invalid clipping pagination");
      return Result::InvalidResponse;
    }
    for (const JsonObjectConst item : items) {
      const JsonString id = item["id"].as<JsonString>();
      if (!validRemoteId(id)) {
        LOG_ERR("ClippingsSync", "Invalid remote clipping id");
        return Result::InvalidResponse;
      }
      ++seen;
      const bool isDeleted = item["deleted"].as<unsigned>() == 1 || item["deleted"].as<bool>();
      Clipping record;
      const char* text = nullptr;
      size_t textLength = 0;
      uint32_t key = 0;
      if (!isDeleted) {
        if (!decodeLive(item, record, text, textLength)) {
          LOG_ERR("ClippingsSync", "Skipping malformed remote clipping %s", id.c_str());
          continue;
        }
        key = clippingSyncPlan::textKey(record.spineIndex, text, textLength);
      }
      const auto decision = plan.onRemote(id.c_str(), isDeleted, key);
      switch (decision.action) {
        case clippingSyncPlan::Action::None:
          if (!isDeleted) plan.markOnServer(decision.index);
          break;
        case clippingSyncPlan::Action::RemoveLocal:
          if (!store.removeClippingAt(decision.index, false)) return Result::InvalidResponse;
          plan.removed(decision.index);
          ++removed;
          break;
        case clippingSyncPlan::Action::DeleteRemote: {
          RemoteId remote{};
          std::memcpy(remote.data(), id.c_str(), id.size());
          deleteRemote.push_back(remote);
          break;
        }
        case clippingSyncPlan::Action::AddLocal: {
          const std::string quote(text, textLength);
          const auto addResult = store.addSyncedClipping(record, quote);
          if (addResult == ClippingStore::AddResult::LimitReached) {
            LOG_ERR("ClippingsSync", "Clipping limit reached; not adding %s", id.c_str());
            break;
          }
          if (addResult != ClippingStore::AddResult::Added) return Result::InvalidResponse;
          // The stored record decides the local identity (its text may be capped).
          char localId[17];
          uint32_t localKey = key;
          if (!store.clippingSyncKeys(store.clippingCount() - 1, localId, localKey)) return Result::InvalidResponse;
          plan.added(localId, localKey);
          ++added;
          break;
        }
      }
    }
    cursor = next;
  }

  // 2. Send deletions made on this device: the logged ids, plus any server ids
  //    that matched a deleted clipping by its text.
  for (const auto& p : pending) {
    RemoteId remote{};
    std::memcpy(remote.data(), p.id, sizeof(p.id));
    deleteRemote.push_back(remote);
  }
  std::sort(deleteRemote.begin(), deleteRemote.end());
  deleteRemote.erase(std::unique(deleteRemote.begin(), deleteRemote.end()), deleteRemote.end());
  for (const auto& remote : deleteRemote) {
    JsonDocument json;
    auto item = json["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = remote.data();
    item["deleted"] = 1;
    if (!serialize(json, buffer.get(), capacity)) return Result::InvalidResponse;
    const Result put = client.put(endpoint.c_str(), buffer.get(), true);
    if (put != Result::Ok) return put;
    ++sentDeletes;
  }
  if (!pending.empty() && !ClippingStore::clearPendingDeletions(path)) {
    // Harmless if it stays: resending a tombstone is idempotent.
    LOG_ERR("ClippingsSync", "Cannot clear clipping deletion log");
  }

  // 3. Upload clippings the server does not have yet.
  std::string text;
  text.reserve(CLIPPING_TEXT_MAX);
  for (size_t i = 0; i < store.clippingCount() && i < plan.local.size(); ++i) {
    if (plan.local[i].onServer) continue;
    const Clipping* clipping = store.clippingAt(i);
    if (!clipping || !store.readClippingText(i, text) || text.empty()) return Result::InvalidResponse;
    JsonDocument json;
    auto item = json["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = static_cast<const char*>(plan.local[i].id);
    item["spine"] = clipping->spineIndex;
    item["start_page"] = clipping->startPage;
    item["end_page"] = clipping->endPage;
    item["pages"] = clipping->pageCount;
    item["start_word"] = clipping->startWordIndex;
    item["end_word"] = clipping->endWordIndex;
    item["words"] = clipping->wordCount;
    if (clipping->paragraphIndex != UINT16_MAX) item["para"] = clipping->paragraphIndex;
    item["chapter"] = static_cast<const char*>(clipping->chapterTitle);
    item["text"] = text.c_str();
    item["created_at"] = clippingUnixTimestamp(clipping->timestamp);
    item["layout_signature"] = clipping->layoutSignature;
    // Omit note/color: preserve annotations added on the server.
    if (!serialize(json, buffer.get(), capacity)) return Result::InvalidResponse;
    const Result put = client.put(endpoint.c_str(), buffer.get(), true);
    if (put != Result::Ok) return put;
    plan.markOnServer(i);
    ++uploaded;
  }

  LOG_INF("ClippingsSync", "Clippings: %u added, %u removed, %u deletions sent, %u uploaded (%u remote)", added,
          removed, sentDeletes, uploaded, seen);
  if (seen == 0 && store.clippingCount() == 0 && sentDeletes == 0) return Result::Skipped;
  return Result::Ok;
}
