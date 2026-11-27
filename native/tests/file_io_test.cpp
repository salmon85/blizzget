#include "base/file.h"
#include "base/error.h"

#include <iostream>

int main() {
  char byte = 0;
  File invalid;
  if (invalid.read(&byte, 1) != 0 || invalid.getc() != EOF || invalid.size() != 0) {
    std::cerr << "invalid file reads are not safe\n";
    return 1;
  }
  try {
    invalid.write("x", 1);
    std::cerr << "write to invalid file did not fail\n";
    return 1;
  } catch (Exception const&) {
  }

  File full("/dev/full", File::REWRITE);
  if (!full) {
    std::cerr << "could not open /dev/full\n";
    return 1;
  }
  MemoryFile source;
  source.write(std::string(1024 * 1024, 'x').data(), 1024 * 1024);
  source.seek(0);
  try {
    full.copy(source);
    std::cerr << "copy to full device did not fail\n";
    return 1;
  } catch (Exception const&) {
  }
  return 0;
}
