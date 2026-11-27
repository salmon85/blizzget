#include "error.h"
#include "types.h"
#include <stdarg.h>

Exception::Exception(char const* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  int len = vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  std::string dst(static_cast<size_t>(len) + 1, '\0');
  vsnprintf(&dst[0], dst.size(), fmt, ap);
  va_end(ap);
  dst.resize(len);
  buf_.str(dst);
}
