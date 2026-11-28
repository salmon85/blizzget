#include "ngdp.h"
#include "base/checksum.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> readAll(fs::path const& path) {
  std::vector<uint8_t> data(fs::file_size(path));
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(data.data()), data.size());
  return data;
}

static uint32_t readLE32(std::vector<uint8_t> const& data, size_t pos) {
  return data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24);
}

int main() {
  fs::path root = fs::temp_directory_path() / "blizzget-index-hash-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  NGDP::Hash first{};
  NGDP::Hash second{};
  {
    NGDP::CascStorage storage((root / "Data").string());
    NGDP::DataStorage data(storage);
    MemoryFile content;
    content.write("abc", 3);

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

  auto bytes = readAll(indexFile);
  if (bytes.size() != 0x10000 || readLE32(bytes, 0) != 16 || bytes[8] != 7 ||
      bytes[10] != 8 || bytes[12] != 4 || bytes[13] != 5 || bytes[14] != 9 ||
      bytes[15] != 30) {
    std::cerr << "bad V7 local index header\n";
    return 1;
  }
  uint64_t segmentSize = 0;
  for (int i = 7; i >= 0; --i) segmentSize = (segmentSize << 8) | bytes[16 + i];
  if (segmentSize != 0x4000000000ULL) {
    std::cerr << "bad segment size\n";
    return 1;
  }
  if (readLE32(bytes, 4) != hashlittle(bytes.data() + 8, 16, 0)) {
    std::cerr << "header hash mismatch\n";
    return 1;
  }

  uint32_t blockSize = readLE32(bytes, 32);
  uint32_t blockHash = readLE32(bytes, 36);
  uint32_t count = blockSize / 18;
  if (count < 3) {
    std::cerr << "expected data and reconstruction entries, got " << count << "\n";
    return 1;
  }
  uint32_t high = 0, low = 0;
  for (uint32_t i = 0; i < count; ++i) {
    hashlittle2(bytes.data() + 40 + i * 18, 18, &high, &low);
  }
  if (blockHash != high) {
    std::cerr << "entry block hash mismatch\n";
    return 1;
  }
  bool foundData = false;
  for (uint32_t i = 0; i < count; ++i) {
    size_t pos = 40 + i * 18;
    if (memcmp(bytes.data() + pos, first, 9) != 0) continue;
    uint32_t size = readLE32(bytes, pos + 14);
    uint32_t offset = ((bytes[pos + 10] & 0x3F) << 24) | (bytes[pos + 11] << 16) |
                      (bytes[pos + 12] << 8) | bytes[pos + 13];
    if (size != 33 || offset != 0x1E0) {
      std::cerr << "bad first entry size/offset " << size << "/" << offset << "\n";
      return 1;
    }
    foundData = true;
  }
  if (!foundData) {
    std::cerr << "data entry not found in index\n";
    return 1;
  }

  auto dataBytes = readAll(root / "Data" / "data" / "data.000");
  if (readLE32(dataBytes, 0x10) != 30 || dataBytes[0x14] != 1 || dataBytes[0x15] != 0) {
    std::cerr << "bad segment reconstruction header\n";
    return 1;
  }
  if (readLE32(dataBytes, 0x16) != hashlittle(dataBytes.data(), 0x16, 0x3D6BE971)) {
    std::cerr << "segment checksum_a mismatch\n";
    return 1;
  }
  for (int i = 0; i < 16; ++i) {
    if (dataBytes[0x1E0 + i] != first[15 - i]) {
      std::cerr << "data entry key is not reversed\n";
      return 1;
    }
  }
  if (readLE32(dataBytes, 0x1F0) != 33 || dataBytes[0x1F4] != 0 || dataBytes[0x1F5] != 0) {
    std::cerr << "bad data entry local header\n";
    return 1;
  }
  if (readLE32(dataBytes, 0x1F6) != hashlittle(dataBytes.data() + 0x1E0, 0x16, 0x3D6BE971)) {
    std::cerr << "data entry checksum_a mismatch\n";
    return 1;
  }
  if (memcmp(dataBytes.data() + 0x1E0 + 30, "abc", 3) != 0) {
    std::cerr << "data entry payload missing\n";
    return 1;
  }
  return 0;
}
