#include "ngdp.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> readAll(fs::path const& path) {
  std::vector<uint8_t> data(fs::file_size(path));
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(data.data()), data.size());
  return data;
}

int main() {
  fs::path root = fs::temp_directory_path() / "blizzget-data-storage-thread-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  std::atomic<bool> failed{false};
  {
    NGDP::CascStorage storage((root / "Data").string());
    NGDP::DataStorage data(storage);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
      workers.emplace_back([&, worker] {
        try {
          for (int item = 0; item < 100; ++item) {
            NGDP::Hash hash{};
            hash[0] = static_cast<uint8_t>(worker);
            hash[1] = static_cast<uint8_t>(worker ^ 0x5a);
            hash[2] = static_cast<uint8_t>(item);
            for (int i = 3; i < 16; ++i) hash[i] = static_cast<uint8_t>(item * 17 + i);
            std::string content(static_cast<size_t>(item + 1), static_cast<char>('a' + worker));
            File file = File::memfile(content.data(), content.size(), true);
            data.addFile(hash, file);
          }
        } catch (...) {
          failed = true;
        }
      });
    }
    for (auto& worker : workers) worker.join();
    data.finish();
  }
  if (failed) {
    std::cerr << "concurrent DataStorage writes threw\n";
    return 1;
  }

  constexpr size_t EntrySize = 18;
  std::set<std::string> hashes;
  size_t totalEntries = 0;
  for (int bucket = 0; bucket < 16; ++bucket) {
    char name[32];
    std::snprintf(name, sizeof name, "%02x00000001.idx", bucket);
    fs::path path = root / "Data" / "data" / name;
    if (!fs::exists(path)) {
      std::cerr << "missing index " << name << "\n";
      return 1;
    }
    auto bytes = readAll(path);
    uint32_t blockSize = bytes[32] | (bytes[33] << 8) | (bytes[34] << 16) | (bytes[35] << 24);
    uint32_t count = blockSize / EntrySize;
    for (uint32_t i = 0; i < count; ++i) {
      size_t pos = 40 + i * EntrySize;
      hashes.insert(std::string(reinterpret_cast<char const*>(bytes.data() + pos), 9));
      ++totalEntries;
    }
  }
  size_t dataFiles = 0;
  for (auto const& entry : fs::directory_iterator(root / "Data" / "data")) {
    if (entry.path().filename().string().rfind("data.", 0) == 0) ++dataFiles;
  }
  size_t expectedEntries = 800 + 16 * dataFiles;
  if (totalEntries != expectedEntries || hashes.size() != expectedEntries) {
    std::cerr << "concurrent index corruption: " << totalEntries << " entries, "
              << hashes.size() << " unique hashes, expected " << expectedEntries << "\n";
    return 1;
  }
  return 0;
}
