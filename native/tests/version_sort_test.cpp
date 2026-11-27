#include "downloader.h"
#include "version_sort.h"

#include <iostream>
#include <string>
#include <vector>

int main() {
  std::vector<VersionRecord> records = {
    {"2.7.4.86197", "a"}, {"2.7.3.82785", "b"}, {"2.7.5.87562", "c"},
    {"2.8.0.99920", "d"}, {"2.6.10.73447", "e"}, {"2.10.0.1", "f"},
    {"2.9.9.99", "g"}, {"2.10.0.1", "h"}
  };
  records[6].created_at = "2025-01-01T00:00:00Z";
  records[7].created_at = "2026-01-01T00:00:00Z";

  std::vector<std::string> expected = {
    "2.10.0.1", "2.10.0.1", "2.9.9.99", "2.8.0.99920",
    "2.7.5.87562", "2.7.4.86197", "2.7.3.82785", "2.6.10.73447"
  };
  sortVersionsNewestFirst(records);
  for (size_t i = 0; i < expected.size(); ++i) {
    if (records[i].version != expected[i]) {
      std::cerr << "position " << i << ": expected " << expected[i]
                << ", got " << records[i].version << "\n";
      return 1;
    }
  }
  if (records[0].build_config != "h") {
    std::cerr << "newer created_at tie-break was not used\n";
    return 1;
  }
  return 0;
}
