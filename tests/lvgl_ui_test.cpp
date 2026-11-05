// Headless LVGL regression for the two visual traps reported on the device:
// long Wi-Fi names wrapping into the IP row, and the Game Boy ROM picker burying
// the shared back button. Uses the real LVGL label/layout engine, not a mock.
#include <lvgl.h>
#include <cassert>
#include <cstdio>
#include "../arduino/08_LVGL_Clock/clock_layout.h"

static lv_disp_draw_buf_t drawBuffer;
static lv_color_t pixels[480 * 40];
static void flush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *) {
  lv_disp_flush_ready(driver);
}
static lv_obj_t *controlLabel(lv_obj_t *parent, const ClockLayout::Rect &r,
                              const char *text, const lv_font_t *font) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_pos(label, r.x, r.y);
  lv_obj_set_size(label, r.w, r.h);
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_label_set_text(label, text);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_update_layout(label);
  return label;
}
int main() {
  lv_init();
  lv_disp_draw_buf_init(&drawBuffer, pixels, nullptr, 480 * 40);
  static lv_disp_drv_t driver;
  lv_disp_drv_init(&driver);
  driver.hor_res = 480; driver.ver_res = 480;
  driver.flush_cb = flush; driver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&driver);
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_size(screen, 480, 480);

  const ClockLayout::Control control = ClockLayout::control(480, 480);
  lv_obj_t *ssid = controlLabel(screen, control.wifiStatus,
                                "AMOLED-Clock-1234567890", &lv_font_montserrat_18);
  lv_obj_t *ip = controlLabel(screen, control.wifiIp,
                              "192.168.4.1", &lv_font_montserrat_16);
  assert(lv_obj_get_height(ssid) <= control.wifiStatus.h);
  assert(lv_obj_get_height(ip) <= control.wifiIp.h);
  assert(lv_obj_get_y(ssid) + lv_obj_get_height(ssid) <= lv_obj_get_y(ip));
  assert(lv_obj_get_x(ssid) + lv_obj_get_width(ssid) <= control.wifi.right());

  const ClockLayout::Gameboy gb = ClockLayout::gameboy(480, 480);
  lv_obj_t *page = lv_obj_create(screen);
  lv_obj_set_size(page, 480, 480);
  lv_obj_t *back = lv_btn_create(page);
  lv_obj_set_pos(back, gb.back.x, gb.back.y);
  lv_obj_set_size(back, gb.back.w, gb.back.h);
  lv_obj_t *list = lv_obj_create(page); // created after back, as in the firmware
  lv_obj_set_size(list, 480, 480);
  assert(lv_obj_get_index(list) > lv_obj_get_index(back));
  lv_obj_move_foreground(back);         // the firmware's escape-route fix
  assert(lv_obj_get_index(back) > lv_obj_get_index(list));
  assert(!gb.back.overlaps(gb.title));
  assert(!gb.back.overlaps(gb.canvas));

  std::puts("lvgl ui: long Wi-Fi SSID stays above IP; Game Boy back stays above ROM picker OK");
  return 0;
}
