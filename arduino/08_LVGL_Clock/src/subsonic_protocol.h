#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure, bounded helpers shared with the native tests.
namespace SubsonicProtocol {
inline bool hexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline size_t urlEncode(const char *input, char *output, size_t capacity) {
  static const char hex[] = "0123456789ABCDEF";
  if (!output || !capacity) return 0;
  size_t used = 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(input ? input : "");
       *p; ++p) {
    const bool unreserved = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                            *p == '.' || *p == '~';
    const size_t needed = unreserved ? 1 : 3;
    if (used + needed >= capacity) break;
    if (unreserved) output[used++] = *p;
    else {
      output[used++] = '%'; output[used++] = hex[*p >> 4]; output[used++] = hex[*p & 15];
    }
  }
  output[used] = 0;
  return used;
}
inline bool validBaseUrl(const char *url) {
  if (!url) return false;
  size_t prefix = 0;
  if (!__builtin_strncmp(url, "http://", 7)) prefix = 7;
  else if (!__builtin_strncmp(url, "https://", 8)) prefix = 8;
  else return false;
  size_t length = 0;
  while (url[length]) ++length;
  if (length <= prefix || length > 240) return false;
  for (size_t i = prefix; i < length; ++i)
    if (static_cast<unsigned char>(url[i]) <= 32 || url[i] == '?' || url[i] == '#') return false;
  return true;
}
}
