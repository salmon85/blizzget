#pragma once

#include "downloader.h"

#include <string>
#include <vector>

struct Product {
  std::string code;
  std::string name;
  bool encrypted = false;
};

std::vector<Product> mergeProductCatalog(std::vector<Product> const& fallback,
                                         std::vector<ProductRecord> const& catalog,
                                         bool showEncrypted = false);

std::vector<Product> filterProducts(std::vector<Product> const& products,
                                    std::string const& query);

bool clearBlizzgetCache(std::string const& path, std::string& error);
