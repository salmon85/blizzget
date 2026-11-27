#include "cache.h"

#include <filesystem>

namespace fs = std::filesystem;

bool clearBlizzgetCache(std::string const& path, std::string& error) {
  fs::path root(path);
  for (char const* name : {"cdn", "versions"}) {
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
