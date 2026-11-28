#include "ngdp.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int bucketFor(NGDP::Hash const& hash, int seed = 0) {
  uint8_t value = 0;
  for (int i = 0; i < 9; ++i) value ^= hash[i];
  return (((value >> 4) ^ value) + seed) & 0x0F;
}

static std::vector<uint8_t> readAll(fs::path const& path) {
  std::vector<uint8_t> data(fs::file_size(path));
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(data.data()), data.size());
  return data;
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

  constexpr size_t EntrySize = 18;
  size_t totalEntries = 0;
  for (int bucket = 0; bucket < 16; ++bucket) {
    char expected[32];
    std::snprintf(expected, sizeof expected, "%02x00000001.idx", bucket);
    if (!names.count(expected)) {
      std::cerr << "missing " << expected << "\n";
      return 1;
    }
    auto bytes = readAll(root / "Data" / "data" / expected);
    uint32_t blockSize = bytes[32] | (bytes[33] << 8) | (bytes[34] << 16) | (bytes[35] << 24);
    uint32_t count = blockSize / EntrySize;
    for (uint32_t i = 0; i < count; ++i) {
      size_t pos = 40 + i * EntrySize;
      NGDP::Hash hash{};
      memcpy(hash, bytes.data() + pos, 9);
      uint32_t size = bytes[pos + 14] | (bytes[pos + 15] << 8) |
                      (bytes[pos + 16] << 16) | (bytes[pos + 17] << 24);
      int seed = (size == 30) ? 1 : 0;
      if (bucketFor(hash, seed) != bucket) {
        std::cerr << "key in wrong bucket in " << expected << "\n";
        return 1;
      }
      ++totalEntries;
    }
  }
  if (totalEntries != 80) {
    std::cerr << "expected 80 indexed entries, got " << totalEntries << "\n";
    return 1;
  }
  return 0;
}
