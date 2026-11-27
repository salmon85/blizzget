#include "cache.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

std::vector<Product> mergeProductCatalog(std::vector<Product> const& fallback,
                                         std::vector<ProductRecord> const& catalog,
                                         bool showEncrypted) {
  std::vector<Product> products = fallback;
  std::unordered_set<std::string> seen;
  for (auto const& product : fallback) seen.insert(product.code);
  for (auto const& record : catalog) {
    if (record.encrypted && !showEncrypted) continue;
    if (!record.code.empty() && seen.insert(record.code).second) {
      products.push_back({record.code, record.name, record.encrypted});
    }
  }
  std::sort(products.begin() + fallback.size(), products.end(), [](Product const& a, Product const& b) {
    std::string an = a.name, bn = b.name;
    std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return std::tolower(c); });
    if (an != bn) return an < bn;
    return a.code < b.code;
  });
  return products;
}

std::vector<Product> filterProducts(std::vector<Product> const& products,
                                    std::string const& query) {
  std::string needle = query;
  auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  auto first = std::find_if(needle.begin(), needle.end(), notSpace);
  auto last = std::find_if(needle.rbegin(), needle.rend(), notSpace).base();
  if (first >= last) return products;
  needle = std::string(first, last);
  std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });

  std::vector<Product> matches;
  for (auto const& product : products) {
    std::string name = product.name;
    std::string code = product.code;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::tolower(c); });
    if (name.find(needle) != std::string::npos || code.find(needle) != std::string::npos) matches.push_back(product);
  }
  return matches;
}

bool clearBlizzgetCache(std::string const& path, std::string& error) {
  fs::path root(path);
  for (char const* name : {"cdn", "versions", "catalog", "cdn-paths"}) {
    std::error_code ec;
    fs::remove_all(root / name, ec);
    if (ec) {
      error = ec.message();
      return false;
    }
  }
  error.clear();
  return true;
}
