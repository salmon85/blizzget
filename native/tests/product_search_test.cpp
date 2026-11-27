#include "cache.h"

#include <iostream>

int main() {
  std::vector<Product> products = {
    {"d3", "Diablo III", false},
    {"wow", "World of Warcraft", false},
    {"agent_beta", "Battle.net Agent Beta", true},
    {"anbs", "Diablo Immortal", false},
  };

  auto diablo = filterProducts(products, "  dIaBlO  ");
  if (diablo.size() != 2 || diablo[0].code != "d3" || diablo[1].code != "anbs") return 1;
  auto byCode = filterProducts(products, "AGENT");
  if (byCode.size() != 1 || byCode[0].code != "agent_beta") return 1;
  auto byName = filterProducts(products, "WARCRAFT");
  if (byName.size() != 1 || byName[0].code != "wow") return 1;
  if (!filterProducts(products, "no-such-product").empty()) return 1;
  if (filterProducts(products, "   ").size() != products.size()) return 1;
  return 0;
}
