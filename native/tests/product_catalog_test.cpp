#include "downloader.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
  fs::path cache = fs::temp_directory_path() / "blizzget-product-catalog-test";
  std::error_code ec;
  fs::remove_all(cache, ec);
  fs::create_directories(cache / "catalog");
  std::ofstream(cache / "catalog" / "summary.json") << R"json({
    "success": true,
    "result": {
      "results": [
        {
          "results": [
            {"product":"d3","name":"Diablo III","flags":"versions","encrypted":false},
            {"product":"d3","name":"Duplicate","flags":"versions","encrypted":false},
            {"product":"anbs","name":"Diablo Immortal","flags":"versions","encrypted":false},
            {"product":"scor","name":"Sea of Thieves","flags":"versions","encrypted":false},
            {"product":"d3","name":"CDN only","flags":"cdns","encrypted":false}
          ]
        }
      ]
    }
  })json";

  auto records = Downloader::fetchProductCatalog(cache.string(), [](std::string const&) {});
  if (records.size() != 3) {
    std::cerr << "expected 3 unique version products, got " << records.size() << "\n";
    return 1;
  }
  if (records[0].code != "d3" || records[0].name != "Diablo III" || records[0].encrypted) {
    std::cerr << "catalog sorting or parsing failed\n";
    return 1;
  }
  if (records[1].code != "anbs" || records[2].code != "scor") {
    std::cerr << "catalog order failed\n";
    return 1;
  }

  fs::create_directories(cache / "cdn-paths" / "d3t");
  std::ofstream(cache / "cdn-paths" / "d3t" / "us.json") << R"json({
    "success": true,
    "result": {
      "data": [
        {"region":"us","path":"tpr/diablo3"},
        {"region":"eu","path":"tpr/other"}
      ]
    }
  })json";
  if (Downloader::fetchCdnPath("d3t", "us", cache.string()) != "diablo3") {
    std::cerr << "CDN path resolution failed\n";
    return 1;
  }
  return 0;
}
