// Lightweight Tabler-style vector icons drawn with native LVGL objects.
// Geometry follows Tabler's 24x24, 2 px, round-cap outline convention.
// Reference: https://tabler.io/icons

enum TablerIconKind : uint8_t {
  TABLER_ICON_MUSIC = 0,
  TABLER_ICON_PLAY,
  TABLER_ICON_PAUSE,
  TABLER_ICON_SKIP_BACK,
  TABLER_ICON_SKIP_FORWARD,
  TABLER_ICON_PLAYLIST,
  TABLER_ICON_ADJUSTMENTS,
  TABLER_ICON_VOLUME,
  TABLER_ICON_SUN,
  TABLER_ICON_CLOUD,
  TABLER_ICON_CLOUD_RAIN,
  TABLER_ICON_PHOTO,
  TABLER_ICON_BOLT,
  TABLER_ICON_X,
  TABLER_ICON_REPEAT,
  TABLER_ICON_REPEAT_ONCE,
  TABLER_ICON_SHUFFLE,
  TABLER_ICON_ALARM,
  TABLER_ICON_ALARM_OFF,
  TABLER_ICON_CHEVRON_LEFT,
  TABLER_ICON_CHEVRON_RIGHT,
  TABLER_ICON_AUDIOBOOK,
  TABLER_ICON_ROTATE_LEFT,
  TABLER_ICON_RADIO,
  TABLER_ICON_STOP
};

void freeTablerLinePoints(lv_event_t *event) {
  void *points = lv_event_get_user_data(event);
  if (points) lv_mem_free(points);
}

lv_coord_t tablerScale(lv_coord_t coordinate, lv_coord_t size) {
  return static_cast<lv_coord_t>((coordinate * size + 12) / 24);
}

lv_obj_t *createTablerLayer(lv_obj_t *parent, lv_coord_t size) {
  lv_obj_t *layer = lv_obj_create(parent);
  lv_obj_remove_style_all(layer);
  lv_obj_set_size(layer, size, size);
  lv_obj_center(layer);
  lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return layer;
}

lv_obj_t *addTablerPath(lv_obj_t *parent, const lv_point_t *source,
                        uint16_t count, lv_coord_t size, lv_color_t color,
                        lv_coord_t stroke = 0) {
  if (!source || count < 2) return nullptr;
  lv_point_t *points = static_cast<lv_point_t *>(
    lv_mem_alloc(sizeof(lv_point_t) * count));
  if (!points) return nullptr;
  for (uint16_t i = 0; i < count; ++i) {
    points[i].x = tablerScale(source[i].x, size);
    points[i].y = tablerScale(source[i].y, size);
  }
  lv_obj_t *line = lv_line_create(parent);
  lv_line_set_points(line, points, count);
  lv_obj_set_style_line_width(line, stroke ? stroke : max<lv_coord_t>(2, size / 12), 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  lv_obj_set_style_line_color(line, color, 0);
  lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(line, freeTablerLinePoints, LV_EVENT_DELETE, points);
  return line;
}

lv_obj_t *addTablerOutline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_coord_t width, lv_coord_t height,
                           lv_coord_t radius, lv_coord_t size,
                           lv_color_t color) {
  lv_obj_t *outline = lv_obj_create(parent);
  lv_obj_remove_style_all(outline);
  lv_obj_set_pos(outline, tablerScale(x, size), tablerScale(y, size));
  lv_obj_set_size(outline, tablerScale(width, size), tablerScale(height, size));
  lv_obj_set_style_radius(outline,
                          radius == LV_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE
                                                     : tablerScale(radius, size), 0);
  lv_obj_set_style_border_width(outline, max<lv_coord_t>(2, size / 12), 0);
  lv_obj_set_style_border_color(outline, color, 0);
  lv_obj_set_style_border_opa(outline, LV_OPA_COVER, 0);
  lv_obj_clear_flag(outline, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return outline;
}

lv_obj_t *addTablerDot(lv_obj_t *parent, lv_coord_t centerX,
                       lv_coord_t centerY, lv_coord_t diameter,
                       lv_coord_t size, lv_color_t color) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  const lv_coord_t scaledDiameter = tablerScale(diameter, size);
  lv_obj_set_size(dot, scaledDiameter, scaledDiameter);
  lv_obj_set_pos(dot, tablerScale(centerX, size) - scaledDiameter / 2,
                 tablerScale(centerY, size) - scaledDiameter / 2);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, color, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return dot;
}

void addTablerSegment(lv_obj_t *parent, lv_coord_t x1, lv_coord_t y1,
                      lv_coord_t x2, lv_coord_t y2, lv_coord_t size,
                      lv_color_t color) {
  const lv_point_t points[] = {{x1, y1}, {x2, y2}};
  addTablerPath(parent, points, 2, size, color);
}

lv_obj_t *createTablerIcon(lv_obj_t *parent, TablerIconKind kind,
                           lv_coord_t size, lv_color_t color = lv_color_white()) {
  lv_obj_t *layer = createTablerLayer(parent, size);
  switch (kind) {
    case TABLER_ICON_MUSIC: {
      const lv_point_t stem[] = {{9, 18}, {9, 5}, {20, 3}, {20, 16}};
      const lv_point_t bar[] = {{9, 9}, {20, 7}};
      addTablerPath(layer, stem, 4, size, color);
      addTablerPath(layer, bar, 2, size, color);
      addTablerOutline(layer, 4, 16, 5, 4, LV_RADIUS_CIRCLE, size, color);
      addTablerOutline(layer, 15, 14, 5, 4, LV_RADIUS_CIRCLE, size, color);
      break;
    }
    case TABLER_ICON_PLAY: {
      const lv_point_t path[] = {{7, 4}, {20, 12}, {7, 20}, {7, 4}};
      addTablerPath(layer, path, 4, size, color);
      break;
    }
    case TABLER_ICON_PAUSE:
      addTablerOutline(layer, 6, 4, 4, 16, 1, size, color);
      addTablerOutline(layer, 14, 4, 4, 16, 1, size, color);
      break;
    case TABLER_ICON_SKIP_BACK: {
      addTablerSegment(layer, 5, 5, 5, 19, size, color);
      const lv_point_t path[] = {{19, 5}, {8, 12}, {19, 19}, {19, 5}};
      addTablerPath(layer, path, 4, size, color);
      break;
    }
    case TABLER_ICON_SKIP_FORWARD: {
      addTablerSegment(layer, 19, 5, 19, 19, size, color);
      const lv_point_t path[] = {{5, 5}, {16, 12}, {5, 19}, {5, 5}};
      addTablerPath(layer, path, 4, size, color);
      break;
    }
    case TABLER_ICON_PLAYLIST:
      addTablerDot(layer, 4, 6, 2, size, color);
      addTablerDot(layer, 4, 12, 2, size, color);
      addTablerDot(layer, 4, 18, 2, size, color);
      addTablerSegment(layer, 8, 6, 21, 6, size, color);
      addTablerSegment(layer, 8, 12, 21, 12, size, color);
      addTablerSegment(layer, 8, 18, 21, 18, size, color);
      break;
    case TABLER_ICON_ADJUSTMENTS:
      addTablerSegment(layer, 6, 4, 6, 8, size, color);
      addTablerSegment(layer, 6, 14, 6, 20, size, color);
      addTablerSegment(layer, 12, 4, 12, 12, size, color);
      addTablerSegment(layer, 12, 18, 12, 20, size, color);
      addTablerSegment(layer, 18, 4, 18, 6, size, color);
      addTablerSegment(layer, 18, 12, 18, 20, size, color);
      addTablerOutline(layer, 4, 8, 4, 6, LV_RADIUS_CIRCLE, size, color);
      addTablerOutline(layer, 10, 12, 4, 6, LV_RADIUS_CIRCLE, size, color);
      addTablerOutline(layer, 16, 6, 4, 6, LV_RADIUS_CIRCLE, size, color);
      break;
    case TABLER_ICON_VOLUME: {
      const lv_point_t speaker[] = {{4, 10}, {8, 10}, {13, 5}, {13, 19},
                                    {8, 14}, {4, 14}, {4, 10}};
      const lv_point_t wave1[] = {{16, 9}, {18, 12}, {16, 15}};
      const lv_point_t wave2[] = {{19, 6}, {22, 12}, {19, 18}};
      addTablerPath(layer, speaker, 7, size, color);
      addTablerPath(layer, wave1, 3, size, color);
      addTablerPath(layer, wave2, 3, size, color);
      break;
    }
    case TABLER_ICON_SUN:
      addTablerOutline(layer, 8, 8, 8, 8, LV_RADIUS_CIRCLE, size, color);
      addTablerSegment(layer, 12, 2, 12, 5, size, color);
      addTablerSegment(layer, 12, 19, 12, 22, size, color);
      addTablerSegment(layer, 2, 12, 5, 12, size, color);
      addTablerSegment(layer, 19, 12, 22, 12, size, color);
      addTablerSegment(layer, 4, 4, 7, 7, size, color);
      addTablerSegment(layer, 17, 17, 20, 20, size, color);
      addTablerSegment(layer, 4, 20, 7, 17, size, color);
      addTablerSegment(layer, 17, 7, 20, 4, size, color);
      break;
    case TABLER_ICON_CLOUD: {
      // Tabler cloud.svg projected to a compact round-stroke polyline.
      const lv_point_t cloud[] = {
        {7, 18}, {5, 18}, {3, 16}, {3, 13}, {5, 11}, {8, 11},
        {9, 7}, {12, 5}, {16, 6}, {18, 10}, {21, 11}, {22, 14},
        {21, 17}, {19, 18}, {7, 18}
      };
      addTablerPath(layer, cloud, 15, size, color);
      break;
    }
    case TABLER_ICON_CLOUD_RAIN: {
      const lv_point_t cloud[] = {
        {7, 15}, {5, 15}, {3, 13}, {3, 10}, {5, 8}, {8, 8},
        {9, 5}, {12, 3}, {16, 4}, {18, 8}, {21, 9}, {22, 12},
        {21, 14}, {19, 15}, {7, 15}
      };
      addTablerPath(layer, cloud, 15, size, color);
      addTablerSegment(layer, 8, 18, 7, 21, size, color);
      addTablerSegment(layer, 13, 18, 12, 21, size, color);
      addTablerSegment(layer, 18, 18, 17, 21, size, color);
      break;
    }
    case TABLER_ICON_PHOTO: {
      addTablerOutline(layer, 3, 5, 18, 14, 2, size, color);
      addTablerDot(layer, 9, 10, 3, size, color);
      const lv_point_t hills[] = {{4, 17}, {9, 13}, {12, 16}, {15, 13}, {20, 17}};
      addTablerPath(layer, hills, 5, size, color);
      break;
    }
    case TABLER_ICON_BOLT: {
      const lv_point_t path[] = {{13, 2}, {5, 14}, {11, 14},
                                 {10, 22}, {19, 9}, {13, 9}, {13, 2}};
      addTablerPath(layer, path, 7, size, color);
      break;
    }
    case TABLER_ICON_X:
      addTablerSegment(layer, 6, 6, 18, 18, size, color);
      addTablerSegment(layer, 18, 6, 6, 18, size, color);
      break;
    case TABLER_ICON_REPEAT: {
      const lv_point_t top[] = {{4, 12}, {4, 9}, {7, 6}, {20, 6}, {17, 3}};
      const lv_point_t bottom[] = {{20, 12}, {20, 15}, {17, 18}, {4, 18}, {7, 21}};
      addTablerPath(layer, top, 5, size, color);
      addTablerPath(layer, bottom, 5, size, color);
      break;
    }
    case TABLER_ICON_REPEAT_ONCE: {
      const lv_point_t top[] = {{4, 12}, {4, 9}, {7, 6}, {20, 6}, {17, 3}};
      const lv_point_t bottom[] = {{20, 12}, {20, 15}, {17, 18}, {4, 18}, {7, 21}};
      addTablerPath(layer, top, 5, size, color);
      addTablerPath(layer, bottom, 5, size, color);
      addTablerSegment(layer, 11, 10, 13, 9, size, color);
      addTablerSegment(layer, 13, 9, 13, 15, size, color);
      break;
    }
    case TABLER_ICON_SHUFFLE: {
      const lv_point_t upper[] = {{4, 7}, {7, 7}, {17, 17}, {21, 17}};
      const lv_point_t lower[] = {{4, 17}, {7, 17}, {10, 14}};
      const lv_point_t cross[] = {{14, 10}, {17, 7}, {21, 7}};
      const lv_point_t topArrow[] = {{18, 4}, {21, 7}, {18, 10}};
      const lv_point_t bottomArrow[] = {{18, 14}, {21, 17}, {18, 20}};
      addTablerPath(layer, upper, 4, size, color);
      addTablerPath(layer, lower, 3, size, color);
      addTablerPath(layer, cross, 3, size, color);
      addTablerPath(layer, topArrow, 3, size, color);
      addTablerPath(layer, bottomArrow, 3, size, color);
      break;
    }
    case TABLER_ICON_ALARM: {
      // https://tabler.io/icons/icon/alarm
      addTablerOutline(layer, 5, 6, 14, 14, LV_RADIUS_CIRCLE, size, color);
      const lv_point_t hands[] = {{12, 10}, {12, 13}, {14, 13}};
      addTablerPath(layer, hands, 3, size, color);
      addTablerSegment(layer, 7, 4, 4, 6, size, color);
      addTablerSegment(layer, 17, 4, 20, 6, size, color);
      break;
    }
    case TABLER_ICON_ALARM_OFF: {
      // Polyline projection of Tabler alarm-off.svg's two interrupted arcs.
      const lv_point_t lowerArc[] = {
        {8, 8}, {6, 10}, {5, 13}, {6, 16}, {8, 18},
        {11, 20}, {14, 20}, {17, 18}
      };
      const lv_point_t upperArc[] = {
        {19, 15}, {19, 12}, {18, 9}, {16, 7}, {13, 6}, {10, 6}
      };
      const lv_point_t hands[] = {{12, 12}, {12, 13}, {13, 13}};
      addTablerPath(layer, lowerArc, 8, size, color);
      addTablerPath(layer, upperArc, 6, size, color);
      addTablerPath(layer, hands, 3, size, color);
      addTablerSegment(layer, 5, 5, 4, 6, size, color);
      addTablerSegment(layer, 17, 4, 20, 6, size, color);
      addTablerSegment(layer, 3, 3, 21, 21, size, color);
      break;
    }
    case TABLER_ICON_CHEVRON_LEFT: {
      const lv_point_t path[] = {{15, 6}, {9, 12}, {15, 18}};
      addTablerPath(layer, path, 3, size, color);
      break;
    }
    case TABLER_ICON_CHEVRON_RIGHT: {
      const lv_point_t path[] = {{9, 6}, {15, 12}, {9, 18}};
      addTablerPath(layer, path, 3, size, color);
      break;
    }
    case TABLER_ICON_AUDIOBOOK: {
      // Open book projected from Tabler book.svg: two shallow page arcs top
      // and bottom joined by three spines. Reads clearly as an audiobook.
      const lv_point_t topArc[] = {{3, 6}, {7, 5}, {12, 6}, {17, 5}, {21, 6}};
      const lv_point_t bottomArc[] = {{3, 19}, {7, 18}, {12, 19}, {17, 18}, {21, 19}};
      addTablerPath(layer, topArc, 5, size, color);
      addTablerPath(layer, bottomArc, 5, size, color);
      addTablerSegment(layer, 3, 6, 3, 19, size, color);
      addTablerSegment(layer, 12, 6, 12, 19, size, color);
      addTablerSegment(layer, 21, 6, 21, 19, size, color);
      break;
    }
    case TABLER_ICON_ROTATE_LEFT: {
      // Counter-clockwise refresh arrow (Tabler rotate-left style) used for
      // "restart from the beginning" and the -30 s rewind key.
      const lv_point_t arc[] = {
        {5, 9}, {7, 6}, {10, 4}, {14, 4}, {17, 6},
        {19, 9}, {20, 12}, {19, 15}, {17, 18}, {14, 20},
        {10, 20}, {7, 18}, {5, 15}
      };
      const lv_point_t arrow[] = {{5, 4}, {5, 9}, {10, 9}};
      addTablerPath(layer, arc, 13, size, color);
      addTablerPath(layer, arrow, 3, size, color);
      break;
    }
    case TABLER_ICON_RADIO: {
      // Classic transistor radio (Tabler radio style): diagonal antenna, a
      // rounded body, a circular tuning dial and a short speaker grille.
      addTablerSegment(layer, 17, 3, 9, 8, size, color);
      addTablerOutline(layer, 3, 8, 18, 12, 2, size, color);
      addTablerOutline(layer, 14, 12, 5, 5, LV_RADIUS_CIRCLE, size, color);
      addTablerSegment(layer, 6, 12, 6, 16, size, color);
      addTablerSegment(layer, 9, 12, 9, 16, size, color);
      break;
    }
    case TABLER_ICON_STOP: {
      // Filled-look stop square (outlined to match the 2 px icon convention).
      addTablerOutline(layer, 6, 6, 12, 12, 2, size, color);
      break;
    }
  }
  return layer;
}

void setTablerIconColor(lv_obj_t *object, lv_color_t color) {
  if (!object) return;
  lv_obj_set_style_line_color(object, color, 0);
  lv_obj_set_style_border_color(object, color, 0);
  lv_obj_set_style_bg_color(object, color, 0);
  const uint32_t childCount = lv_obj_get_child_cnt(object);
  for (uint32_t i = 0; i < childCount; ++i) {
    setTablerIconColor(lv_obj_get_child(object, i), color);
  }
}
