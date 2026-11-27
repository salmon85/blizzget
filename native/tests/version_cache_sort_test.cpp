#include "downloader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
  fs::path cache = fs::temp_directory_path() / "blizzget-version-cache-test";
  std::error_code ec;
  fs::remove_all(cache, ec);
  fs::create_directories(cache / "versions" / "d3");
  std::ofstream(cache / "versions" / "d3" / "us.json") << R"json([
    {"version":"2.7.4.86197","build_config":"a","cdn_config":"c","product_config":"p","created_at":"2025-01-01"},
    {"version":"2.7.3.82785","build_config":"b","cdn_config":"c","product_config":"p","created_at":"2024-01-01"},
    {"version":"2.8.0.99920","build_config":"d","cdn_config":"c","product_config":"p","created_at":"2026-01-01"},
    {"version":"2.10.0.1","build_config":"f","cdn_config":"c","product_config":"p","created_at":"2027-01-01"},
    {"version":"2.9.9.99","build_config":"g","cdn_config":"c","product_config":"p","created_at":"2026-06-01"}
  ])json";

  auto records = Downloader::fetchVersions("d3", "us", cache.string(), [](std::string const&) {});
  std::vector<std::string> expected = {"2.10.0.1", "2.9.9.99", "2.8.0.99920", "2.7.4.86197", "2.7.3.82785"};
  if (records.size() != expected.size()) return 1;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (records[i].version != expected[i]) {
      std::cerr << "position " << i << ": expected " << expected[i]
                << ", got " << records[i].version << "\n";
      return 1;
    }
  }
  return 0;
}
