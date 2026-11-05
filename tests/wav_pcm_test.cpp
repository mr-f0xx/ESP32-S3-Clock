#include "../arduino/08_LVGL_Clock/src/wav_pcm.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
  using namespace WavPcm;
  assert(supported(1, 1, 16) && supported(1, 2, 24) && supported(3, 2, 32));
  assert(!supported(1, 0, 16) && !supported(1, 3, 16) && !supported(1, 1, 12));
  assert(!supported(3, 1, 64) && !supported(7, 1, 16));
  const uint8_t eight[] = {0, 128, 255};
  assert(decodeSample(eight, 8, 1) == -32768 && decodeSample(eight + 1, 8, 1) == 0);
  assert(decodeSample(eight + 2, 8, 1) == 32512);
  const uint8_t sixteen[] = {0x00, 0x80, 0xff, 0x7f};
  assert(decodeSample(sixteen, 16, 1) == -32768 && decodeSample(sixteen + 2, 16, 1) == 32767);
  const uint8_t twentyFour[] = {0x00, 0x00, 0x80, 0xff, 0xff, 0x7f};
  assert(decodeSample(twentyFour, 24, 1) == -32768 && decodeSample(twentyFour + 3, 24, 1) == 32767);
  const uint8_t thirtyTwo[] = {0x00, 0x00, 0x00, 0x80};
  assert(decodeSample(thirtyTwo, 32, 1) == -32768);
  const float values[] = {-1.0f, 0.0f, 1.0f, 2.0f};
  for (float value : values) {
    uint8_t bytes[4]; memcpy(bytes, &value, 4);
    const int16_t sample = decodeSample(bytes, 32, 3);
    if (value == -1.0f) assert(sample == -32767);
    else if (value == 0.0f) assert(sample == 0);
    else assert(sample == 32767);
  }
  Info info;
  const uint8_t data[] = {'d','a','t','a', 4,0,0,0};
  assert(acceptChunk(info, data, 12, 24) && info.dataOffset == 20 && info.dataBytes == 4);
  assert(!acceptChunk(info, data, 12, 23));
  assert(!acceptChunk(info, data, 20, 24));
  puts("wav PCM: 8/16/24/32-bit and float decoding, format rejection, chunk bounds OK");
}
