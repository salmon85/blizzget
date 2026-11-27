#include "downloader.h"
#include "version_sort.h"
#include "base/error.h"
#include "base/http.h"
#include "ngdp.h"

#include <json-glib/json-glib.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using Hash = NGDP::Hash;

namespace {

constexpr uint64_t BlockSize = 1ULL << 20;

std::string readAll(File& file) {
  if (!file) return {};
  file.seek(0);
  std::string result;
  char buffer[65536];
  while (size_t count = file.read(buffer, sizeof buffer)) result.append(buffer, count);
  file.seek(0);
  return result;
}

File fetchFile(std::string const& url) {
  HttpRequest request(url);
  request.setTimeout(90);
  if (!request.send() || (request.status() != 200 && request.status() != 206)) {
    throw Exception("HTTP %u for %s", request.status(), url.c_str());
  }
  return request.response();
}

std::string fetchText(std::string const& url) {
  File file = fetchFile(url);
  return readAll(file);
}

JsonNode* member(JsonNode* object, char const* name) {
  if (!object || !JSON_NODE_HOLDS_OBJECT(object)) return nullptr;
  return json_object_get_member(json_node_get_object(object), name);
}

std::string jsonString(JsonNode* object, char const* name) {
  JsonNode* node = member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node)) return {};
  const char* value = json_node_get_string(node);
  return value ? value : "";
}

gint64 jsonInteger(JsonNode* object, char const* name) {
  JsonNode* node = member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node)) return 0;
  return json_node_get_int(node);
}

JsonParser* parseJson(std::string const& text) {
  GError* error = nullptr;
  JsonParser* parser = json_parser_new();
  if (!json_parser_load_from_data(parser, text.c_str(), static_cast<gssize>(text.size()), &error)) {
    std::string message = error ? error->message : "invalid JSON";
    if (error) g_error_free(error);
    g_object_unref(parser);
    throw Exception("JSON parse failed: %s", message.c_str());
  }
  return parser;
}

std::vector<VersionRecord> parseVersionResult(JsonNode* result, std::string const& region) {
  std::vector<VersionRecord> records;
  JsonNode* data = member(result, "data");
  if (!data || !JSON_NODE_HOLDS_ARRAY(data)) return records;
  JsonArray* array = json_node_get_array(data);
  std::unordered_set<std::string> seen;
  for (guint i = 0; i < json_array_get_length(array); ++i) {
    JsonNode* entry = json_array_get_element(array, i);
    if (jsonString(entry, "region") != region) continue;
    VersionRecord record;
    record.version = jsonString(entry, "version_name");
    if (record.version.empty()) record.version = jsonString(entry, "build_id");
    record.build_config = jsonString(entry, "build_config");
    record.cdn_config = jsonString(entry, "cdn_config");
    record.product_config = jsonString(entry, "product_config");
    record.created_at = jsonString(entry, "created_at");
    if (record.build_config.empty() || !seen.insert(record.build_config).second) continue;
    records.push_back(std::move(record));
  }
  return records;
}

std::vector<VersionRecord> parseCachedVersions(std::string const& text) {
  std::vector<VersionRecord> records;
  JsonParser* parser = parseJson(text);
  JsonNode* root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
    g_object_unref(parser);
    return records;
  }
  JsonArray* array = json_node_get_array(root);
  for (guint i = 0; i < json_array_get_length(array); ++i) {
    JsonNode* entry = json_array_get_element(array, i);
    VersionRecord record;
    record.version = jsonString(entry, "version");
    record.build_config = jsonString(entry, "build_config");
    record.cdn_config = jsonString(entry, "cdn_config");
    record.product_config = jsonString(entry, "product_config");
    record.created_at = jsonString(entry, "created_at");
    if (!record.build_config.empty()) records.push_back(std::move(record));
  }
  g_object_unref(parser);
  return records;
}

std::string cdnPathForProduct(std::string const& product) {
  static std::map<std::string, std::string> paths = {
      {"d3", "diablo3"}, {"fenris", "diablo4"}, {"osi", "diablo2"},
      {"wow", "wow"}, {"wow_classic", "wow"}, {"wow_classic_era", "wow"},
      {"wow_classic_titan", "wow"}, {"wow_anniversary", "wow"},
      {"wowt", "wow"}, {"wowxptr", "wow"}, {"pro", "overwatch"},
      {"hsb", "hearthstone"}, {"hero", "heroes"}, {"s2", "sc2"},
      {"s1", "sc1"}, {"w3", "warcraft3"}, {"wlby", "cod"},
      {"agent", "battle.net"}, {"bna", "battle.net"}, {"bts", "battle.net"},
  };
  auto it = paths.find(product);
  return it == paths.end() ? product : it->second;
}

std::string configUrl(std::string const& base, std::string const& type, std::string const& hash) {
  return base + type + "/" + hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash;
}

std::string dataUrl(std::string const& base, std::string const& hash, bool index = false) {
  return configUrl(base, "data", hash) + (index ? ".index" : "");
}

NGDP::ConfigFile parseConfigText(std::string const& text) {
  NGDP::ConfigFile result;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#') continue;
    size_t pos = line.find(" = ");
    if (pos != std::string::npos) result[line.substr(0, pos)] = line.substr(pos + 3);
  }
  return result;
}

std::string firstToken(std::string const& value) {
  size_t pos = value.find(' ');
  return value.substr(0, pos);
}

std::string secondToken(std::string const& value) {
  size_t pos = value.find(' ');
  return pos == std::string::npos ? value : value.substr(pos + 1);
}

struct Tag {
  std::string name;
  uint16_t type = 0;
  std::vector<uint8_t> mask;
};

std::vector<Tag> parseTags(File& file, uint32_t count, uint32_t entries) {
  std::vector<Tag> tags;
  uint32_t maskSize = (entries + 7) / 8;
  for (uint32_t i = 0; i < count; ++i) {
    Tag tag;
    int c;
    while ((c = file.getc()) != EOF && c != 0) tag.name.push_back(static_cast<char>(c));
    tag.type = file.read16(true);
    tag.mask.resize(maskSize);
    if (maskSize) file.read(tag.mask.data(), maskSize);
    tags.push_back(std::move(tag));
  }
  return tags;
}

std::vector<uint8_t> selectionMask(std::vector<Tag> const& tags, uint32_t entries,
                                   std::set<std::string> const& wanted) {
  std::vector<uint8_t> mask((entries + 7) / 8, 0);
  for (auto const& tag : tags) {
    if (!wanted.count(tag.name)) continue;
    for (size_t i = 0; i < mask.size() && i < tag.mask.size(); ++i) mask[i] |= tag.mask[i];
  }
  if (std::none_of(mask.begin(), mask.end(), [](uint8_t value) { return value != 0; })) {
    std::fill(mask.begin(), mask.end(), 0xFF);
  }
  return mask;
}

bool selected(std::vector<uint8_t> const& mask, uint32_t index) {
  return (mask[index / 8] & (1U << (7 - (index & 7)))) != 0;
}

struct DownloadEntry {
  Hash hash{};
  uint32_t size = 0;
};

struct InstallEntry {
  std::string name;
  Hash hash{};
  uint32_t size = 0;
};

struct Manifest {
  std::vector<DownloadEntry> downloads;
  std::vector<InstallEntry> installs;
  std::vector<Tag> tags;
};

Manifest parseDownloadManifest(File& file) {
  if (file.read16(true) != 0x444C) throw Exception("invalid download manifest");
  uint8_t version = file.read8();
  uint8_t hashSize = file.read8();
  uint8_t checksums = file.read8();
  uint32_t entries = file.read32(true);
  uint16_t tagCount = file.read16(true);
  if (hashSize != 16) throw Exception("unsupported download hash size");
  uint32_t extra = 6 + (checksums ? 4 : 0);
  if (version >= 2) {
    uint8_t flagBytes = file.read8();
    extra += flagBytes;
    if (version >= 3) file.seek(4, SEEK_CUR);
  }
  Manifest manifest;
  manifest.downloads.reserve(entries);
  for (uint32_t i = 0; i < entries; ++i) {
    DownloadEntry entry;
    file.read(entry.hash, sizeof entry.hash);
    file.seek(extra, SEEK_CUR);
    manifest.downloads.push_back(entry);
  }
  manifest.tags = parseTags(file, tagCount, entries);
  return manifest;
}

Manifest parseInstallManifest(File& file) {
  if (file.read16(true) != 0x494E) throw Exception("invalid install manifest");
  file.seek(2, SEEK_CUR);
  uint16_t tagCount = file.read16(true);
  uint32_t entries = file.read32(true);
  Manifest manifest;
  manifest.tags = parseTags(file, tagCount, entries);
  manifest.installs.reserve(entries);
  for (uint32_t i = 0; i < entries; ++i) {
    InstallEntry entry;
    int c;
    while ((c = file.getc()) != EOF && c != 0) entry.name.push_back(static_cast<char>(c));
    file.read(entry.hash, sizeof entry.hash);
    entry.size = file.read32(true);
    manifest.installs.push_back(std::move(entry));
  }
  return manifest;
}

uint32_t bigEndian32(uint8_t const* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

struct Archive {
  std::string hash;
  std::string cacheFile;
  std::string maskFile;
  std::string indexData;
  std::vector<uint8_t> mask;
  std::vector<bool> needed;
};

struct Location {
  size_t archive = 0;
  uint32_t size = 0;
  uint32_t offset = 0;
};

struct Chunk {
  size_t archive = 0;
  uint32_t block = 0;
};

std::unordered_map<std::string, Location> loadIndex(std::string const& data) {
  std::unordered_map<std::string, Location> result;
  Hash zero{};
  for (size_t block = 0; block + 4096 <= data.size(); block += 4096) {
    for (size_t pos = 0; pos + 24 <= 4096; pos += 24) {
      uint8_t const* ptr = reinterpret_cast<uint8_t const*>(data.data()) + block + pos;
      if (!memcmp(ptr, zero, sizeof zero)) return result;
      Location location;
      location.size = bigEndian32(ptr + 16);
      location.offset = bigEndian32(ptr + 20);
      char hex[33];
      for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", ptr[i]);
      result.emplace(hex, location);
    }
  }
  return result;
}

std::string hashHex(Hash const& hash) {
  return NGDP::to_string(hash);
}

void writeBytes(std::string const& path, std::string const& data) {
  File file(path, File::REWRITE);
  if (!file) throw Exception("failed to write %s", path.c_str());
  file.write(data.data(), data.size());
  file.flush();
}

std::string cachePath(DownloadOptions const& options, std::string const& hash) {
  return options.cache_dir / "cdn" / cdnPathForProduct(options.product) /
         "data" / hash.substr(0, 2) / hash.substr(2, 4) / hash;
}

File loadCachedBlte(std::string const& ekey, DownloadOptions const& options,
                    std::vector<Archive> const& archives,
                    std::unordered_map<std::string, Location> const& locations) {
  std::string path = cachePath(options, ekey);
  File file(path);
  if (file) return file;
  auto it = locations.find(ekey);
  if (it == locations.end()) return File();
  Archive const& archive = archives[it->second.archive];
  File source(archive.cacheFile);
  if (!source) return File();
  source.seek(it->second.offset);
  MemoryFile result;
  uint8_t* output = result.reserve(it->second.size);
  if (source.read(output, it->second.size) != it->second.size) {
    return File();
  }
  result.seek(0);
  return result;
}

}

std::string defaultCacheDirectory() {
  if (char const* value = std::getenv("BLIZZGET_CACHE_DIR")) return value;
  if (char const* home = std::getenv("HOME")) {
    return (fs::path(home) / ".cache" / "cascette").string();
  }
  return (fs::current_path() / "cache").string();
}

std::string formatBytes(uint64_t bytes) {
  static char const* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
  std::ostringstream out;
  out << std::fixed << std::setprecision(unit ? 2 : 0) << value << ' ' << units[unit];
  return out.str();
}

std::string formatDuration(uint64_t seconds) {
  if (seconds == UINT64_MAX) return "unknown";
  uint64_t hours = seconds / 3600;
  uint64_t minutes = (seconds % 3600) / 60;
  uint64_t secs = seconds % 60;
  std::ostringstream out;
  if (hours) out << hours << "h ";
  if (hours || minutes) out << minutes << "m ";
  out << secs << 's';
  return out.str();
}

void Downloader::log(std::string const& text) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.log_line = text;
    progress_.log_lines.push_back(text);
    ++progress_.log_count;
  }
  emit();
}

void Downloader::setStage(std::string const& text) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.stage = text;
  }
  emit();
}

void Downloader::emit() {
  Callback callback;
  ProgressSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callback = callback_;
    snapshot = progress_;
  }
  if (callback) callback(snapshot);
}

std::vector<VersionRecord> Downloader::fetchVersions(
    std::string const& product, std::string const& region, std::string const& cacheDir,
    std::function<void(std::string const&)> const& status) {
  fs::path cacheFile = fs::path(cacheDir) / fs::path("versions") /
                       fs::path(product) / fs::path(region + ".json");
  std::error_code ec;
  if (fs::exists(cacheFile, ec)) {
    auto age = std::chrono::duration_cast<std::chrono::hours>(
        fs::file_time_type::clock::now() - fs::last_write_time(cacheFile, ec));
    if (!ec && age < std::chrono::hours(6)) {
      std::ifstream input(cacheFile);
      std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      if (!text.empty()) {
        auto cached = parseCachedVersions(text);
        if (!cached.empty()) {
          sortVersionsNewestFirst(cached);
          return cached;
        }
      }
    }
  }

  std::string api = "https://blizztrack.com/api/manifest/" + product;
  status("Fetching current versions...");
  File currentFile = fetchFile(api + "/versions");
  JsonParser* currentParser = parseJson(readAll(currentFile));
  JsonNode* currentResult = member(json_parser_get_root(currentParser), "result");
  std::vector<VersionRecord> merged = parseVersionResult(currentResult, region);

  status("Fetching version history...");
  File firstFile = fetchFile(api + "/seqn?file=versions&page=1&limit=100");
  JsonParser* firstParser = parseJson(readAll(firstFile));
  JsonNode* firstResult = member(json_parser_get_root(firstParser), "result");
  std::vector<int> seqns;
  JsonNode* results = member(firstResult, "results");
  if (results && JSON_NODE_HOLDS_ARRAY(results)) {
    JsonArray* array = json_node_get_array(results);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
      JsonNode* item = json_array_get_element(array, i);
      int seqn = static_cast<int>(jsonInteger(item, "seqn"));
      if (seqn) seqns.push_back(seqn);
    }
  }
  int totalPages = static_cast<int>(jsonInteger(firstResult, "total_pages"));
  for (int page = 2; page <= totalPages && page <= 200; ++page) {
    try {
      File file = fetchFile(api + "/seqn?file=versions&page=" + std::to_string(page) + "&limit=100");
      JsonParser* parser = parseJson(readAll(file));
      JsonNode* pageResults = member(member(json_parser_get_root(parser), "result"), "results");
      if (pageResults && JSON_NODE_HOLDS_ARRAY(pageResults)) {
        JsonArray* array = json_node_get_array(pageResults);
        for (guint i = 0; i < json_array_get_length(array); ++i) {
          int seqn = static_cast<int>(jsonInteger(json_array_get_element(array, i), "seqn"));
          if (seqn) seqns.push_back(seqn);
        }
      }
      g_object_unref(parser);
    } catch (...) { /* history pages are best-effort */ }
  }
  g_object_unref(firstParser);
  g_object_unref(currentParser);

  std::sort(seqns.begin(), seqns.end());
  seqns.erase(std::unique(seqns.begin(), seqns.end()), seqns.end());
  std::atomic<size_t> next{0};
  std::mutex mergeMutex;
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&] {
      while (true) {
        size_t index = next.fetch_add(1);
        if (index >= seqns.size()) break;
        try {
          File file = fetchFile(api + "/versions?seqn=" + std::to_string(seqns[index]));
          JsonParser* parser = parseJson(readAll(file));
          auto records = parseVersionResult(member(json_parser_get_root(parser), "result"), region);
          g_object_unref(parser);
          std::lock_guard<std::mutex> lock(mergeMutex);
          for (auto& record : records) merged.push_back(std::move(record));
        } catch (...) { /* snapshots are best-effort */ }
      }
    });
  }
  for (auto& worker : workers) worker.join();

  sortVersionsNewestFirst(merged);
  std::unordered_set<std::string> seen;
  std::vector<VersionRecord> unique;
  for (auto& record : merged) if (seen.insert(record.build_config).second) unique.push_back(record);
  sortVersionsNewestFirst(unique);

  JsonBuilder* builder = json_builder_new();
  json_builder_begin_array(builder);
  for (auto const& record : unique) {
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version");
    json_builder_add_string_value(builder, record.version.c_str());
    json_builder_set_member_name(builder, "build_config");
    json_builder_add_string_value(builder, record.build_config.c_str());
    json_builder_set_member_name(builder, "cdn_config");
    json_builder_add_string_value(builder, record.cdn_config.c_str());
    json_builder_set_member_name(builder, "product_config");
    json_builder_add_string_value(builder, record.product_config.c_str());
    json_builder_set_member_name(builder, "created_at");
    json_builder_add_string_value(builder, record.created_at.c_str());
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);
  JsonNode* root = json_builder_get_root(builder);
  char* serialized = json_to_string(root, FALSE);
  writeBytes(cacheFile.string(), serialized);
  g_free(serialized);
  json_node_free(root);
  g_object_unref(builder);
  status("Loaded " + std::to_string(unique.size()) + " " + product + " versions (" + region + ")");
  return unique;
}

void Downloader::run(DownloadOptions const& options) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_ = ProgressSnapshot{};
  }
  auto started = std::chrono::steady_clock::now();
  std::atomic<uint64_t> bytesDone{0};
  std::atomic<uint64_t> failedChunks{0};
  std::mutex rateMutex;
  std::chrono::steady_clock::time_point rateNext = std::chrono::steady_clock::now();
  auto throttle = [&](uint64_t bytes) {
    if (!options.speed_cap) return;
    std::lock_guard<std::mutex> lock(rateMutex);
    auto now = std::chrono::steady_clock::now();
    if (rateNext < now) rateNext = now;
    rateNext += std::chrono::nanoseconds(static_cast<int64_t>(bytes * 1e9 / options.speed_cap));
    std::this_thread::sleep_until(rateNext);
  };

  try {
    std::string cdnPath = cdnPathForProduct(options.product);
    std::string base = "https://level3.blizzard.com/tpr/" + cdnPath + "/";
    setStage("Fetching build configuration...");
    std::string cdnText = fetchText(configUrl(base, "config", options.cdn_config));
    std::string buildText = fetchText(configUrl(base, "config", options.build_config));
    auto cdnConfig = parseConfigText(cdnText);
    auto buildConfig = parseConfigText(buildText);

    setStage("Loading encoding table...");
    std::string encodingKey = secondToken(buildConfig["encoding"]);
    uint32_t encodingSize = static_cast<uint32_t>(std::stoul(firstToken(buildConfig["encoding-size"])));
    File encodingRaw = fetchFile(dataUrl(base, encodingKey));
    writeBytes(cachePath(options, encodingKey), readAll(encodingRaw));
    File encodingDecoded = NGDP::DecodeBLTE(encodingRaw, encodingSize);
    if (!encodingDecoded) throw Exception("failed to decode encoding table");
    NGDP::Encoding encoding(encodingDecoded);

    auto loadManifest = [&](std::string const& field, uint32_t size) {
      Hash ckey{};
      NGDP::from_string(ckey, firstToken(buildConfig[field]));
      auto const* enc = encoding.getEncoding(ckey);
      if (!enc) throw Exception("missing encoding entry for %s", field.c_str());
      std::string ekey = hashHex(enc->keys[0]);
      File raw = fetchFile(dataUrl(base, ekey));
      writeBytes(cachePath(options, ekey), readAll(raw));
      File decoded = NGDP::DecodeBLTE(raw, size);
      if (!decoded) throw Exception("failed to decode %s", field.c_str());
      return std::make_pair(std::move(decoded), ekey);
    };

    setStage("Reading download and install manifests...");
    auto downloadPair = loadManifest("download", static_cast<uint32_t>(std::stoul(firstToken(buildConfig["download-size"]))));
    auto installPair = loadManifest("install", static_cast<uint32_t>(std::stoul(firstToken(buildConfig["install-size"]))));
    Manifest downloadManifest = parseDownloadManifest(downloadPair.first);
    Manifest installManifest = parseInstallManifest(installPair.first);
    std::set<std::string> wantedTags = {options.platform, options.arch, options.locale};
    auto downloadMask = selectionMask(downloadManifest.tags,
                                      static_cast<uint32_t>(downloadManifest.downloads.size()), wantedTags);
    auto installMask = selectionMask(installManifest.tags,
                                     static_cast<uint32_t>(installManifest.installs.size()), wantedTags);

    std::vector<std::string> neededEkeys;
    neededEkeys.push_back(encodingKey);
    neededEkeys.push_back(downloadPair.second);
    neededEkeys.push_back(installPair.second);

    size_t selectedDownloadCount = 0;
    for (size_t i = 0; i < downloadManifest.downloads.size(); ++i) {
      if (!selected(downloadMask, static_cast<uint32_t>(i))) continue;
      if (options.max_files && selectedDownloadCount >= options.max_files) break;
      neededEkeys.push_back(hashHex(downloadManifest.downloads[i].hash));
      ++selectedDownloadCount;
    }
    if (options.max_files) {
      size_t selectedInstallCount = 0;
      for (size_t i = 0; i < installManifest.installs.size(); ++i) {
        if (!selected(installMask, static_cast<uint32_t>(i))) continue;
        if (selectedInstallCount >= options.max_files) break;
        auto const* enc = encoding.getEncoding(installManifest.installs[i].hash);
        if (enc) neededEkeys.push_back(hashHex(enc->keys[0]));
        ++selectedInstallCount;
      }
    }

    setStage("Loading CDN archive indices...");
    std::vector<Archive> archives;
    std::unordered_map<std::string, Location> locations;
    std::vector<std::string> archiveHashes = split(cdnConfig["archives"]);
    archives.reserve(archiveHashes.size());
    for (auto const& hash : archiveHashes) {
      Archive archive;
      archive.hash = hash;
      archive.cacheFile = cachePath(options, hash);
      archive.maskFile = archive.cacheFile + ".mask";
      archive.indexData = fetchText(dataUrl(base, hash, true));
      fs::create_directories(fs::path(archive.cacheFile).parent_path());
      auto archiveLocations = loadIndex(archive.indexData);
      for (auto const& item : archiveLocations) {
        Location location = item.second;
        location.archive = archives.size();
        locations[item.first] = location;
      }
      archives.push_back(std::move(archive));
      log("Loaded archive index " + hash.substr(0, 12));
    }

    for (auto const& ekeyHex : neededEkeys) {
      auto it = locations.find(ekeyHex);
      if (it == locations.end()) continue;
      Archive& archive = archives[it->second.archive];
      uint32_t start = it->second.offset / BlockSize;
      uint32_t end = (it->second.offset + it->second.size + BlockSize - 1) / BlockSize;
      if (archive.needed.size() < end) archive.needed.resize(end, false);
      for (uint32_t block = start; block < end; ++block) archive.needed[block] = true;
    }

    for (auto& archive : archives) {
      std::ifstream maskInput(archive.maskFile, std::ios::binary);
      if (maskInput) {
        archive.mask.assign(std::istreambuf_iterator<char>(maskInput), std::istreambuf_iterator<char>());
      }
    }

    std::vector<Chunk> chunks;
    uint64_t totalChunkBytes = 0;
    for (size_t i = 0; i < archives.size(); ++i) {
      for (uint32_t block = 0; block < archives[i].needed.size(); ++block) {
        if (!archives[i].needed[block]) continue;
        if (block / 8 < archives[i].mask.size() && (archives[i].mask[block / 8] & (1U << (block & 7)))) continue;
        chunks.push_back({i, block});
        totalChunkBytes += BlockSize;
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.files_total = chunks.size();
      progress_.files_done = 0;
      progress_.bytes_total = totalChunkBytes;
      progress_.bytes_done = 0;
    }
    setStage("Downloading CASC data archives...");
    log("Downloading " + std::to_string(chunks.size()) + " archive chunks with " +
        std::to_string(options.global_connections) + " connections");

    std::atomic<size_t> nextChunk{0};
    std::mutex maskMutex;
    auto worker = [&] {
      while (!stop_) {
        while (pause_ && !stop_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        size_t index = nextChunk.fetch_add(1);
        if (index >= chunks.size()) break;
        Chunk const& chunk = chunks[index];
        Archive& archive = archives[chunk.archive];
        uint64_t start = uint64_t(chunk.block) * BlockSize;
        uint64_t end = start + BlockSize - 1;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !stop_; ++attempt) {
          try {
            HttpRequest request(dataUrl(base, archive.hash));
            request.setTimeout(120);
            request.addHeader("Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end));
            if (!request.send() || (request.status() != 200 && request.status() != 206)) continue;
            File response = request.response();
            std::string body = readAll(response);
            if (body.empty()) continue;
            auto headers = request.headers();
            uint64_t archiveTotal = 0;
            auto range = headers.find("Content-Range");
            if (range != headers.end()) {
              unsigned long long rangeStart = 0, rangeEnd = 0, reportedTotal = 0;
              if (sscanf(range->second.c_str(), "bytes %llu-%llu/%llu",
                         &rangeStart, &rangeEnd, &reportedTotal) != 3) reportedTotal = 0;
              archiveTotal = reportedTotal;
            }
            int fd = open(archive.cacheFile.c_str(), O_RDWR | O_CREAT, 0644);
            if (fd < 0) continue;
            if (archiveTotal && ftruncate(fd, static_cast<off_t>(archiveTotal)) != 0) {
              close(fd);
              continue;
            }
            ssize_t written = pwrite(fd, body.data(), body.size(), static_cast<off_t>(start));
            close(fd);
            if (written != static_cast<ssize_t>(body.size())) continue;
            throttle(body.size());
            {
              std::lock_guard<std::mutex> lock(maskMutex);
              if (archive.mask.size() <= chunk.block / 8) archive.mask.resize(chunk.block / 8 + 1, 0);
              archive.mask[chunk.block / 8] |= 1U << (chunk.block & 7);
              writeBytes(archive.maskFile, std::string(archive.mask.begin(), archive.mask.end()));
            }
            uint64_t done = bytesDone.fetch_add(body.size()) + body.size();
            {
              std::lock_guard<std::mutex> lock(mutex_);
              progress_.files_done = nextChunk.load() > progress_.files_done ? nextChunk.load() : progress_.files_done;
              progress_.bytes_done = std::min(done, progress_.bytes_total);
              double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
              progress_.bytes_per_second = elapsed > 0 ? static_cast<uint64_t>(done / elapsed) : 0;
              progress_.eta_seconds = progress_.bytes_per_second && done < progress_.bytes_total
                  ? (progress_.bytes_total - done) / progress_.bytes_per_second : UINT64_MAX;
            }
            emit();
            ok = true;
            break;
          } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
          }
        }
        if (!ok) {
          ++failedChunks;
          log("Failed archive chunk " + archive.hash.substr(0, 12) + " block " + std::to_string(chunk.block));
        }
      }
    };
    int workerCount = std::max(1, std::min(options.global_connections, static_cast<int>(chunks.size())));
    std::vector<std::thread> workers;
    for (int i = 0; i < workerCount; ++i) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();
    if (stop_) throw Exception("Download stopped");
    if (failedChunks) throw Exception("Failed to download %llu archive chunks",
                                     static_cast<unsigned long long>(failedChunks.load()));

    setStage("Writing local CASC storage...");
    fs::create_directories(fs::path(options.output_dir) / "Data");
    NGDP::CascStorage storage((fs::path(options.output_dir) / "Data").string());
    File cdnFile = File::memfile(cdnText.data(), cdnText.size(), true);
    File buildFile = File::memfile(buildText.data(), buildText.size(), true);
    storage.addConfig(options.cdn_config, cdnFile);
    storage.addConfig(options.build_config, buildFile);
    for (auto const& archive : archives) {
      File index = File::memfile(archive.indexData.data(), archive.indexData.size(), true);
      storage.addIndex(archive.hash, index);
    }
    NGDP::DataStorage localStorage(storage);
    for (auto const& ekeyHex : neededEkeys) {
      File file = loadCachedBlte(ekeyHex, options, archives, locations);
      if (!file) throw Exception("missing cached file %s", ekeyHex.c_str());
      Hash ekey{};
      NGDP::from_string(ekey, ekeyHex);
      localStorage.addFile(ekey, file);
    }

    if (!buildConfig["patch-config"].empty()) {
      try {
        std::string patchConfig = fetchText(configUrl(base, "patch", buildConfig["patch-config"]));
        File patchFile = File::memfile(patchConfig.data(), patchConfig.size(), true);
        storage.addConfig(buildConfig["patch-config"], patchFile);
        if (!buildConfig["patch-index"].empty()) {
          std::string patchIndex = fetchText(dataUrl(base, secondToken(buildConfig["patch-index"])));
          File index = File::memfile(patchIndex.data(), patchIndex.size(), true);
          storage.addIndex(secondToken(buildConfig["patch-index"]), index);
        }
        if (!buildConfig["patch"].empty()) {
          std::string patch = fetchText(configUrl(base, "patch", buildConfig["patch"]));
          File patchFile = File::memfile(patch.data(), patch.size(), true);
          Hash patchHash{};
          NGDP::from_string(patchHash, buildConfig["patch"]);
          localStorage.addFile(patchHash, patchFile);
        }
      } catch (...) { log("Patch metadata unavailable; continuing with base install"); }
    }

    File info(fs::path(options.output_dir) / ".build.info", File::REWRITE);
    info.printf("Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16");
    info.printf("|Install Key!HEX:16|IM Size!DEC:4|CDN Path!STRING:0|CDN Hosts!STRING:0");
    info.printf("|Tags!STRING:0|Armadillo!STRING:0|Last Activated!STRING:0|Version!STRING:0|Keyring!HEX:16|KeyService!STRING:0\n");
    Hash installCkey{};
    NGDP::from_string(installCkey, firstToken(buildConfig["install"]));
    auto const* installEnc = encoding.getEncoding(installCkey);
    info.printf("%s|1|%s|%s|%s|%u|%s|level3.blizzard.com|%s:%s:%s||%s|%s||\n",
                options.region.c_str(), options.build_config.c_str(), options.cdn_config.c_str(),
                installEnc ? hashHex(installEnc->keys[0]).c_str() : "", installEnc ? installEnc->usize : 0,
                cdnPath.c_str(), options.platform.c_str(), options.arch.c_str(), options.locale.c_str(),
                buildConfig["build-name"].c_str(), buildConfig["build-name"].c_str());

    setStage("Installing files...");
    size_t installTotal = 0;
    for (size_t i = 0; i < installManifest.installs.size(); ++i) {
      if (!selected(installMask, static_cast<uint32_t>(i))) continue;
      if (options.max_files && installTotal >= options.max_files) break;
      ++installTotal;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.files_total = installTotal;
      progress_.files_done = 0;
      progress_.bytes_total = 0;
      progress_.bytes_done = 0;
    }
    size_t totalIndex = 0;
    size_t counted = 0;
    for (auto const& entry : installManifest.installs) {
      if (!selected(installMask, static_cast<uint32_t>(totalIndex++))) continue;
      if (options.max_files && counted >= options.max_files) break;
      progress_.bytes_total += entry.size;
      ++counted;
    }
    size_t installIndex = 0;
    size_t installedCount = 0;
    for (auto const& entry : installManifest.installs) {
      if (stop_) throw Exception("Download stopped");
      bool take = selected(installMask, static_cast<uint32_t>(installIndex++)) &&
                  (!options.max_files || installedCount < options.max_files);
      if (take) {
        ++installedCount;
        auto const* enc = encoding.getEncoding(entry.hash);
        if (!enc) throw Exception("missing encoding entry for %s", entry.name.c_str());
        File raw = loadCachedBlte(hashHex(enc->keys[0]), options, archives, locations);
        File decoded = NGDP::DecodeBLTE(raw, entry.size);
        if (!decoded) throw Exception("failed to extract %s", entry.name.c_str());
        fs::path relative(entry.name);
        if (relative.empty() || relative.is_absolute() ||
            std::any_of(relative.begin(), relative.end(), [](fs::path const& part) {
              return part == "..";
            })) {
          throw Exception("unsafe install path: %s", entry.name.c_str());
        }
        fs::path outputPath = (fs::path(options.output_dir) / relative).lexically_normal();
        fs::path outputRoot = fs::path(options.output_dir).lexically_normal();
        if (outputPath.string().find(outputRoot.string() + "/") != 0) {
          throw Exception("install path escapes output directory: %s", entry.name.c_str());
        }
        File output(outputPath.string(), File::REWRITE);
        output.copy(decoded);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++progress_.files_done;
          progress_.bytes_done += entry.size;
        }
        emit();
      }
    }
    log("Done");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.finished = true;
      progress_.stage = "Complete";
    }
    emit();
  } catch (Exception const& ex) {
    log(std::string("Error: ") + ex.what());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.failed = true;
      progress_.stage = "Failed";
    }
    emit();
  } catch (std::exception const& ex) {
    log(std::string("Error: ") + ex.what());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.failed = true;
      progress_.stage = "Failed";
    }
    emit();
  } catch (...) {
    log("Error: unknown downloader failure");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.failed = true;
      progress_.stage = "Failed";
    }
    emit();
  }
}
