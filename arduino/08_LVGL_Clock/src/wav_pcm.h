#pragma once
#include <stdint.h>
#include <stddef.h>
namespace WavPcm {
struct Info {
  uint32_t sampleRate = 0, dataOffset = 0, dataBytes = 0;
  uint16_t format = 0, channels = 0, bits = 0;
  bool valid = false;
};
inline uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t *p) { return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; }
// Call with the first 12 bytes, then each 8-byte chunk header. Chunk positions
// start at 12. Reject truncated chunks and unsupported non-PCM encodings.
inline bool acceptChunk(Info &info, const uint8_t *header, uint32_t position, uint32_t fileSize) {
  if (position + 8 > fileSize) return false;
  const uint32_t size = u32(header + 4);
  if (size > fileSize - position - 8) return false;
  if (header[0] == 'f' && header[1] == 'm' && header[2] == 't' && header[3] == ' ') {
    if (size < 16) return false;
    return true; // Caller parses the actual fmt payload separately.
  }
  if (header[0] == 'd' && header[1] == 'a' && header[2] == 't' && header[3] == 'a') {
    info.dataOffset = position + 8; info.dataBytes = size;
  }
  return true;
}
inline bool supported(uint16_t format, uint16_t channels, uint16_t bits) {
  return (format == 1 || format == 3) && channels >= 1 && channels <= 2 &&
         ((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (format == 3 && bits == 32));
}
inline int16_t decodeSample(const uint8_t *p, uint16_t bits, uint16_t format) {
  int32_t value = 0;
  if (format == 3 && bits == 32) {
    const uint32_t raw = u32(p); float f; __builtin_memcpy(&f, &raw, 4);
    if (!(f >= -1.0f && f <= 1.0f)) return f < 0 ? -32768 : 32767;
    return static_cast<int16_t>(f * 32767.0f);
  }
  if (bits == 8) return static_cast<int16_t>((int32_t(p[0]) - 128) * 256);
  if (bits == 16) return static_cast<int16_t>(u16(p));
  if (bits == 24) {
    value = int32_t(p[0]) | int32_t(p[1]) << 8 | int32_t(p[2]) << 16;
    if (value & 0x800000) value |= ~0xFFFFFF;
    return static_cast<int16_t>(value >> 8);
  }
  if (bits == 32) return static_cast<int16_t>(u32(p) >> 16);
  return 0;
}
}
