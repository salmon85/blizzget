#include "path.h"
#include "error.h"

#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std;

string path::name(string const& value) {
  return fs::path(value).filename().string();
}

string path::title(string const& value) {
  return fs::path(value).stem().string();
}

string path::path(string const& value) {
  return fs::path(value).parent_path().string();
}

string path::ext(string const& value) {
  return fs::path(value).extension().string();
}

void path::create(string const& value) {
  if (value.empty()) return;
  std::error_code ec;
  fs::create_directories(value, ec);
  if (ec) throw Exception("failed to create directory: ") << value;
}

std::string path::root() {
  static std::string root_;
  if (root_.empty()) {
#ifdef _WIN32
    vector<char> buffer(32768);
    DWORD length = GetModuleFileNameA(nullptr, buffer.data(), buffer.size());
    root_ = fs::path(string(buffer.data(), length)).parent_path().string();
#else
    root_ = fs::read_symlink("/proc/self/exe").parent_path().string();
#endif
  }
  return root_;
}

string operator/(string const& lhs, string const& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  return (fs::path(lhs) / fs::path(rhs)).string();
}
