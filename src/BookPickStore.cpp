#include "BookPickStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string>

BookPickStore& BookPickStore::getInstance() {
  static BookPickStore instance;
  return instance;
}

void BookPickStore::ensureLoaded() {
  if (loaded) return;
  loaded = true;
  model.clear();
  const char* path = getFilePath();
  if (!Storage.exists(path)) return;

  FsFile file;
  if (!Storage.openFileForRead("PICK", path, file)) {
    LOG_ERR("PICK", "Cannot open %s", path);
    return;
  }
  // Stack use is fixed and small (library::PickTaste::SERIALIZED_SIZE, ~700 bytes).
  uint8_t buffer[library::PickTaste::SERIALIZED_SIZE];
  const int read = file.read(buffer, sizeof(buffer));
  // A file longer than the model is a different format: reject it rather than truncate.
  const bool exactSize = read == static_cast<int>(sizeof(buffer)) && file.available() == 0;
  file.close();
  if (!exactSize || !model.deserialize(buffer, sizeof(buffer))) {
    LOG_ERR("PICK", "Ignoring unreadable taste file (%d bytes)", read);
    model.clear();
  }
}

bool BookPickStore::writeModel() const {
  uint8_t buffer[library::PickTaste::SERIALIZED_SIZE];
  const size_t length = model.serialize(buffer, sizeof(buffer));
  if (length != sizeof(buffer)) {
    LOG_ERR("PICK", "Cannot serialize taste model");
    return false;
  }

  const std::string path = getFilePath();
  const std::string tmpPath = path + ".tmp";
  if (!Storage.ensureDirectoryExists("/.crosspoint")) {
    LOG_ERR("PICK", "Cannot create /.crosspoint");
    return false;
  }
  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR("PICK", "Cannot remove stale %s", tmpPath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite("PICK", tmpPath, file)) {
    LOG_ERR("PICK", "Cannot write %s", tmpPath.c_str());
    return false;
  }
  const size_t written = file.write(buffer, length);
  file.flush();
  const bool synced = written == length && file.sync();
  const bool closed = file.close();
  if (!synced || !closed) {
    LOG_ERR("PICK", "Incomplete write of %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  // Rename onto an existing file is not guaranteed on FAT, so clear it first. A
  // failure between the two steps only costs the learned preferences.
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    LOG_ERR("PICK", "Cannot replace %s", path.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("PICK", "Cannot publish %s", path.c_str());
    return false;
  }
  return true;
}

bool BookPickStore::saveIfDirty() {
  if (!dirty) return true;
  if (!writeModel()) return false;
  dirty = false;
  return true;
}

bool BookPickStore::reset() {
  loaded = true;
  model.clear();
  dirty = true;
  return saveIfDirty();
}
