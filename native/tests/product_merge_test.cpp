#include "cache.h"
#include "downloader.h"

#include <iostream>
#include <vector>

int main() {
  std::vector<Product> fallback = {{"d3", "Diablo III"}, {"wow", "World of Warcraft"}};
  std::vector<ProductRecord> catalog = {
    {"scor", "Sea of Thieves", false},
    {"d3", "Renamed Diablo", false},
    {"anbs", "Diablo Immortal", false},
    {"agent_beta", "Battle.net Agent Beta", true}
  };
  auto products = mergeProductCatalog(fallback, catalog, false);
  if (products.size() != 4) return 1;
  if (products[0].code != "d3" || products[0].name != "Diablo III") return 1;
  if (products[1].code != "wow") return 1;
  if (products[2].code != "anbs" || products[3].code != "scor") return 1;

  auto withEncrypted = mergeProductCatalog(fallback, catalog, true);
  if (withEncrypted.size() != 5) return 1;
  if (withEncrypted[2].code != "agent_beta" || !withEncrypted[2].encrypted) return 1;
  return 0;
}
