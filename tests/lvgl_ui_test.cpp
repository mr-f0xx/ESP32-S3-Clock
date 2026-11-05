// Headless LVGL regression for device-visible layout traps: long Wi-Fi names
// wrapping into the IP row, the Game Boy ROM picker burying the shared back
// button, and control-centre text painting high inside its tile because LVGL
// labels anchor the first line to the top of a fixed-height object box.
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
  const ClockLayout::Rect line = ClockLayout::centeredLineRect(
    r, static_cast<int16_t>(lv_font_get_line_height(font)));
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(label, line.x, line.y);
  lv_obj_set_size(label, line.w, line.h);
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_label_set_text(label, text);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_update_layout(label);
  return label;
}
static void assertCenteredInRow(lv_obj_t *label, const ClockLayout::Rect &row) {
  const int labelCenter = lv_obj_get_y(label) + lv_obj_get_height(label) / 2;
  const int rowCenter = row.y + row.h / 2;
  assert(labelCenter >= rowCenter - 1 && labelCenter <= rowCenter + 1);
  assert(lv_obj_get_height(label) <= row.h);
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
  assertCenteredInRow(ssid, control.wifiStatus);
  assertCenteredInRow(ip, control.wifiIp);

  lv_obj_t *battery = controlLabel(screen, control.battery, "94%", &lv_font_montserrat_22);
  lv_obj_t *rotation = controlLabel(screen, control.rotationStatus, "AUTO", &lv_font_montserrat_18);
  assertCenteredInRow(battery, control.battery);
  assertCenteredInRow(rotation, control.rotationStatus);
  // The battery/lock icons are centered in their own 40 px rects; the text
  // centerline must match the icon centerline, not sit one line-box above it.
  const int batteryIconCenter = control.batteryIcon.y + control.batteryIcon.h / 2;
  const int batteryTextCenter = lv_obj_get_y(battery) + lv_obj_get_height(battery) / 2;
  assert(batteryTextCenter >= batteryIconCenter - 1 && batteryTextCenter <= batteryIconCenter + 1);
  const int lockIconCenter = control.rotationIcon.y + control.rotationIcon.h / 2;
  const int lockTextCenter = lv_obj_get_y(rotation) + lv_obj_get_height(rotation) / 2;
  assert(lockTextCenter >= lockIconCenter - 1 && lockTextCenter <= lockIconCenter + 1);

  // The five top pills use a one-line label centered inside the button, not a
  // tall label box whose glyphs render at the top of the tile.
  lv_obj_t *pill = lv_btn_create(screen);
  const ClockLayout::Rect pillRect = control.settings[0];
  lv_obj_set_pos(pill, pillRect.x, pillRect.y);
  lv_obj_set_size(pill, pillRect.w, pillRect.h);
  lv_obj_set_style_pad_all(pill, 2, 0);
  lv_obj_t *pillLabel = lv_label_create(pill);
  lv_obj_set_width(pillLabel, pillRect.w - 8);
  lv_obj_set_height(pillLabel, lv_font_get_line_height(&lv_font_montserrat_16));
  lv_obj_set_style_text_font(pillLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(pillLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(pillLabel, LV_LABEL_LONG_DOT);
  lv_label_set_text(pillLabel, "Theme GLW");
  lv_obj_center(pillLabel);
  lv_obj_update_layout(pill);
  lv_area_t pillArea, pillLabelArea;
  lv_obj_get_coords(pill, &pillArea);
  lv_obj_get_coords(pillLabel, &pillLabelArea);
  const int pillCenter = pillArea.y1 + lv_area_get_height(&pillArea) / 2;
  const int pillTextCenter = pillLabelArea.y1 + lv_area_get_height(&pillLabelArea) / 2;
  assert(pillTextCenter >= pillCenter - 1 && pillTextCenter <= pillCenter + 1);

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

  std::puts("lvgl ui: Wi-Fi rows, Game Boy back, pill/battery/lock vertical centering OK");
  return 0;
}
