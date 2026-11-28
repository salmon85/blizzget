#include "downloader.h"
#include "fallback_catalog.h"
#include "version_sort.h"
#include "base/error.h"
#include "base/http.h"
#include "ngdp.h"

#include <json-glib/json-glib.h>
#include <glib.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <functional>
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
  std::string lastError;
  for (int attempt = 0; attempt < 3; ++attempt) {
    HttpRequest request(url);
    request.setTimeout(90);
    bool transferred = request.send();
    uint32 status = request.status();
    if (transferred && (status == 200 || status == 206)) return request.response();
    lastError = transferred ? "HTTP " + std::to_string(status) + " for " + url
                            : "transfer failed (HTTP " + std::to_string(status) + ") for " + url;
    if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(250 << attempt));
  }
  throw Exception("%s", lastError.c_str());
}

void fetchFileToPath(std::string const& url, std::string const& path, uint64_t resumeOffset = 0) {
  HttpRequest request(url);
  request.setTimeout(90);
  bool transferred = request.download(path, resumeOffset);
  if (!transferred) throw Exception("transfer failed (HTTP %u) for %s", request.status(), url.c_str());
  if (resumeOffset && request.status() == 200) {
    std::error_code ec;
    fs::remove(path, ec);
    throw Exception("server ignored resume range for %s", url.c_str());
  }
  if (request.status() != 200 && request.status() != 206) {
    std::error_code ec;
    fs::remove(path, ec);
    throw Exception("HTTP %u for %s", request.status(), url.c_str());
  }
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

bool jsonBoolean(JsonNode* object, char const* name) {
  JsonNode* node = member(object, name);
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_boolean(node);
}

bool validProductCode(std::string const& code) {
  return !code.empty() && std::all_of(code.begin(), code.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-';
  });
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

std::string parseCurrentCdnConfig(std::string const& text, std::string const& region) {
  JsonParser* parser = parseJson(text);
  JsonNode* result = member(json_parser_get_root(parser), "result");
  JsonNode* data = member(result, "data");
  std::string config;
  if (data && JSON_NODE_HOLDS_ARRAY(data)) {
    JsonArray* array = json_node_get_array(data);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
      JsonNode* entry = json_array_get_element(array, i);
      if (jsonString(entry, "region") != region) continue;
      config = jsonString(entry, "cdn_config");
      break;
    }
  }
  g_object_unref(parser);
  if (config.size() != 32 || !std::all_of(config.begin(), config.end(), [](unsigned char c) { return std::isxdigit(c); })) return {};
  return config;
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

std::vector<ProductRecord> parseProductCatalog(std::string const& text) {
  JsonParser* parser = parseJson(text);
  JsonNode* root = json_parser_get_root(parser);
  JsonNode* summaries = member(member(root, "result"), "results");
  std::vector<ProductRecord> records;
  if (summaries && JSON_NODE_HOLDS_ARRAY(summaries) && json_array_get_length(json_node_get_array(summaries))) {
    JsonNode* snapshot = json_array_get_element(json_node_get_array(summaries), 0);
    JsonNode* products = member(snapshot, "results");
    if (products && JSON_NODE_HOLDS_ARRAY(products)) {
      JsonArray* array = json_node_get_array(products);
      std::unordered_set<std::string> seen;
      for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonNode* entry = json_array_get_element(array, i);
        if (jsonString(entry, "flags") != "versions") continue;
        ProductRecord record;
        record.code = jsonString(entry, "product");
        record.name = jsonString(entry, "name");
        record.encrypted = jsonBoolean(entry, "encrypted");
        if (record.code.empty() || !validProductCode(record.code) ||
            !seen.insert(record.code).second) continue;
        if (record.name.empty()) record.name = record.code;
        records.push_back(std::move(record));
      }
    }
  }
  g_object_unref(parser);
  std::sort(records.begin(), records.end(), [](ProductRecord const& a, ProductRecord const& b) {
    std::string an = a.name, bn = b.name;
    std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return std::tolower(c); });
    if (an != bn) return an < bn;
    return a.code < b.code;
  });
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
  auto fallback = fallbackCdnPaths.find(product);
  if (fallback != fallbackCdnPaths.end()) return fallback->second;
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
  size_t bytes = (entries + 7) / 8;
  std::vector<uint8_t> mask(bytes, 0xFF);
  std::set<uint16_t> wantedTypes;
  for (auto const& tag : tags) {
    if (wanted.count(tag.name)) wantedTypes.insert(tag.type);
  }
  if (wantedTypes.empty()) return mask;
  for (uint16_t type : wantedTypes) {
    std::vector<uint8_t> selected(bytes, 0);
    std::vector<uint8_t> covered(bytes, 0);
    for (auto const& tag : tags) {
      if (tag.type != type) continue;
      for (size_t i = 0; i < bytes && i < tag.mask.size(); ++i) {
        covered[i] |= tag.mask[i];
        if (wanted.count(tag.name)) selected[i] |= tag.mask[i];
      }
    }
    for (size_t i = 0; i < bytes; ++i) {
      mask[i] &= selected[i] | static_cast<uint8_t>(~covered[i]);
    }
  }
  return mask;
}

bool selected(std::vector<uint8_t> const& mask, uint32_t index) {
  return (mask[index / 8] & (1U << (7 - (index & 7)))) != 0;
}

fs::path installRelativePath(std::string const& name) {
  std::string normalized = name;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return fs::path(normalized);
}

template <typename Entry>
std::vector<std::string> selectedEntryHashes(std::vector<Entry> const& entries,
                                             std::vector<uint8_t> const& mask,
                                             size_t maxFiles) {
  std::vector<std::string> hashes;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!selected(mask, static_cast<uint32_t>(i))) continue;
    if (maxFiles && hashes.size() >= maxFiles) break;
    hashes.push_back(NGDP::to_string(entries[i].hash));
  }
  return hashes;
}

struct DownloadEntry {
  Hash hash{};
  uint64_t size = 0;
  uint8_t priority = 0;
  std::vector<uint8_t> flags;
};

inline bool downloadEntryNeeded(DownloadEntry const& entry, bool skipOptional) {
  return !skipOptional || entry.priority <= 1;
}

inline bool includeDownloadEntry(DownloadEntry const& entry, bool selected, bool skipOptional,
                                 bool entireCdn) {
  return entireCdn || (selected && downloadEntryNeeded(entry, skipOptional));
}

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
  uint8_t flagBytes = 0;
  if (version >= 2) {
    flagBytes = file.read8();
    if (version >= 3) file.seek(4, SEEK_CUR);
  }
  Manifest manifest;
  manifest.downloads.reserve(entries);
  for (uint32_t i = 0; i < entries; ++i) {
    DownloadEntry entry;
    file.read(entry.hash, sizeof entry.hash);
    for (int byte = 0; byte < 5; ++byte) entry.size = (entry.size << 8) | file.read8();
    entry.priority = file.read8();
    if (checksums) file.seek(4, SEEK_CUR);
    entry.flags.resize(flagBytes);
    if (flagBytes) file.read(entry.flags.data(), flagBytes);
    manifest.downloads.push_back(std::move(entry));
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

bool archiveRangeReady(Location const& location, std::vector<uint8_t> const& mask) {
  uint32_t start = location.offset / BlockSize;
  uint32_t end = (location.offset + location.size + BlockSize - 1) / BlockSize;
  for (uint32_t block = start; block < end; ++block) {
    if (block / 8 >= mask.size() || !(mask[block / 8] & (1U << (block & 7)))) return false;
  }
  return true;
}

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

std::string versionFromBuildName(std::string const& buildName) {
  std::vector<std::string> version;
  std::string buildId;
  std::string token;
  auto consume = [&]() {
    if (token.empty() || !std::isdigit(static_cast<unsigned char>(token[0]))) {
      token.clear();
      return;
    }
    size_t digits = 0;
    while (digits < token.size() && std::isdigit(static_cast<unsigned char>(token[digits]))) ++digits;
    std::string number = token.substr(0, digits);
    if (number.size() >= 4) buildId = number;
    else version.push_back(number);
    token.clear();
  };
  for (char c : buildName) {
    if (c == '_' || c == '-' || c == ' ') consume();
    else token.push_back(c);
  }
  consume();
  if (version.empty()) return buildName;
  std::string result;
  for (auto const& part : version) {
    if (!result.empty()) result += '.';
    result += part;
  }
  if (!buildId.empty()) result += "." + buildId;
  return result;
}

std::string utcTimestamp() {
  std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

bool lowerEndsWith(std::string value, std::string const& suffix) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isClientExecutableName(std::string const& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (!lowerEndsWith(lower, ".exe")) return false;
  static char const* excluded[] = {"inspector", "blizzarderror", "launcher", "agent",
                                   "redist", "vc_", "unins", "crash", "support", "setup"};
  for (char const* token : excluded) {
    if (lower.find(token) != std::string::npos) return false;
  }
  return true;
}

// Returns 1 for the modern 15-column layout, 0 for legacy, -1 if the client
// executable does not identify exactly one layout.
int detectBuildInfoSchema(std::string const& executableBytes) {
  bool modern = executableBytes.find("CDN Servers") != std::string::npos;
  bool legacy = executableBytes.find("KeyService") != std::string::npos;
  if (modern == legacy) return -1;
  return modern ? 1 : 0;
}

bool extractedFileMatches(fs::path const& path, Hash const& contentKey, uint64_t expectedSize) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != expectedSize || ec) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  GChecksum* checksum = g_checksum_new(G_CHECKSUM_MD5);
  char buffer[1 << 20];
  while (input) {
    input.read(buffer, sizeof buffer);
    std::streamsize count = input.gcount();
    if (count > 0) g_checksum_update(checksum, reinterpret_cast<unsigned char*>(buffer), static_cast<gssize>(count));
  }
  if (input.bad()) {
    g_checksum_free(checksum);
    return false;
  }
  std::string actual = g_checksum_get_string(checksum);
  g_checksum_free(checksum);
  return actual == NGDP::to_string(contentKey);
}

void writeBytes(std::string const& path, std::string const& data) {
  File file(path, File::REWRITE);
  if (!file) throw Exception("failed to write %s", path.c_str());
  file.write(data.data(), data.size());
  file.flush();
}

std::string cachePath(DownloadOptions const& options, std::string const& hash) {
  std::string cdnPath = options.cdn_path.empty() ? cdnPathForProduct(options.product) : options.cdn_path;
  return options.cache_dir / "cdn" / cdnPath /
         "data" / hash.substr(0, 2) / hash.substr(2, 4) / hash;
}

int cascWorkerCount(int connections, size_t totalFiles) {
  return std::max(1, std::min({8, std::max(1, connections), static_cast<int>(totalFiles)}));
}

constexpr int LooseFetchAttempts = 8;

File fetchLooseWithRetry(std::string const& path,
                         std::function<void(std::string const&, uint64_t)> const& fetchInto) {
  std::string partial = path + ".part";
  for (int attempt = 0; attempt < LooseFetchAttempts; ++attempt) {
    try {
      fs::create_directories(fs::path(path).parent_path());
      uint64_t offset = 0;
      std::error_code sizeError;
      uint64_t existing = fs::file_size(partial, sizeError);
      if (!sizeError) offset = existing;
      fetchInto(partial, offset);
      if (!fs::exists(partial) || fs::file_size(partial) == 0) {
        throw Exception("empty loose CDN response");
      }
      std::error_code ec;
      fs::rename(partial, path, ec);
      if (ec) throw Exception("failed to cache loose CDN file: %s", ec.message().c_str());
      return File(path);
    } catch (...) {
      if (attempt + 1 == LooseFetchAttempts) {
        std::error_code ec;
        fs::remove(partial, ec);
        throw;
      }
      int delay = std::min(200 << std::min(attempt, 5), 5000);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
  }
  return File();
}

File loadCachedBlte(std::string const& ekey, DownloadOptions const& options,
                    std::vector<Archive> const& archives,
                    std::unordered_map<std::string, Location> const& locations,
                    std::function<void(std::string const&, std::string const&, uint64_t)> const& fetchLoose) {
  std::string path = cachePath(options, ekey);
  File file(path);
  if (file) return file;
  auto fetchLooseFile = [&]() -> File {
    if (!fetchLoose) return File();
    return fetchLooseWithRetry(path, [&](std::string const& destination, uint64_t offset) {
      fetchLoose(ekey, destination, offset);
    });
  };
  auto it = locations.find(ekey);
  if (it == locations.end()) return fetchLooseFile();
  Archive const& archive = archives[it->second.archive];
  File source(archive.cacheFile);
  if (!source) return fetchLooseFile();
  source.seek(it->second.offset);
  MemoryFile result;
  uint8_t* output = result.reserve(it->second.size);
  if (source.read(output, it->second.size) != it->second.size) {
    return fetchLooseFile();
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

uint64_t etaFromProgress(uint64_t done, uint64_t total, double elapsedSeconds) {
  if (!done || done >= total || elapsedSeconds <= 0.0) return UINT64_MAX;
  double eta = (static_cast<double>(total - done) * elapsedSeconds) / static_cast<double>(done);
  if (!std::isfinite(eta) || eta >= static_cast<double>(UINT64_MAX)) return UINT64_MAX;
  return std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(eta)));
}

uint64_t etaFromRate(uint64_t done, uint64_t total, double filesPerSecond) {
  if (done >= total || filesPerSecond <= 0.0) return UINT64_MAX;
  double eta = static_cast<double>(total - done) / filesPerSecond;
  if (!std::isfinite(eta) || eta >= static_cast<double>(UINT64_MAX)) return UINT64_MAX;
  return std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(eta)));
}

uint64_t bytesPerSecond(uint64_t delta, double elapsedSeconds) {
  if (!delta || elapsedSeconds <= 0.0) return 0;
  double rate = static_cast<double>(delta) / elapsedSeconds;
  if (!std::isfinite(rate) || rate >= static_cast<double>(UINT64_MAX)) return UINT64_MAX;
  return static_cast<uint64_t>(rate);
}

double progressFraction(ProgressSnapshot const& snapshot) {
  if (snapshot.casc_files_total) {
    double written = static_cast<double>(snapshot.casc_files_done) / static_cast<double>(snapshot.casc_files_total);
    if (!snapshot.bytes_total && !snapshot.files_total) return written;
    double downloaded = snapshot.bytes_total
        ? static_cast<double>(snapshot.bytes_done) / static_cast<double>(snapshot.bytes_total)
        : 0.0;
    return std::clamp((downloaded + written) / 2.0, 0.0, 1.0);
  }
  double fraction = 0.0;
  if (snapshot.bytes_total) fraction = static_cast<double>(snapshot.bytes_done) / static_cast<double>(snapshot.bytes_total);
  else if (snapshot.files_total) fraction = static_cast<double>(snapshot.files_done) / static_cast<double>(snapshot.files_total);
  return std::clamp(fraction, 0.0, 1.0);
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

std::vector<ProductRecord> Downloader::fetchProductCatalog(
    std::string const& cacheDir, std::function<void(std::string const&)> const& status) {
  fs::path cacheFile = fs::path(cacheDir) / "catalog" / "summary.json";
  std::error_code ec;
  std::string cachedText;
  if (fs::exists(cacheFile, ec)) {
    std::ifstream input(cacheFile);
    cachedText.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    auto age = std::chrono::duration_cast<std::chrono::hours>(
        fs::file_time_type::clock::now() - fs::last_write_time(cacheFile, ec));
    if (!ec && age < std::chrono::hours(24)) {
      auto records = parseProductCatalog(cachedText);
      status("Loaded " + std::to_string(records.size()) + " products from cached BlizzTrack catalog");
      return records;
    }
  }

  try {
    status("Updating BlizzTrack product catalog...");
    File file = fetchFile("https://blizztrack.com/api/summary");
    std::string text = readAll(file);
    auto records = parseProductCatalog(text);
    writeBytes(cacheFile.string(), text);
    status("Updated catalog: " + std::to_string(records.size()) + " products");
    return records;
  } catch (...) {
    if (!cachedText.empty()) {
      auto records = parseProductCatalog(cachedText);
      status("Using cached BlizzTrack catalog: " + std::to_string(records.size()) + " products");
      return records;
    }
    throw;
  }
}

std::string parseCdnPath(std::string const& text, std::string const& region) {
  JsonParser* parser = parseJson(text);
  JsonNode* data = member(member(json_parser_get_root(parser), "result"), "data");
  std::string path;
  if (data && JSON_NODE_HOLDS_ARRAY(data)) {
    JsonArray* array = json_node_get_array(data);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
      JsonNode* entry = json_array_get_element(array, i);
      if (jsonString(entry, "region") != region) continue;
      path = jsonString(entry, "path");
      break;
    }
  }
  g_object_unref(parser);
  if (path.rfind("tpr/", 0) == 0) path.erase(0, 4);
  while (!path.empty() && path.back() == '/') path.pop_back();
  return path;
}

std::string Downloader::fetchCdnPath(std::string const& product, std::string const& region,
                                     std::string const& cacheDir) {
  if (!validProductCode(product) || (region != "us" && region != "eu")) return {};
  fs::path cacheFile = fs::path(cacheDir) / "cdn-paths" / fs::path(product) / fs::path(region + ".json");
  std::error_code ec;
  std::string cachedText;
  if (fs::exists(cacheFile, ec)) {
    std::ifstream input(cacheFile);
    cachedText.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    auto age = std::chrono::duration_cast<std::chrono::hours>(
        fs::file_time_type::clock::now() - fs::last_write_time(cacheFile, ec));
    if (!ec && age < std::chrono::hours(24)) return parseCdnPath(cachedText, region);
  }
  try {
    File file = fetchFile("https://blizztrack.com/api/manifest/" + product + "/cdns");
    std::string text = readAll(file);
    std::string path = parseCdnPath(text, region);
    if (!path.empty()) writeBytes(cacheFile.string(), text);
    return path;
  } catch (...) {
    if (!cachedText.empty()) return parseCdnPath(cachedText, region);
    return {};
  }
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
    std::string cdnPath = options.cdn_path.empty() ? cdnPathForProduct(options.product) : options.cdn_path;
    std::string base = "https://level3.blizzard.com/tpr/" + cdnPath + "/";
    setStage("Fetching build configuration...");
    std::string effectiveCdnConfig = options.cdn_config;
    std::string cdnText;
    try {
      cdnText = fetchText(configUrl(base, "config", effectiveCdnConfig));
    } catch (Exception const&) {
      std::string current = fetchText("https://blizztrack.com/api/manifest/" + options.product + "/versions");
      effectiveCdnConfig = parseCurrentCdnConfig(current, options.region);
      if (effectiveCdnConfig.empty()) throw Exception("no current CDN config available for %s (%s)", options.product.c_str(), options.region.c_str());
      log("Historical CDN config unavailable; using current config " + effectiveCdnConfig);
      cdnText = fetchText(configUrl(base, "config", effectiveCdnConfig));
    }
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

    InstallEntry const* clientExeEntry = nullptr;
    {
      size_t entryIndex = 0;
      for (auto const& entry : installManifest.installs) {
        bool isSelected = selected(installMask, static_cast<uint32_t>(entryIndex++));
        if (!isSelected || !isClientExecutableName(entry.name)) continue;
        if (!clientExeEntry || entry.size > clientExeEntry->size) clientExeEntry = &entry;
      }
    }
    if (clientExeEntry) {
      log("Client executable for schema detection: " + clientExeEntry->name);
    } else {
      log("No client executable found for schema detection");
    }

    std::vector<std::string> neededEkeys;
    neededEkeys.push_back(encodingKey);
    neededEkeys.push_back(downloadPair.second);
    neededEkeys.push_back(installPair.second);

    size_t skippedOptional = 0;
    uint64_t skippedOptionalBytes = 0;
    size_t entireEntries = 0;
    uint64_t entireBytes = 0;
    for (size_t i = 0; i < downloadManifest.downloads.size(); ++i) {
      bool isSelected = selected(downloadMask, static_cast<uint32_t>(i));
      auto const& entry = downloadManifest.downloads[i];
      if (!includeDownloadEntry(entry, isSelected, options.skip_optional, options.entire_cdn)) {
        if (isSelected) {
          ++skippedOptional;
          skippedOptionalBytes += entry.size;
        }
        continue;
      }
      if (options.entire_cdn) {
        ++entireEntries;
        entireBytes += entry.size;
      }
      neededEkeys.push_back(NGDP::to_string(entry.hash));
      if (options.max_files && neededEkeys.size() >= options.max_files) break;
    }
    if (options.entire_cdn) {
      log("Entire CDN mode: downloading all " + std::to_string(entireEntries) +
          " entries (" + std::to_string(entireBytes / (1024 * 1024)) + " MiB)");
    } else if (skippedOptional) {
      log("Skipping " + std::to_string(skippedOptional) + " optional entries (" +
          std::to_string(skippedOptionalBytes / (1024 * 1024)) + " MiB)");
    }
    for (auto const& hashText : selectedEntryHashes(installManifest.installs, installMask, options.max_files)) {
      Hash hash{};
      NGDP::from_string(hash, hashText);
      auto const* enc = encoding.getEncoding(hash);
      if (enc) neededEkeys.push_back(NGDP::to_string(enc->keys[0]));
    }
    {
      std::unordered_set<std::string> seen;
      std::vector<std::string> unique;
      unique.reserve(neededEkeys.size());
      for (auto& ekey : neededEkeys) {
        if (seen.insert(ekey).second) unique.push_back(std::move(ekey));
      }
      neededEkeys = std::move(unique);
    }

    setStage("Loading CDN archive indices...");
    std::vector<std::string> archiveHashes = split(cdnConfig["archives"]);
    std::vector<Archive> archives(archiveHashes.size());
    for (size_t i = 0; i < archiveHashes.size(); ++i) {
      archives[i].hash = archiveHashes[i];
      archives[i].cacheFile = cachePath(options, archiveHashes[i]);
      archives[i].maskFile = archives[i].cacheFile + ".mask";
    }
    std::atomic<size_t> nextArchiveIndex{0};
    std::atomic<bool> indexFailed{false};
    std::exception_ptr indexError;
    std::mutex indexErrorMutex;
    auto indexWorker = [&] {
      while (!stop_ && !indexFailed) {
        size_t index = nextArchiveIndex.fetch_add(1);
        if (index >= archives.size()) break;
        try {
          std::string data;
          for (int attempt = 0; attempt < 3; ++attempt) {
            try {
              data = fetchText(dataUrl(base, archiveHashes[index], true));
              break;
            } catch (...) {
              if (attempt == 2) throw;
              std::this_thread::sleep_for(std::chrono::milliseconds(250 << attempt));
            }
          }
          archives[index].indexData = std::move(data);
          fs::create_directories(fs::path(archives[index].cacheFile).parent_path());
        } catch (...) {
          {
            std::lock_guard<std::mutex> lock(indexErrorMutex);
            if (!indexError) indexError = std::current_exception();
          }
          indexFailed = true;
        }
      }
    };
    int indexThreads = std::max(1, std::min(16, static_cast<int>(archives.size())));
    log("Loading " + std::to_string(archives.size()) + " archive indices with " +
        std::to_string(indexThreads) + " connections");
    std::vector<std::thread> indexWorkers;
    for (int i = 0; i < indexThreads; ++i) indexWorkers.emplace_back(indexWorker);
    for (auto& thread : indexWorkers) thread.join();
    if (indexError) std::rethrow_exception(indexError);
    if (stop_) throw Exception("Download stopped");

    std::vector<std::pair<std::string, std::string>> patchIndices;
    for (auto const& hash : split(cdnConfig["patch-archives"])) {
      if (hash.size() != 32) continue;
      try {
        patchIndices.emplace_back(hash, fetchText(configUrl(base, "patch", hash) + ".index"));
      } catch (...) {
        log("Patch archive index unavailable: " + hash);
      }
    }

    std::unordered_map<std::string, Location> locations;
    for (size_t i = 0; i < archives.size(); ++i) {
      auto archiveLocations = loadIndex(archives[i].indexData);
      for (auto const& item : archiveLocations) {
        Location location = item.second;
        location.archive = i;
        locations[item.first] = location;
      }
      log("Loaded archive index " + archives[i].hash.substr(0, 12));
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

    std::vector<std::string> looseEkeys;
    std::vector<std::string> archiveEkeys;
    looseEkeys.reserve(neededEkeys.size());
    archiveEkeys.reserve(neededEkeys.size());
    for (auto const& ekey : neededEkeys) {
      if (locations.find(ekey) == locations.end()) looseEkeys.push_back(ekey);
      else archiveEkeys.push_back(ekey);
    }
    size_t looseCount = looseEkeys.size();
    if (looseCount) {
      log("CASC scheduling: " + std::to_string(looseCount) + " loose-only files in parallel with " +
          std::to_string(archiveEkeys.size()) + " archive-backed files");
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

    std::unique_ptr<NGDP::CascStorage> storage;
    std::unique_ptr<NGDP::DataStorage> localStorage;
    {
      fs::create_directories(fs::path(options.output_dir) / "Data");
      storage = std::make_unique<NGDP::CascStorage>((fs::path(options.output_dir) / "Data").string());
      File cdnFile = File::memfile(cdnText.data(), cdnText.size(), true);
      File buildFile = File::memfile(buildText.data(), buildText.size(), true);
      storage->addConfig(effectiveCdnConfig, cdnFile);
      storage->addConfig(options.build_config, buildFile);
      for (auto const& archive : archives) {
        File index = File::memfile(archive.indexData.data(), archive.indexData.size(), true);
        storage->addIndex(archive.hash, index);
      }
      for (auto const& patchIndex : patchIndices) {
        File index = File::memfile(patchIndex.second.data(), patchIndex.second.size(), true);
        storage->addIndex(patchIndex.first, index);
      }
      localStorage = std::make_unique<NGDP::DataStorage>(*storage);
    }
    auto fetchLoose = [&](std::string const& ekey, std::string const& destination, uint64_t offset) {
      fetchFileToPath(dataUrl(base, ekey), destination, offset);
    };

    std::atomic<size_t> nextChunk{0};
    std::atomic<size_t> nextLooseFile{0};
    std::atomic<size_t> nextArchiveFile{0};
    std::atomic<size_t> cascFilesDone{0};
    std::atomic<uint64_t> cascBytesDone{0};
    std::atomic<bool> writerFailed{false};
    std::mutex maskMutex;
    std::condition_variable chunkReady;
    auto cascStarted = std::chrono::steady_clock::now();
    auto cascRateTime = cascStarted;
    size_t cascRateDone = 0;
    double cascRate = 0.0;
    auto cascByteRateTime = cascStarted;
    uint64_t cascByteRateDone = 0;
    double cascByteRate = 0.0;
    bool downloadsDone = false;
    std::exception_ptr writerError;
    std::mutex writerErrorMutex;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.casc_files_total = neededEkeys.size();
      progress_.casc_files_done = 0;
      progress_.casc_eta_seconds = UINT64_MAX;
    }
    auto cascWriter = [&](std::vector<std::string> const& queue, std::atomic<size_t>& nextFile) {
      try {
        while (!stop_ && !writerFailed) {
          size_t index = nextFile.fetch_add(1);
          if (index >= queue.size()) break;
          std::string const& ekeyHex = queue[index];
          while (pause_ && !stop_ && !writerFailed) std::this_thread::sleep_for(std::chrono::milliseconds(100));
          auto location = locations.find(ekeyHex);
          if (location != locations.end() && !File(cachePath(options, ekeyHex))) {
            Archive& archive = archives[location->second.archive];
            std::unique_lock<std::mutex> lock(maskMutex);
            while (!archiveRangeReady(location->second, archive.mask)) {
              if (stop_) throw Exception("Download stopped");
              if (downloadsDone) throw Exception("missing archive chunks for %s", ekeyHex.c_str());
              chunkReady.wait_for(lock, std::chrono::milliseconds(100));
            }
          }
          File file = loadCachedBlte(ekeyHex, options, archives, locations, fetchLoose);
          if (!file) throw Exception("missing cached file %s", ekeyHex.c_str());
          uint64_t fileSize = file.size();
          Hash ekey{};
          NGDP::from_string(ekey, ekeyHex);
          if (localStorage) localStorage->addFile(ekey, file);
          size_t done = cascFilesDone.fetch_add(1) + 1;
          uint64_t bytes = cascBytesDone.fetch_add(fileSize) + fileSize;
          auto now = std::chrono::steady_clock::now();
          {
            std::lock_guard<std::mutex> lock(mutex_);
            double elapsed = std::chrono::duration<double>(now - cascRateTime).count();
            size_t delta = done - cascRateDone;
            if (delta && elapsed > 0.0) {
              double instant = static_cast<double>(delta) / elapsed;
              cascRate = cascRate > 0.0 ? cascRate * 0.7 + instant * 0.3 : instant;
              cascRateDone = done;
              cascRateTime = now;
            }
            double byteElapsed = std::chrono::duration<double>(now - cascByteRateTime).count();
            if (byteElapsed >= 0.25) {
              uint64_t instantBytes = bytesPerSecond(bytes - cascByteRateDone, byteElapsed);
              double instant = static_cast<double>(instantBytes);
              cascByteRate = cascByteRate > 0.0 ? cascByteRate * 0.7 + instant * 0.3 : instant;
              cascByteRateDone = bytes;
              cascByteRateTime = now;
            }
            progress_.casc_files_done = done;
            progress_.casc_eta_seconds = etaFromRate(done, neededEkeys.size(), cascRate);
            progress_.casc_bytes_done = bytes;
            progress_.casc_bytes_per_second = static_cast<uint64_t>(cascByteRate);
          }
          emit();
        }
      } catch (...) {
        {
          std::lock_guard<std::mutex> lock(writerErrorMutex);
          if (!writerError) writerError = std::current_exception();
        }
        writerFailed = true;
        chunkReady.notify_all();
      }
    };
    int looseThreads = looseEkeys.empty() ? 0 : cascWorkerCount(options.global_connections, looseEkeys.size());
    int archiveThreads = archiveEkeys.empty() ? 0 : std::max(1, std::min(looseEkeys.empty() ? 8 : 4,
                                                                                    std::min(options.global_connections,
                                                                                             static_cast<int>(archiveEkeys.size()))));
    std::vector<std::thread> cascWriterThreads;
    for (int i = 0; i < looseThreads; ++i) cascWriterThreads.emplace_back(cascWriter, std::cref(looseEkeys), std::ref(nextLooseFile));
    for (int i = 0; i < archiveThreads; ++i) cascWriterThreads.emplace_back(cascWriter, std::cref(archiveEkeys), std::ref(nextArchiveFile));
    log("CASC writer started with " + std::to_string(looseThreads) + " loose and " +
        std::to_string(archiveThreads) + " archive workers");

    auto worker = [&] {
      while (!stop_ && !writerFailed) {
        while (pause_ && !stop_ && !writerFailed) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        size_t index = nextChunk.fetch_add(1);
        if (index >= chunks.size()) break;
        Chunk const& chunk = chunks[index];
        Archive& archive = archives[chunk.archive];
        uint64_t start = uint64_t(chunk.block) * BlockSize;
        uint64_t end = start + BlockSize - 1;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !stop_ && !writerFailed; ++attempt) {
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
              chunkReady.notify_all();
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
    {
      std::lock_guard<std::mutex> lock(maskMutex);
      downloadsDone = true;
      chunkReady.notify_all();
    }
    for (auto& thread : cascWriterThreads) thread.join();
    if (writerError && !failedChunks && !stop_) std::rethrow_exception(writerError);
    if (stop_) throw Exception("Download stopped");
    if (failedChunks) throw Exception("Failed to download %llu archive chunks",
                                     static_cast<unsigned long long>(failedChunks.load()));
    if (writerError) std::rethrow_exception(writerError);

    if (localStorage) {
      setStage("Writing local CASC storage...");
      {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.files_total = neededEkeys.size();
        progress_.files_done = cascFilesDone.load();
        progress_.bytes_total = 0;
        progress_.bytes_done = 0;
        progress_.bytes_per_second = 0;
        progress_.eta_seconds = UINT64_MAX;
        progress_.casc_files_total = 0;
        progress_.casc_files_done = 0;
        progress_.casc_eta_seconds = UINT64_MAX;
        progress_.casc_bytes_done = 0;
        progress_.casc_bytes_per_second = 0;
      }
      emit();
      localStorage->finish();
    }

    if (!buildConfig["patch-config"].empty()) {
      try {
        std::string patchConfig = fetchText(configUrl(base, "config", buildConfig["patch-config"]));
        File patchFile = File::memfile(patchConfig.data(), patchConfig.size(), true);
        storage->addConfig(buildConfig["patch-config"], patchFile);
      } catch (...) { log("Patch configuration unavailable; continuing with base install"); }
    }

    File info(fs::path(options.output_dir) / ".build.info", File::REWRITE);
    std::string activated = utcTimestamp();
    std::string version = versionFromBuildName(buildConfig["build-name"]);
    int versionMajor = 0;
    int versionMinor = 0;
    std::sscanf(version.c_str(), "%d.%d", &versionMajor, &versionMinor);
    bool versionRequestsModern = versionMajor > 2 || (versionMajor == 2 && versionMinor >= 8);
    bool modernBuildInfo = versionRequestsModern;
    if (clientExeEntry) {
      auto const* clientEnc = encoding.getEncoding(clientExeEntry->hash);
      if (clientEnc) {
        File raw = loadCachedBlte(hashHex(clientEnc->keys[0]), options, archives, locations, fetchLoose);
        File decoded = raw ? NGDP::DecodeBLTE(raw, clientExeEntry->size) : File();
        if (decoded) {
          int detected = detectBuildInfoSchema(readAll(decoded));
          if (detected >= 0) {
            modernBuildInfo = detected == 1;
            log(std::string("Detected ") + (modernBuildInfo ? "modern" : "legacy") +
                " .build.info schema from " + clientExeEntry->name);
          } else {
            log("Client executable did not identify a unique .build.info schema; using version rule");
          }
        } else {
          log("Could not decode client executable for schema detection; using version rule");
        }
      }
    }
    if (modernBuildInfo) {
      info.printf("Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16");
      info.printf("|Install Key!HEX:16|IM Size!DEC:4|CDN Path!STRING:0|CDN Hosts!STRING:0");
      info.printf("|CDN Servers!STRING:0|Tags!STRING:0|Armadillo!STRING:0|Last Activated!STRING:0");
      info.printf("|Version!STRING:0|KeyRing!HEX:16|Product!STRING:0\n");
    } else {
      info.printf("Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16");
      info.printf("|Install Key!HEX:16|IM Size!DEC:4|CDN Path!STRING:0|CDN Hosts!STRING:0");
      info.printf("|Tags!STRING:0|Armadillo!STRING:0|Last Activated!STRING:0|Version!STRING:0|Keyring!HEX:16|KeyService!STRING:0\n");
    }
    Hash installCkey{};
    NGDP::from_string(installCkey, firstToken(buildConfig["install"]));
    auto const* installEnc = encoding.getEncoding(installCkey);
    std::string cdnInfoPath = cdnPath.rfind("tpr/", 0) == 0 ? cdnPath : "tpr/" + cdnPath;
    if (modernBuildInfo) {
      info.printf("%s|1|%s|%s|%s|%u|%s|level3.blizzard.com", options.region.c_str(),
                  options.build_config.c_str(), effectiveCdnConfig.c_str(),
                  installEnc ? hashHex(installEnc->keys[0]).c_str() : "", installEnc ? installEnc->usize : 0,
                  cdnInfoPath.c_str());
      info.printf("|http://level3.blizzard.com/?maxhosts=4 https://level3.ssl.blizzard.com/?maxhosts=4");
      info.printf("|%s:%s:%s||%s|%s||\n", options.platform.c_str(), options.arch.c_str(),
                  options.locale.c_str(), activated.c_str(), version.c_str());
    } else {
      info.printf("%s|1|%s|%s|%s|%u|%s|level3.blizzard.com|%s:%s:%s||%s|%s||\n",
                  options.region.c_str(), options.build_config.c_str(), effectiveCdnConfig.c_str(),
                  installEnc ? hashHex(installEnc->keys[0]).c_str() : "", installEnc ? installEnc->usize : 0,
                  cdnInfoPath.c_str(), options.platform.c_str(), options.arch.c_str(), options.locale.c_str(),
                  activated.c_str(), version.c_str());
    }

    setStage("Installing files...");
    std::vector<InstallEntry const*> installEntries;
    installEntries.reserve(installManifest.installs.size());
    size_t installIndex = 0;
    for (auto const& entry : installManifest.installs) {
      if (selected(installMask, static_cast<uint32_t>(installIndex++))) {
        installEntries.push_back(&entry);
        if (options.max_files && installEntries.size() >= options.max_files) break;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.files_total = installEntries.size();
      progress_.files_done = 0;
      progress_.bytes_total = 0;
      progress_.bytes_done = 0;
    }
    for (auto const* entry : installEntries) progress_.bytes_total += entry->size;

    std::atomic<size_t> nextInstall{0};
    std::atomic<size_t> skippedFiles{0};
    std::atomic<bool> extractionFailed{false};
    std::mutex extractionErrorMutex;
    std::exception_ptr extractionError;
    auto extractWorker = [&] {
      while (!stop_ && !extractionFailed) {
        size_t index = nextInstall.fetch_add(1);
        if (index >= installEntries.size()) break;
        InstallEntry const& entry = *installEntries[index];
        try {
          fs::path relative = installRelativePath(entry.name);
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
          if (extractedFileMatches(outputPath, entry.hash, entry.size)) {
            ++skippedFiles;
          } else {
            auto const* enc = encoding.getEncoding(entry.hash);
            if (!enc) throw Exception("missing encoding entry for %s", entry.name.c_str());
            File raw = loadCachedBlte(hashHex(enc->keys[0]), options, archives, locations, fetchLoose);
            File decoded = NGDP::DecodeBLTE(raw, entry.size);
            if (!decoded) throw Exception("failed to extract %s", entry.name.c_str());
            std::error_code dirError;
            fs::create_directories(outputPath.parent_path(), dirError);
            File output(outputPath.string(), File::REWRITE);
            output.copy(decoded);
          }
          {
            std::lock_guard<std::mutex> lock(mutex_);
            ++progress_.files_done;
            progress_.bytes_done += entry.size;
          }
          emit();
        } catch (...) {
          {
            std::lock_guard<std::mutex> lock(extractionErrorMutex);
            if (!extractionError) extractionError = std::current_exception();
          }
          extractionFailed = true;
        }
      }
    };
    int extractThreads = std::max(1, std::min(8, static_cast<int>(installEntries.size())));
    std::vector<std::thread> extractWorkers;
    for (int i = 0; i < extractThreads; ++i) extractWorkers.emplace_back(extractWorker);
    for (auto& worker : extractWorkers) worker.join();
    if (extractionError) std::rethrow_exception(extractionError);
    if (stop_) throw Exception("Download stopped");
    if (skippedFiles) log("Skipped " + std::to_string(skippedFiles.load()) + " verified files");
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
