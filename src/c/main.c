#include <pebble.h>

// Neue Abfrage, solange die App offen ist
#define REFRESH_SECONDS (3 * SECONDS_PER_MINUTE)
// Danach automatische Abfragen aussetzen, schont das TomTom-Kontingent
#define AUTO_REFRESH_LIMIT (30 * SECONDS_PER_MINUTE)
// Ab diesem Alter wird der Stand farbig markiert
#define STALE_AFTER (20 * SECONDS_PER_MINUTE)
// Ohne Antwort vom Handy gilt die Abfrage als gescheitert
#define REQUEST_TIMEOUT_MS 45000

#define PERSIST_KEY_DATA_OUT 1
#define PERSIST_KEY_DATA_BACK 2
#define PERSIST_KEY_SWITCH 3
#define PERSIST_KEY_LABEL_OUT 4
#define PERSIST_KEY_LABEL_BACK 5

// Ab dieser Uhrzeit (Minuten nach Mitternacht) zeigt die App beim Öffnen den Rückweg
#define DEFAULT_SWITCH_MINUTES (12 * 60)

typedef enum {
  DIRECTION_OUT = 0,
  DIRECTION_BACK = 1,
} Direction;

typedef struct {
  int32_t minutes;   // Fahrzeit mit Verkehr, -1 = noch keine Daten
  int32_t delay;     // Verzögerung durch Stau in Sekunden
  int32_t distance;  // Strecke in Metern
  int32_t updated;   // Zeitpunkt der Abfrage bei TomTom
  int8_t trend;      // -1 kürzer, 0 gleich, 1 länger als die vorige Abfrage
} RouteData;

static Window *s_window;
static Layer *s_canvas;
static GPath *s_arrow_up;
static GPath *s_arrow_down;
static AppTimer *s_timeout;

static RouteData s_routes[2] = { { .minutes = -1 }, { .minutes = -1 } };
static const uint32_t PERSIST_KEYS[2] = { PERSIST_KEY_DATA_OUT, PERSIST_KEY_DATA_BACK };
static Direction s_direction;
static bool s_direction_chosen;  // per UP/DOWN gewählt, die Uhrzeit gilt dann nicht mehr
static int32_t s_switch_minutes = DEFAULT_SWITCH_MINUTES;
static RouteData *s_data = &s_routes[DIRECTION_OUT];  // die angezeigte Richtung
static char s_status[48];
// Bezeichnungen der Richtungen aus den Einstellungen
static char s_labels[2][24] = { "Zur Arbeit", "Nach Hause" };
static const uint32_t LABEL_KEYS[2] = { PERSIST_KEY_LABEL_OUT, PERSIST_KEY_LABEL_BACK };
static bool s_loading;
static bool s_paused;
static time_t s_session_start;
static time_t s_last_request;

static GPoint s_up_points[] = { { 0, -10 }, { 10, 6 }, { -10, 6 } };
static GPoint s_down_points[] = { { 0, 10 }, { 10, -6 }, { -10, -6 } };
static const GPathInfo ARROW_UP_INFO = { .num_points = 3, .points = s_up_points };
static const GPathInfo ARROW_DOWN_INFO = { .num_points = 3, .points = s_down_points };

// --- Richtung ---------------------------------------------------------------

static void set_direction(Direction direction) {
  s_direction = direction;
  s_data = &s_routes[direction];
}

static Direction direction_for_now(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int minutes = t->tm_hour * 60 + t->tm_min;
  return minutes >= s_switch_minutes ? DIRECTION_BACK : DIRECTION_OUT;
}

// --- Verbindung zum Handy ---------------------------------------------------

static void set_status(const char *text) {
  strncpy(s_status, text, sizeof(s_status) - 1);
  s_status[sizeof(s_status) - 1] = '\0';
}

static void stop_loading(void) {
  s_loading = false;
  if (s_timeout) {
    app_timer_cancel(s_timeout);
    s_timeout = NULL;
  }
}

static void timeout_callback(void *context) {
  s_timeout = NULL;
  if (s_loading) {
    s_loading = false;
    set_status("Keine Antwort vom Handy");
    layer_mark_dirty(s_canvas);
  }
}

static void start_timeout(void) {
  if (s_timeout) {
    app_timer_cancel(s_timeout);
  }
  s_timeout = app_timer_register(REQUEST_TIMEOUT_MS, timeout_callback, NULL);
}

static void request_refresh(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
  dict_write_uint8(iter, MESSAGE_KEY_DIRECTION, s_direction);
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_loading = true;
    s_last_request = time(NULL);
    start_timeout();
  }
  layer_mark_dirty(s_canvas);
}

static void store_route(Direction direction, DictionaryIterator *iter) {
  Tuple *minutes = dict_find(iter, MESSAGE_KEY_MINUTES);
  Tuple *updated = dict_find(iter, MESSAGE_KEY_UPDATED);
  if (!minutes || !updated) {
    return;
  }
  Tuple *delay = dict_find(iter, MESSAGE_KEY_DELAY);
  Tuple *distance = dict_find(iter, MESSAGE_KEY_DISTANCE);
  RouteData *route = &s_routes[direction];

  int32_t m = minutes->value->int32;
  int32_t u = updated->value->int32;
  // Trend nur bei einem wirklich neuen Messwert ändern
  if (u != route->updated) {
    if (route->minutes < 0 || m == route->minutes) {
      route->trend = 0;
    } else {
      route->trend = m > route->minutes ? 1 : -1;
    }
  }
  route->minutes = m;
  route->updated = u;
  route->delay = delay ? delay->value->int32 : 0;
  route->distance = distance ? distance->value->int32 : 0;
  persist_write_data(PERSIST_KEYS[direction], route, sizeof(*route));
}

static void store_label(Direction direction, Tuple *tuple) {
  if (!tuple || tuple->length < 2) {  // leer oder nur das Nullbyte
    return;
  }
  strncpy(s_labels[direction], tuple->value->cstring, sizeof(s_labels[direction]) - 1);
  s_labels[direction][sizeof(s_labels[direction]) - 1] = '\0';
  persist_write_string(LABEL_KEYS[direction], s_labels[direction]);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *switch_time = dict_find(iter, MESSAGE_KEY_SWITCH);
  if (switch_time) {
    s_switch_minutes = switch_time->value->int32;
    persist_write_int(PERSIST_KEY_SWITCH, s_switch_minutes);
    if (!s_direction_chosen) {
      set_direction(direction_for_now());
    }
  }

  store_label(DIRECTION_OUT, dict_find(iter, MESSAGE_KEY_LABEL_OUT));
  store_label(DIRECTION_BACK, dict_find(iter, MESSAGE_KEY_LABEL_BACK));

  // PebbleKit JS ist bereit oder hat neue Einstellungen: jetzt abfragen
  if (dict_find(iter, MESSAGE_KEY_JS_READY)) {
    set_status("");
    request_refresh();
    return;
  }

  Tuple *direction_tuple = dict_find(iter, MESSAGE_KEY_DIRECTION);
  Direction direction = direction_tuple && direction_tuple->value->uint8 == DIRECTION_BACK
                        ? DIRECTION_BACK : DIRECTION_OUT;
  store_route(direction, iter);

  // Antworten für die andere Richtung nur speichern, nicht als Status zeigen
  if (direction == s_direction) {
    stop_loading();
    Tuple *status = dict_find(iter, MESSAGE_KEY_STATUS);
    set_status(status ? status->value->cstring : "");
  }
  layer_mark_dirty(s_canvas);
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  stop_loading();
  set_status("Handy nicht verbunden");
  layer_mark_dirty(s_canvas);
}

// --- Zeichnen ---------------------------------------------------------------

static GColor level_color(void) {
  if (s_data->minutes < 0) {
    return GColorDarkGray;
  }
  if (s_data->delay <= 2 * SECONDS_PER_MINUTE) {
    return GColorIslamicGreen;
  }
  if (s_data->delay <= 10 * SECONDS_PER_MINUTE) {
    return GColorChromeYellow;
  }
  return GColorRed;
}

static GColor level_text_color(void) {
  return gcolor_equal(level_color(), GColorChromeYellow) ? GColorBlack : GColorWhite;
}

static GSize text_size(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 200, 80),
                                               GTextOverflowModeTrailingEllipsis,
                                               GTextAlignmentLeft);
}

static void draw_text(GContext *ctx, const char *text, GFont font, GRect box,
                      GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis, align, NULL);
}

static void draw_header(GContext *ctx, GRect bounds, time_t now) {
  graphics_context_set_fill_color(ctx, level_color());
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, 32), 0, GCornerNone);

  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  draw_text(ctx, s_labels[s_direction], font, GRect(8, 0, 130, 30), GTextAlignmentLeft, level_text_color());

  char clock[8];
  strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
  draw_text(ctx, clock, font, GRect(bounds.size.w - 78, 0, 70, 30), GTextAlignmentRight,
            level_text_color());
}

static void draw_trend(GContext *ctx, GPoint center) {
  if (s_data->trend > 0) {
    gpath_move_to(s_arrow_up, center);
    graphics_context_set_fill_color(ctx, GColorMelon);
    gpath_draw_filled(ctx, s_arrow_up);
  } else if (s_data->trend < 0) {
    gpath_move_to(s_arrow_down, center);
    graphics_context_set_fill_color(ctx, GColorMintGreen);
    gpath_draw_filled(ctx, s_arrow_down);
  } else {
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, GRect(center.x - 9, center.y - 2, 18, 5), 2, GCornersAll);
  }
}

static void draw_minutes(GContext *ctx, GRect bounds) {
  char number[12];
  if (s_data->minutes < 0) {
    strcpy(number, "--");
  } else {
    snprintf(number, sizeof(number), "%d", (int)s_data->minutes);
  }

  GFont big = fonts_get_system_font(FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM);
  GFont unit = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GSize number_size = text_size(number, big);
  GSize unit_size = text_size("min", unit);

  // Ohne Daten gibt es keinen Trend, die Striche stehen dann mittig
  const int arrow_w = s_data->minutes < 0 ? 0 : 20;
  const int gap = s_data->minutes < 0 ? 0 : 8;
  int total = arrow_w + gap + number_size.w + 4 + unit_size.w;
  int x = (bounds.size.w - total) / 2;
  const int top = 42;

  if (s_data->minutes >= 0) {
    draw_trend(ctx, GPoint(x + arrow_w / 2, top + 34));
  }
  x += arrow_w + gap;
  draw_text(ctx, number, big, GRect(x, top, number_size.w + 4, 70), GTextAlignmentLeft,
            GColorWhite);
  x += number_size.w + 4;
  draw_text(ctx, "min", unit, GRect(x, top + 30, unit_size.w + 4, 34), GTextAlignmentLeft,
            GColorWhite);
}

static void draw_delay(GContext *ctx, GRect bounds) {
  if (s_data->minutes < 0) {
    return;
  }
  char text[24];
  int delay_min = (s_data->delay + 30) / SECONDS_PER_MINUTE;
  if (delay_min < 1) {
    strcpy(text, "kein Stau");
  } else {
    snprintf(text, sizeof(text), "+%d min Stau", delay_min);
  }

  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GSize size = text_size(text, font);
  int w = size.w + 24;
  GRect pill = GRect((bounds.size.w - w) / 2, 116, w, 28);
  graphics_context_set_fill_color(ctx, level_color());
  graphics_fill_rect(ctx, pill, 8, GCornersAll);
  draw_text(ctx, text, font, GRect(pill.origin.x, pill.origin.y - 3, pill.size.w, 30),
            GTextAlignmentCenter, level_text_color());
}

static void draw_details(GContext *ctx, GRect bounds, time_t now) {
  if (s_data->minutes < 0) {
    draw_text(ctx, "Noch keine Daten", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
              GRect(0, 148, bounds.size.w, 30), GTextAlignmentCenter, GColorLightGray);
    return;
  }

  char arrival[20];
  time_t at = now + s_data->minutes * SECONDS_PER_MINUTE;
  strftime(arrival, sizeof(arrival), "Ankunft %H:%M", localtime(&at));
  draw_text(ctx, arrival, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
            GRect(0, 146, bounds.size.w, 32), GTextAlignmentCenter, GColorWhite);

  char detail[40];
  int free_min = s_data->minutes - (s_data->delay + 30) / SECONDS_PER_MINUTE;
  snprintf(detail, sizeof(detail), "%d,%d km, ohne Stau %d min", (int)(s_data->distance / 1000),
           (int)((s_data->distance % 1000) / 100), free_min);
  draw_text(ctx, detail, fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(0, 178, bounds.size.w, 22), GTextAlignmentCenter, GColorLightGray);
}

static void draw_footer(GContext *ctx, GRect bounds, time_t now) {
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(8, 203), GPoint(bounds.size.w - 8, 203));

  char text[48];
  GColor color = GColorLightGray;
  if (s_loading) {
    strcpy(text, "Aktualisiere ...");
  } else if (s_status[0]) {
    strcpy(text, s_status);
    color = GColorMelon;
  } else if (s_paused) {
    strcpy(text, "Pausiert, SELECT = neu");
    color = GColorChromeYellow;
  } else if (s_data->minutes >= 0) {
    int age = (int)(now - s_data->updated);
    if (age < 0) {
      age = 0;
    }
    if (age < SECONDS_PER_MINUTE) {
      strcpy(text, "Stand: gerade eben");
    } else {
      snprintf(text, sizeof(text), "Stand: vor %d min", age / SECONDS_PER_MINUTE);
    }
    if (age >= STALE_AFTER) {
      color = GColorChromeYellow;
    }
  } else {
    text[0] = '\0';
  }
  draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(0, 204, bounds.size.w, 22), GTextAlignmentCenter, color);
}

static void update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  time_t now = time(NULL);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  draw_header(ctx, bounds, now);
  draw_minutes(ctx, bounds);
  draw_delay(ctx, bounds);
  draw_details(ctx, bounds, now);
  draw_footer(ctx, bounds, now);
}

// --- Ablauf -----------------------------------------------------------------

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  time_t now = time(NULL);
  if (!s_paused && now - s_session_start >= AUTO_REFRESH_LIMIT) {
    s_paused = true;
  }
  if (!s_paused && !s_loading && now - s_last_request >= REFRESH_SECONDS - 5) {
    request_refresh();
  }
  layer_mark_dirty(s_canvas);
}

static void restart_session(void) {
  s_session_start = time(NULL);
  s_paused = false;
  set_status("");
  request_refresh();
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  restart_session();
}

static void toggle_click(ClickRecognizerRef recognizer, void *context) {
  set_direction(s_direction == DIRECTION_OUT ? DIRECTION_BACK : DIRECTION_OUT);
  s_direction_chosen = true;
  restart_session();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_UP, toggle_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, toggle_click);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, update_proc);
  layer_add_child(root, s_canvas);
  s_arrow_up = gpath_create(&ARROW_UP_INFO);
  s_arrow_down = gpath_create(&ARROW_DOWN_INFO);
}

static void window_unload(Window *window) {
  gpath_destroy(s_arrow_up);
  gpath_destroy(s_arrow_down);
  layer_destroy(s_canvas);
}

static void init(void) {
  for (int i = 0; i < 2; i++) {
    if (persist_exists(PERSIST_KEYS[i])) {
      persist_read_data(PERSIST_KEYS[i], &s_routes[i], sizeof(s_routes[i]));
    }
  }
  if (persist_exists(PERSIST_KEY_SWITCH)) {
    s_switch_minutes = persist_read_int(PERSIST_KEY_SWITCH);
  }
  for (int i = 0; i < 2; i++) {
    if (persist_exists(LABEL_KEYS[i])) {
      persist_read_string(LABEL_KEYS[i], s_labels[i], sizeof(s_labels[i]));
    }
  }
  set_direction(direction_for_now());

  // Die erste Abfrage startet, sobald PebbleKit JS JS_READY meldet
  s_session_start = time(NULL);
  s_last_request = s_session_start;
  s_loading = true;

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(256, 64);
  start_timeout();

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
