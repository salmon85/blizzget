#include "base/checksum.h"

#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
  uint32_t high = 0, low = 0;
  hashlittle2("", 0, &high, &low);
  if (high != 0xDEADBEEF || low != 0xDEADBEEF) return 1;

  constexpr char text[] = "Four score and seven years ago";
  high = 0; low = 0;
  hashlittle2(text, 30, &high, &low);
  if (high != 0x17770551 || low != 0xCE7226E6) {
    std::cerr << std::hex << high << " " << low << "\n";
    return 1;
  }

  high = 0; low = 1;
  hashlittle2(text, 30, &high, &low);
  if (high != 0xE3607CAE || low != 0xBD371DE4) {
    std::cerr << std::hex << high << " " << low << "\n";
    return 1;
  }

  high = 1; low = 0;
  hashlittle2(text, 30, &high, &low);
  if (high != 0xCD628161 || low != 0x6CBEA4B3) {
    std::cerr << std::hex << high << " " << low << "\n";
    return 1;
  }
  return 0;
}
