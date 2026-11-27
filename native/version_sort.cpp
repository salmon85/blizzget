#include "version_sort.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<uint64_t> versionComponents(std::string const& version) {
  std::vector<uint64_t> components;
  uint64_t value = 0;
  bool inNumber = false;
  for (unsigned char ch : version) {
    if (std::isdigit(ch)) {
      value = value * 10 + (ch - '0');
      inNumber = true;
    } else if (inNumber) {
      components.push_back(value);
      value = 0;
      inNumber = false;
    }
  }
  if (inNumber) components.push_back(value);
  return components;
}

} // namespace

void sortVersionsNewestFirst(std::vector<VersionRecord>& records) {
  std::sort(records.begin(), records.end(), [](VersionRecord const& left, VersionRecord const& right) {
    auto a = versionComponents(left.version);
    auto b = versionComponents(right.version);
    size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
      uint64_t av = i < a.size() ? a[i] : 0;
      uint64_t bv = i < b.size() ? b[i] : 0;
      if (av != bv) return av > bv;
    }
    if (left.created_at != right.created_at) return left.created_at > right.created_at;
    if (left.version != right.version) return left.version > right.version;
    return left.build_config > right.build_config;
  });
}
