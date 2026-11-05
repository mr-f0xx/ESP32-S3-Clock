#include <assert.h>
#include <stdio.h>
#include "lvgl.h"
#define FONT(n) extern const lv_font_t lv_font_##n##_48; extern const lv_font_t lv_font_##n##_96;
FONT(digitalnumbers) FONT(rajdhani) FONT(robotomono) FONT(neonlines)
int main(void) {
 const lv_font_t *fonts[] = {&lv_font_digitalnumbers_48,&lv_font_digitalnumbers_96,&lv_font_rajdhani_48,&lv_font_rajdhani_96,&lv_font_robotomono_48,&lv_font_robotomono_96,&lv_font_neonlines_48,&lv_font_neonlines_96};
 for (unsigned i=0; i<8; ++i) {
  lv_font_glyph_dsc_t g; const char *s="-0123456789:";
  for (;*s;++s) { assert(lv_font_get_glyph_dsc(fonts[i], &g, *s, 0)); assert(lv_font_get_glyph_bitmap(fonts[i], *s)); }
  unsigned max=0; for (char d='0';d<='9';++d) { unsigned w=lv_font_get_glyph_width(fonts[i],d,0); if (w>max) max=w; }
  unsigned width = 4 * max + lv_font_get_glyph_width(fonts[i],':',0);
  printf("font %u maximum time width %u px, line %u px\n",i,width,fonts[i]->line_height);
  assert(width<=400);
 }
}
