#include "ngdp.h"
#include "base/checksum.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
  fs::path root = fs::temp_directory_path() / "blizzget-index-hash-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  {
    NGDP::CascStorage storage((root / "Data").string());
    NGDP::DataStorage data(storage);
    MemoryFile content;
    content.write("abc", 3);

    NGDP::Hash first{};
    NGDP::Hash second{};
    for (int i = 0; i < 16; ++i) {
      first[i] = static_cast<uint8_t>(i);
      second[i] = static_cast<uint8_t>(i);
    }
    second[8] ^= 0x11;
    for (int i = 9; i < 16; ++i) second[i] = static_cast<uint8_t>(0x80 + i);
    data.addFile(first, content);
    content.seek(0);
    data.addFile(second, content);
  }

  fs::path indexFile = root / "Data" / "data" / "0800000001.idx";
  if (!fs::exists(indexFile)) {
    std::cerr << "bucket 8 index file not generated\n";
    return 1;
  }

  File index(indexFile.string());
  index.seek(32);
  uint32_t blockSize = index.read32();
  uint32_t blockHash = index.read32();
  constexpr uint32_t canonicalHash = 0xB8E6D8EF;
  constexpr uint32_t oldBlizzGetHash = 0xBA825285;

  std::cout << "blockSize=" << blockSize << " blockHash=" << std::hex << blockHash << "\n";
  if (blockSize != 36) {
    std::cerr << "unexpected block size\n";
    return 1;
  }
  if (blockHash != canonicalHash) {
    std::cerr << "expected canonical CascLib hash " << canonicalHash
              << ", got " << blockHash << " (old BlizzGet hash was "
              << oldBlizzGetHash << ")\n";
    return 1;
  }
  return 0;
}
