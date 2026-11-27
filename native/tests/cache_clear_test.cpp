#include "cache.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
  fs::path root = fs::temp_directory_path() / "blizzget-cache-clear-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "cdn");
  fs::create_directories(root / "versions");
  std::ofstream(root / "cdn" / "cached.bin") << "cache";
  std::ofstream(root / "versions" / "us.json") << "[]";
  std::ofstream(root / "unrelated.txt") << "keep";

  std::string error;
  if (!clearBlizzgetCache(root.string(), error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (fs::exists(root / "cdn") || fs::exists(root / "versions")) {
    std::cerr << "BlizzGet cache contents were not removed\n";
    return 1;
  }
  if (!fs::exists(root / "unrelated.txt")) {
    std::cerr << "unrelated file was removed\n";
    return 1;
  }
  return 0;
}
