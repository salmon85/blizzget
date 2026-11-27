#include "ngdp.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <string>

namespace fs = std::filesystem;

int bucketFor(NGDP::Hash const& hash) {
  uint8_t value = 0;
  for (int i = 0; i < 9; ++i) value ^= hash[i];
  return ((value >> 4) ^ value) & 0x0F;
}

int main() {
  fs::path root = fs::temp_directory_path() / "blizzget-index-layout-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  {
    NGDP::CascStorage storage((root / "Data").string());
    NGDP::DataStorage data(storage);
    MemoryFile content;
    content.write("x", 1);
    for (int i = 0; i < 64; ++i) {
      NGDP::Hash hash{};
      hash[0] = static_cast<uint8_t>(i * 37);
      for (int j = 1; j < 16; ++j) hash[j] = static_cast<uint8_t>(i * 29 + j * 17);
      content.seek(0);
      data.addFile(hash, content);
    }
  }

  std::set<std::string> names;
  for (auto const& entry : fs::directory_iterator(root / "Data" / "data")) {
    if (entry.path().extension() == ".idx") names.insert(entry.path().filename().string());
  }
  if (names.size() != 16) {
    std::cerr << "expected 16 index files, got " << names.size() << "\n";
    return 1;
  }

  size_t totalEntries = 0;
  for (int bucket = 0; bucket < 16; ++bucket) {
    char expected[32];
    std::snprintf(expected, sizeof expected, "%02x00000001.idx", bucket);
    if (!names.count(expected)) {
      std::cerr << "missing " << expected << "\n";
      return 1;
    }
    File index((root / "Data" / "data" / expected).string());
    index.seek(10);
    if (index.read8() != bucket) {
      std::cerr << "wrong bucket index in " << expected << "\n";
      return 1;
    }
    index.seek(32);
    uint32_t blockSize = index.read32();
    index.seek(40);
    for (uint32_t offset = 0; offset < blockSize; offset += 18) {
      NGDP::Hash hash{};
      for (int i = 0; i < 9; ++i) hash[i] = index.read8();
      index.seek(9, SEEK_CUR);
      if (bucketFor(hash) != bucket) {
        std::cerr << "key in wrong bucket in " << expected << "\n";
        return 1;
      }
      ++totalEntries;
    }
  }
  if (totalEntries != 64) {
    std::cerr << "expected 64 indexed entries, got " << totalEntries << "\n";
    return 1;
  }
  return 0;
}
