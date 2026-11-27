#include "fallback_catalog.h"

#include <iostream>
#include <string>
#include <unordered_set>

int main() {
  constexpr size_t count = sizeof(fallbackProducts) / sizeof(fallbackProducts[0]);
  if (count <= 13) {
    std::cerr << "expanded fallback catalog is missing\n";
    return 1;
  }
  if (std::string(fallbackProducts[0].code) != "d3") return 1;
  std::unordered_set<std::string> seen;
  for (auto const& product : fallbackProducts) {
    if (product.encrypted || !seen.insert(product.code).second) return 1;
    if (!fallbackCdnPaths.count(product.code)) return 1;
  }
  for (char const* code : {"d3t", "anbs"}) {
    if (!seen.count(code)) return 1;
  }
  if (seen.count("scor")) {
    std::cerr << "unverified Sea of Thieves entry is bundled\n";
    return 1;
  }
  return 0;
}
