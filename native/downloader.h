#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct VersionRecord {
  std::string version;
  std::string build_config;
  std::string cdn_config;
  std::string product_config;
  std::string created_at;
};

struct ProductRecord {
  std::string code;
  std::string name;
  bool encrypted = false;
};

struct DownloadOptions {
  std::string product;
  std::string region;
  std::string platform = "Windows";
  std::string arch = "x86_64";
  std::string locale = "enUS";
  std::string build_config;
  std::string cdn_config;
  std::string cdn_path;
  std::string output_dir;
  std::string cache_dir;
  int global_connections = 48;
  int per_host_connections = 12;
  uint64_t speed_cap = 0;
  size_t max_files = 0; // Native test hook; 0 means all selected files.
};

struct ProgressSnapshot {
  std::string stage;
  uint64_t files_total = 0;
  uint64_t files_done = 0;
  uint64_t bytes_total = 0;
  uint64_t bytes_done = 0;
  uint64_t bytes_per_second = 0;
  uint64_t eta_seconds = 0;
  uint64_t log_count = 0;
  std::string log_line;
  std::vector<std::string> log_lines;
  bool finished = false;
  bool failed = false;
};

class Downloader {
public:
  using Callback = std::function<void(ProgressSnapshot const&)>;

  void setCallback(Callback callback) { callback_ = std::move(callback); }
  void resetState() { stop_ = false; pause_ = false; }
  void requestStop() { stop_ = true; }
  void requestPause() { pause_ = !pause_.load(); }
  bool isPaused() const { return pause_.load(); }

  void run(DownloadOptions const& options);

  static std::vector<VersionRecord> fetchVersions(
      std::string const& product,
      std::string const& region,
      std::string const& cacheDir,
      std::function<void(std::string const&)> const& status);

  static std::vector<ProductRecord> fetchProductCatalog(
      std::string const& cacheDir,
      std::function<void(std::string const&)> const& status);

  static std::string fetchCdnPath(
      std::string const& product,
      std::string const& region,
      std::string const& cacheDir);

private:
  std::atomic<bool> stop_{false};
  std::atomic<bool> pause_{false};
  std::mutex mutex_;
  ProgressSnapshot progress_;
  Callback callback_;

  void log(std::string const& text);
  void setStage(std::string const& text);
  void emit();
};

std::string defaultCacheDirectory();
std::string formatBytes(uint64_t bytes);
std::string formatDuration(uint64_t seconds);
