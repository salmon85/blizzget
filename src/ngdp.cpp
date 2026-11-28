#include "ngdp.h"
#include "base/http.h"
#include "base/path.h"
#include "base/checksum.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <lz4.h>

namespace NGDP {

  const std::string HOST = "http://us.patch.battle.net:1119";

  static std::string cacheRoot() {
    if (char const* value = std::getenv("BLIZZGET_CACHE_DIR")) return value;
    if (char const* value = std::getenv("CASCETTE_CACHE_DIR")) return value;
    if (char const* home = std::getenv("HOME")) {
      return (std::filesystem::path(home) / ".cache" / "cascette").string();
    }
    return path::root() / "cache";
  }

  const std::map<std::string, std::string> ProgramCodes = {
    { "agent", "Battle.net Agent" },
    { "bna", "Battle.net App" },
    { "bnt", "Heroes of the Storm Alpha (Deprecated)" },
    { "d3", "Diablo 3 Retail" },
    { "d3cn", "Diablo 3 China" },
    { "d3t", "Diablo 3 Test" },
    { "demo", "Demo (Partial)" },
    { "dst2a", "Destiny 2 Alpha (Encrypted)" },
    { "hero", "Heroes of the Storm Retail" },
    { "herot", "Heroes of the Storm Test" },
    { "heroc", "Heroes of the Storm Tournament" },
    { "hsb", "Hearthstone" },
    { "hst", "Hearthstone Test (Partial)" },
    { "osib", "Diablo II: Resurrected (Alpha)" },
    { "pro", "Overwatch Retail" },
    { "prot", "Overwatch Test" },
    { "prob", "Overwatch Beta" },
    { "proc", "Overwatch Tournament" },
    { "prodev", "Overwatch Dev (Encrypted)" },
    { "s1", "StarCraft I" },
    { "s1a", "StarCraft I Alpha (Encrypted)" },
    { "s1t", "StarCraft I Test" },
    { "sc2", "StarCraft II (Deprecated)" },
    { "s2", "StarCraft II Retail" },
    { "s2t", "StarCraft II Test (Deprecated)" },
    { "s2b", "StarCraft II Beta (Deprecated)" },
    { "test", "Test (Deprecated)" },
    { "storm", "Heroes of the Storm (Deprecated)" },
    { "war3", "Warcraft III Old Ver (Partial)" },
    { "w3", "Warcraft III" },
    { "wow", "World of Warcraft Retail" },
    { "wowt", "World of Warcraft Test" },
    { "wow_beta", "World of Warcraft Beta" },
    { "wow_classic", "World of Warcraft Classic" },
    { "wow_classic_beta", "World of Warcraft Classic Beta" },
    { "wowdemo", "World of Warcraft Demo" },
    { "wowdev", "World of Warcraft Dev" },
  };

  NGDP::NGDP(std::string const& app)
    : program_(app)
  {
    File file = HttpRequest::get(HOST + "/" + app + "/cdns");
    if (!file) {
      throw Exception("failed to fetch cdns file");
    }
    for (std::string const& line : file) {
      if (line.substr(0, 2) == "##") continue;
      if (line.find('!') != std::string::npos || line.empty()) continue;
      auto parts = split(line, '|');
      auto& config = cdns_[parts[0]];
      config.path = parts[1];
      config.hosts = split(parts[2], ' ');
    }

    file = HttpRequest::get(HOST + "/" + app + "/versions");
    if (!file) {
      throw Exception("failed to fetch versions file");
    }
    for (std::string const& line : file) {
      if (line.substr(0, 2) == "##") continue;
      if (line.find('!') != std::string::npos || line.empty()) continue;
      auto parts = split(line, '|');
      auto& config = versions_[parts[0]];
      config.build = parts[1];
      config.cdn = parts[2];
      config.id = std::stoi(parts[4]);
      config.version = parts[5];
      if (cdns_.count(parts[0])) {
        regions_.push_back(parts[0]);
      }
    }
  }

  bool NGDP::setRegion(std::string const& region) {
    if (!cdns_.count(region) || !versions_.count(region)) {
      return false;
    }
    region_ = region;
    base_ = "http://" + cdns_[region].hosts[0] + "/" + cdns_[region].path + "/";
    return true;
  }

  std::string NGDP::geturl(std::string const& hash, std::string const& type, bool index) const {
    std::string url = base_ + type + "/" + hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash;
    if (index) url += ".index";
    return url;
  }
  File NGDP::load(std::string const& hash, std::string const& type, bool index) const {
    return HttpRequest::get(geturl(hash, type, index));
  }

  File DecodeBLTE(File& blte, uint32 eusize) {
    if (blte.read32(true) != 0x424C5445) return File();
    uint32 headerSize = blte.read32(true);
    if (headerSize) {
      std::vector<uint32> csize;
      std::vector<uint32> usize;
      uint16 flags = blte.read16(true);
      uint16 chunks = blte.read16(true);
      for (uint16 i = 0; i < chunks; ++i) {
        csize.push_back(blte.read32(true));
        usize.push_back(blte.read32(true));
        blte.seek(16, SEEK_CUR);
      }
      MemoryFile dst;
      std::vector<uint8> tmp;
      for (uint16 i = 0; i < chunks; ++i) {
        uint8 type = blte.read8();
        if (type == 'N') {
          if (csize[i] - 1 != usize[i]) return File();
          blte.read(dst.reserve(usize[i]), usize[i]);
        } else if (type == 'Z') {
          tmp.resize(csize[i] - 1);
          blte.read(&tmp[0], tmp.size());
          if (gzinflate(&tmp[0], tmp.size(), dst.reserve(usize[i]), &usize[i])) return File();
        } else if (type == 'L') {
          tmp.resize(csize[i] - 1);
          blte.read(&tmp[0], tmp.size());
          uint8* out = dst.reserve(usize[i]);
          if (LZ4_decompress_safe(reinterpret_cast<char const*>(tmp.data()),
                                  reinterpret_cast<char*>(out),
                                  static_cast<int>(tmp.size()),
                                  static_cast<int>(usize[i])) != static_cast<int>(usize[i])) {
            return File();
          }
        } else {
          // unsupported compression
          return File();
        }
      }
      dst.seek(0);
      return dst;
    } else {
      uint64 offset = blte.tell();
      uint64 size = blte.size() - offset;
      uint8 type = blte.read8();
      if (type == 'N') {
        return blte.subfile(offset + 1, size - 1);
      } else if (eusize) {
        blte.seek(offset + 1, SEEK_SET);
        std::vector<uint8> tmp(size - 1);
        blte.read(&tmp[0], tmp.size());
        MemoryFile dst;
        if (type == 'L') {
          uint8* out = dst.reserve(eusize);
          if (LZ4_decompress_safe(reinterpret_cast<char const*>(tmp.data()),
                                  reinterpret_cast<char*>(out),
                                  static_cast<int>(tmp.size()),
                                  static_cast<int>(eusize)) != static_cast<int>(eusize)) {
            return File();
          }
        } else if (gzinflate(&tmp[0], tmp.size(), dst.reserve(eusize), &eusize)) {
          return File();
        }
        dst.seek(0);
        return dst;
      } else {
        // unsupported compression
        return File();
      }
    }
  }

  ConfigFile ParseConfig(File& file) {
    ConfigFile result;
    if (!file) return result;
    for (std::string const& line : file) {
      if (line[0] == '#') continue;
      size_t pos = line.find(" = ");
      if (pos == std::string::npos) continue;
      result[line.substr(0, pos)] = line.substr(pos + 3);
    }
    return result;
  }

#pragma pack(push, 1)
  struct EncodingFileHeader {
    uint16 signature;
    uint8 unk;
    uint8 sizeA;
    uint8 sizeB;
    uint16 flagsA;
    uint16 flagsB;
    uint32 entriesA;
    uint32 entriesB;
    uint8 unk2;
    uint32 stringSize;
  };
#pragma pack(pop)

  void from_string(Hash hash, std::string const& str) {
    int val;
    for (int i = 0; i < sizeof(Hash); ++i) {
      sscanf(&str[i * 2], "%02x", &val);
      hash[i] = val;
    }
  }
  std::string to_string(const Hash hash) {
    return MD5::format(hash);
  }

  Encoding::Encoding(File& file) {
    EncodingFileHeader header;
    file.read(&header, sizeof header);
    flip(header.signature);
    flip(header.entriesA);
    flip(header.entriesB);
    flip(header.stringSize);
    if (header.signature != 0x454E || header.sizeA != 16 || header.sizeB != 16) {
      throw Exception("invalid encoding file");
    }

    uint32 size = file.size();
    uint32 posHeaderA = sizeof(EncodingFileHeader) + header.stringSize;
    uint32 posEntriesA = posHeaderA + header.entriesA * 32;
    uint32 posHeaderB = posEntriesA + header.entriesA * 4096;
    uint32 posEntriesB = posHeaderB + header.entriesB * 32;
    uint32 posLayout = posEntriesB + header.entriesB * 4096;

    data_.resize(header.stringSize + (header.entriesA + header.entriesB) * 4096 + (size - posLayout));
    char* layouts = (char*) &data_[0];
    uint8* entriesA = &data_[header.stringSize];
    uint8* entriesB = entriesA + header.entriesA * 4096;
    layout_ = (char*) (entriesB + header.entriesB * 4096);

    file.read(layouts, header.stringSize);
    file.seek(posEntriesA, SEEK_SET);
    file.read(entriesA, header.entriesA * 4096);
    file.seek(posEntriesB, SEEK_SET);
    file.read(entriesB, header.entriesB * 4096);
    file.read(layout_, size - posLayout);

    for (char* ptr = layouts; ptr < layouts + header.stringSize; ++ptr) {
      layouts_.push_back(ptr);
      while (*ptr) ++ptr;
    }

    file.seek(posHeaderA, SEEK_SET);
    encodingTable_.resize(header.entriesA);
    for (uint32 i = 0; i < header.entriesA; ++i) {
      file.read(encodingTable_[i].hash, sizeof(Hash));
      Hash blockHash, realHash;
      file.read(blockHash, sizeof blockHash);
      MD5::checksum(entriesA, 4096, realHash);
      if (memcmp(realHash, blockHash, sizeof(Hash))) {
        throw Exception("encoding file checksum mismatch");
      }
      for (uint8* ptr = entriesA; ptr + sizeof(EncodingEntry) <= entriesA + 4096;) {
        EncodingEntry* entry = reinterpret_cast<EncodingEntry*>(ptr);
        if (!entry->keyCount) break;
        encodingTable_[i].entries.push_back(entry);
        flip(entry->usize);
        ptr += sizeof(EncodingEntry) + (entry->keyCount - 1) * sizeof(Hash);
      }
      entriesA += 4096;
    }

    Hash nilHash;
    memset(nilHash, 0, sizeof(Hash));

    file.seek(posHeaderB, SEEK_SET);
    layoutTable_.resize(header.entriesB);
    for (uint32 i = 0; i < header.entriesB; ++i) {
      file.read(layoutTable_[i].key, sizeof(Hash));
      Hash blockHash, realHash;
      file.read(blockHash, sizeof blockHash);
      MD5::checksum(entriesB, 4096, realHash);
      if (memcmp(realHash, blockHash, sizeof(Hash))) {
        throw Exception("encoding file checksum mismatch");
      }
      for (uint8* ptr = entriesB; ptr + sizeof(LayoutEntry) <= entriesB + 4096;) {
        LayoutEntry* entry = reinterpret_cast<LayoutEntry*>(ptr);
        if (!memcmp(entry->key, nilHash, sizeof(Hash))) {
          break;
        }
        layoutTable_[i].entries.push_back(entry);
        flip(entry->stringIndex);
        flip(entry->csize);
        ptr += sizeof(LayoutEntry);
      }
      entriesB += 4096;
    }
  }

  template<class Vec, class Comp>
  typename Vec::const_iterator find_hash(Vec const& vec, Comp less) {
    if (vec.empty() || less(vec[0])) return vec.end();
    size_t left = 0, right = vec.size();
    while (right - left > 1) {
      size_t mid = (left + right) / 2;
      if (less(vec[mid])) {
        right = mid;
      } else {
        left = mid;
      }
    }
    return vec.begin() + left;
  }

  Encoding::EncodingEntry const* Encoding::getEncoding(const Hash hash) const {
    auto it = find_hash(encodingTable_, [&hash](EncodingHeader const& rhs) {
      return memcmp(hash, rhs.hash, sizeof(Hash)) < 0;
    });
    if (it == encodingTable_.end()) return nullptr;
    auto sub = find_hash(it->entries, [&hash](EncodingEntry const* rhs) {
      return memcmp(hash, rhs->hash, sizeof(Hash)) < 0;
    });
    if (sub == it->entries.end()) return nullptr;
    if (memcmp((*sub)->hash, hash, sizeof(Hash))) return nullptr;
    return *sub;
  }
  Encoding::LayoutEntry const* Encoding::getLayout(const Hash key) const {
    auto it = find_hash(layoutTable_, [&key](LayoutHeader const& rhs) {
      return memcmp(key, rhs.key, sizeof(Hash)) < 0;
    });
    if (it == layoutTable_.end()) return nullptr;
    auto sub = find_hash(it->entries, [&key](LayoutEntry const* rhs) {
      return memcmp(key, rhs->key, sizeof(Hash)) < 0;
    });
    if (sub == it->entries.end()) return nullptr;
    if (memcmp((*sub)->key, key, sizeof(Hash))) return nullptr;
    return *sub;
  }

  CascStorage::CascStorage(std::string const& root)
    : root_(root)
  {
    path::create(root / "config");
    path::create(root / "data");
    path::create(root / "indices");
    path::create(root / "patch");

    std::vector<std::string> names;
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(root / "data", ec)) {
      if (!entry.is_directory(ec)) names.push_back(entry.path().filename().string());
    }

    for (std::string const& name : names) {
      std::filesystem::remove(root / "data" / name, ec);
    }
  }

  File& CascStorage::addConfig(std::string const& hash, File& file) {
    file.seek(0);
    File(root_ / "config" / hash.substr(0, 2) / hash.substr(2, 2) / hash, File::REWRITE).copy(file);
    file.seek(0);
    return file;
  }
  File& CascStorage::addIndex(std::string const& hash, File& file) {
    file.seek(0);
    File(root_ / "indices" / hash + ".index", File::REWRITE).copy(file);
    file.seek(0);
    return file;
  }
  File CascStorage::addData(std::string const& name) {
    return File(root_ / "data" / name, File::REWRITE);
  }

  File& CascStorage::addCache(std::string const& name, File& file) {
    file.seek(0);
    File(cacheRoot() / name, File::REWRITE).copy(file);
    file.seek(0);
    return file;
  }
  File CascStorage::addCache(std::string const& name) {
    return File(cacheRoot() / name, File::MODIFY);
  }
  File CascStorage::getCache(std::string const& name) {
    return File(cacheRoot() / name);
  }

  static uint32 localChecksumA(uint8 const* header) {
    return hashlittle(header, 0x16, 0x3D6BE971);
  }

  static uint32 localChecksumB(uint8 const* header, uint64 globalOffset) {
    static const uint32 lut[16] = {
      0x049396B8, 0x72A82A9B, 0xEE626CCA, 0x9917754F,
      0x15DE40B1, 0xF5A8A9B6, 0x421EAC7E, 0xA9D55C9A,
      0x317FD40C, 0x04FAF80D, 0x3D6BE971, 0x52933CFD,
      0x27F64B7D, 0xC6F5C11B, 0xD5757E3A, 0x6C388745,
    };
    uint8 accum[4]{};
    for (uint32 i = 0; i < 0x1A; ++i) {
      accum[static_cast<uint32>(globalOffset + i) & 3] ^= header[i];
    }
    uint32 offsetPlusHeader = static_cast<uint32>(globalOffset + 0x1E);
    uint32 mask = lut[offsetPlusHeader & 0xF] ^ offsetPlusHeader;
    uint8 maskBytes[4];
    memcpy(maskBytes, &mask, 4);
    uint8 result[4];
    for (uint32 i = 0; i < 4; ++i) {
      uint32 j = static_cast<uint32>(globalOffset + 0x1A + i) & 3;
      result[i] = accum[j] ^ maskBytes[j];
    }
    uint32 value;
    memcpy(&value, result, 4);
    return value;
  }

  static void writeLocalHeader(File& out, uint8 const* key, uint32 encodedSize,
                               uint16 flags, uint64 globalOffset) {
    uint8 header[0x1E]{};
    for (int i = 0; i < 16; ++i) header[i] = key[15 - i];
    memcpy(header + 0x10, &encodedSize, 4);
    memcpy(header + 0x14, &flags, 2);
    uint32 checksumA = localChecksumA(header);
    memcpy(header + 0x16, &checksumA, 4);
    uint32 checksumB = localChecksumB(header, globalOffset);
    memcpy(header + 0x1A, &checksumB, 4);
    out.write(header, sizeof header);
  }

  static uint8 localBucket(uint8 const* key, uint8 seed) {
    uint8 folded = 0;
    for (int i = 0; i < 9; ++i) folded ^= key[i];
    return static_cast<uint8>((((folded >> 4) ^ folded) + seed) & 0x0F);
  }

  static void buildSegmentKey(Hash out, Hash pathHash, uint32 segmentCount, uint8 bucket) {
    memcpy(out, pathHash, 16);
    out[1] = static_cast<uint8>(segmentCount);
    out[2] = static_cast<uint8>(segmentCount >> 8);
    for (uint32 probe = 0; probe < 0x100; ++probe) {
      out[0] = static_cast<uint8>(probe);
      if (localBucket(out, 1) == bucket) return;
    }
    out[0] = 0;
  }

  static void buildSegmentHeader(uint8* header, Hash pathHash, uint32 segmentIndex) {
    uint64 segmentBase = static_cast<uint64>(segmentIndex) << 30;
    MemoryFile block;
    for (uint32 bucket = 0; bucket < 16; ++bucket) {
      Hash key;
      buildSegmentKey(key, pathHash, segmentIndex, static_cast<uint8>(bucket));
      uint64 globalOffset = segmentBase + bucket * 0x1E;
      writeLocalHeader(block, key, 0x1E, 1, globalOffset);
    }
    block.seek(0);
    block.read(header, 0x1E0);
  }

  DataStorage::DataStorage(CascStorage& storage)
    : storage_(storage)
    , dataCount_(0)
  {
  }

  File& DataStorage::addFile(const Hash hash, File& file) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file) return file;

    uint32 bucket = localBucket(hash, 0);
    auto& index = indices_[bucket];
    if (index.size() >= MaxIndexEntries) writeIndex(bucket);

    uint32 totalSize = 0x1E + static_cast<uint32>(file.size());
    if (!data_ || data_.size() + totalSize > MaxDataSize) {
      if (data_) data_.flush();
      std::string name = fmtstring("data.%03u", dataCount_);
      data_ = storage_.addData(name);
      uint32 archiveId = dataCount_++;

      Hash pathHash;
      std::string archivePath = storage_.dataPath(name);
      MD5::checksum(archivePath.data(), static_cast<uint32>(archivePath.size()), pathHash);
      uint8 segmentHeader[0x1E0];
      buildSegmentHeader(segmentHeader, pathHash, archiveId);
      data_.write(segmentHeader, sizeof segmentHeader);

      for (uint32 slot = 0; slot < 16; ++slot) {
        Hash reconKey;
        for (int i = 0; i < 16; ++i) reconKey[i] = segmentHeader[slot * 0x1E + 15 - i];
        uint32 reconBucket = localBucket(reconKey, 1);
        auto& reconIndex = indices_[reconBucket];
        if (reconIndex.size() >= MaxIndexEntries) writeIndex(reconBucket);
        reconIndex.emplace_back();
        auto& recon = reconIndex.back();
        memcpy(recon.hash, reconKey, sizeof(Hash));
        recon.index = static_cast<uint16>(archiveId);
        recon.offset = slot * 0x1E;
        recon.size = 0x1E;
      }
    }

    uint32 archiveId = dataCount_ - 1;
    uint32 offset = static_cast<uint32>(data_.tell());
    uint64 globalOffset = (static_cast<uint64>(archiveId) << 30) | offset;
    writeLocalHeader(data_, hash, totalSize, 0, globalOffset);
    file.seek(0);
    data_.copy(file);
    file.seek(0);

    index.emplace_back();
    auto& entry = index.back();
    memcpy(entry.hash, hash, sizeof(Hash));
    entry.index = static_cast<uint16>(archiveId);
    entry.offset = offset;
    entry.size = totalSize;
    return file;
  }

#pragma pack(push, 1)
  struct V7IndexHeader {
    uint16 version = 7;
    uint8 bucket = 0;
    uint8 extraBytes = 0;
    uint8 sizeBytes = 4;
    uint8 offsetBytes = 5;
    uint8 keyBytes = 9;
    uint8 segmentBits = 30;
    uint64 segmentSize = 0x4000000000ULL;
  };
#pragma pack(pop)

  void DataStorage::writeIndex(uint32 bucket) {
    if (bucket >= IndexCount) return;
    auto& index_ = indices_[bucket];
    if (index_.empty() && indexVersions_[bucket] != 0) return;
    if (index_.empty()) return;

    uint32 version = ++indexVersions_[bucket];
    std::sort(index_.begin(), index_.end(), [](IndexEntry const& lhs, IndexEntry const& rhs) {
      return memcmp(lhs.hash, rhs.hash, sizeof(Hash)) < 0;
    });

    V7IndexHeader header;
    header.bucket = static_cast<uint8_t>(bucket);

    File index = storage_.addData(fmtstring("%02x%08x.idx", bucket, version));
    index.write32(sizeof(V7IndexHeader));
    index.write32(hashlittle(&header, sizeof header, 0));
    index.write(&header, sizeof header);
    index.write32(0);
    index.write32(0);

    uint32 blockSize = static_cast<uint32>(index_.size() * 18);
    uint32 hashHigh = 0;
    uint32 hashLow = 0;
    std::vector<std::array<uint8_t, 18>> rawEntries;
    rawEntries.reserve(index_.size());
    for (IndexEntry const& entry : index_) {
      std::array<uint8_t, 18> raw{};
      memcpy(raw.data(), entry.hash, 9);
      raw[9] = static_cast<uint8_t>(entry.index >> 2);
      uint32 low = (static_cast<uint32>(entry.index & 3) << 30) | (entry.offset & 0x3FFFFFFF);
      raw[10] = static_cast<uint8_t>(low >> 24);
      raw[11] = static_cast<uint8_t>(low >> 16);
      raw[12] = static_cast<uint8_t>(low >> 8);
      raw[13] = static_cast<uint8_t>(low);
      raw[14] = static_cast<uint8_t>(entry.size);
      raw[15] = static_cast<uint8_t>(entry.size >> 8);
      raw[16] = static_cast<uint8_t>(entry.size >> 16);
      raw[17] = static_cast<uint8_t>(entry.size >> 24);
      hashlittle2(raw.data(), raw.size(), &hashHigh, &hashLow);
      rawEntries.push_back(raw);
    }

    index.write32(blockSize);
    index.write32(hashHigh);
    for (auto const& raw : rawEntries) index.write(raw.data(), raw.size());

    while (index.tell() & 0xFFF) index.write8(0);
    uint64 target = index.tell() + 0xC000;
    target = (target + 0xFFFF) & ~static_cast<uint64>(0xFFFF);
    if (target < 0x10000) target = 0x10000;
    while (index.tell() < target) index.write8(0);
    index.flush();

    index_.clear();
  }

}
