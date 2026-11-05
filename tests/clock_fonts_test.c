#include <assert.h>
#include <stdio.h>
#include "lvgl.h"
extern const lv_font_t lv_font_kameron_48; extern const lv_font_t lv_font_kameron_72; extern const lv_font_t lv_font_kameron_96;
extern const lv_font_t lv_font_silkscreen_48; extern const lv_font_t lv_font_silkscreen_72; extern const lv_font_t lv_font_silkscreen_96;
extern const lv_font_t lv_font_dotgothic16_48; extern const lv_font_t lv_font_dotgothic16_72; extern const lv_font_t lv_font_dotgothic16_96;
extern const lv_font_t lv_font_orbitron_48; extern const lv_font_t lv_font_orbitron_72; extern const lv_font_t lv_font_orbitron_96;
extern const lv_font_t lv_font_audiowide_48; extern const lv_font_t lv_font_audiowide_72; extern const lv_font_t lv_font_audiowide_96;
extern const lv_font_t lv_font_cinzel_48; extern const lv_font_t lv_font_cinzel_72; extern const lv_font_t lv_font_cinzel_96;
extern const lv_font_t lv_font_bebasneue_48; extern const lv_font_t lv_font_bebasneue_72; extern const lv_font_t lv_font_bebasneue_96;
extern const lv_font_t lv_font_digitalnumbers_48; extern const lv_font_t lv_font_digitalnumbers_72; extern const lv_font_t lv_font_digitalnumbers_96;
extern const lv_font_t lv_font_rajdhani_48; extern const lv_font_t lv_font_rajdhani_72; extern const lv_font_t lv_font_rajdhani_96;
extern const lv_font_t lv_font_robotomono_48; extern const lv_font_t lv_font_robotomono_72; extern const lv_font_t lv_font_robotomono_96;
extern const lv_font_t lv_font_neonlines_48; extern const lv_font_t lv_font_neonlines_72; extern const lv_font_t lv_font_neonlines_96;
extern const lv_font_t lv_font_montserrat_48; extern const lv_font_t lv_font_montserrat_time_72; extern const lv_font_t lv_font_montserrat_time_96;
int main(void) {
 const lv_font_t *fonts[] = {
  &lv_font_kameron_48,&lv_font_kameron_72,&lv_font_kameron_96,
  &lv_font_silkscreen_48,&lv_font_silkscreen_72,&lv_font_silkscreen_96,
  &lv_font_dotgothic16_48,&lv_font_dotgothic16_72,&lv_font_dotgothic16_96,
  &lv_font_orbitron_48,&lv_font_orbitron_72,&lv_font_orbitron_96,
  &lv_font_audiowide_48,&lv_font_audiowide_72,&lv_font_audiowide_96,
  &lv_font_cinzel_48,&lv_font_cinzel_72,&lv_font_cinzel_96,
  &lv_font_bebasneue_48,&lv_font_bebasneue_72,&lv_font_bebasneue_96,
  &lv_font_digitalnumbers_48,&lv_font_digitalnumbers_72,&lv_font_digitalnumbers_96,
  &lv_font_rajdhani_48,&lv_font_rajdhani_72,&lv_font_rajdhani_96,
  &lv_font_robotomono_48,&lv_font_robotomono_72,&lv_font_robotomono_96,
  &lv_font_neonlines_48,&lv_font_neonlines_72,&lv_font_neonlines_96,
  &lv_font_montserrat_48,&lv_font_montserrat_time_72,&lv_font_montserrat_time_96,
 };
 for (unsigned i=0; i<sizeof(fonts)/sizeof(fonts[0]); ++i) {
  lv_font_glyph_dsc_t g; const char *s="-0123456789:";
  for (;*s;++s) { assert(lv_font_get_glyph_dsc(fonts[i], &g, *s, 0)); assert(lv_font_get_glyph_bitmap(fonts[i], *s)); }
  unsigned max=0; for (char d='0';d<='9';++d) { unsigned w=lv_font_get_glyph_width(fonts[i],d,0); if (w>max) max=w; }
  unsigned width = 4 * max + lv_font_get_glyph_width(fonts[i],':',0);
  printf("font %u maximum time width %u px, line %u px\n",i,width,fonts[i]->line_height);
  assert(width<=448);
  assert(fonts[i]->line_height<=120);
 }
 return 0;
}
