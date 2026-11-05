#include "../arduino/08_LVGL_Clock/src/subsonic_protocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
  char out[64];
  assert(SubsonicProtocol::urlEncode("a b&c+/~", out, sizeof(out)) == 16);
  assert(!strcmp(out, "a%20b%26c%2B%2F~"));
  assert(SubsonicProtocol::urlEncode("é", out, sizeof(out)) == 6);
  assert(!strcmp(out, "%C3%A9"));
  assert(SubsonicProtocol::urlEncode("abc", out, 3) == 2 && !strcmp(out, "ab"));
  assert(SubsonicProtocol::urlEncode("a b", out, 4) == 1 && !strcmp(out, "a"));
  assert(SubsonicProtocol::urlEncode(nullptr, out, sizeof(out)) == 0);
  assert(SubsonicProtocol::validBaseUrl("http://192.168.1.2:4533"));
  assert(SubsonicProtocol::validBaseUrl("https://music.example.com/gonic"));
  assert(!SubsonicProtocol::validBaseUrl("music.example.com"));
  assert(!SubsonicProtocol::validBaseUrl("https://"));
  assert(!SubsonicProtocol::validBaseUrl("https://host/?x=1"));
  assert(!SubsonicProtocol::validBaseUrl("https://host/#fragment"));
  assert(!SubsonicProtocol::validBaseUrl("ftp://host"));
  puts("subsonic protocol: UTF-8 percent encoding, truncation safety, base URL validation OK");
}
