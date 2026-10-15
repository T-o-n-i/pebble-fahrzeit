#include <pebble.h>

// Fahrzeit — ursprünglich von Toni (github.com/T-o-n-i/pebble-fahrzeit), seit 0.2.0 umgebaut (mit Seb 2026-10-11):
// Start mit der Frage WOHIN? (Favoriten ab Standort oder gespeicherte Strecken), Wahl gilt 1 h bzw. bis 10 min nach
// der Abfahrt, optional Ankunftszeit mit Erinnerung vor der Abfahrt. Bedienung und Begriffe wie Linie 8.
// Das Handy (src/pkjs) hält Favoriten und Strecken, fragt TomTom ab und schickt fertige Minuten.
// Zwei Ansichten: „Dunkel“ (intern toni, Standard, Tonis Original-Skin mit Systemschriften) und seit 0.4.0 „Papier“ (schwarz auf weiß,
// fett, wie in Simzeit und Linie 8). Die Ansicht Phosphor (0.1.0 bis 0.3.1) ist entfernt.

// Neue Abfrage, solange die App offen ist
#define REFRESH_SECONDS (3 * SECONDS_PER_MINUTE)
// Danach automatische Abfragen aussetzen, schont das TomTom-Kontingent
#define AUTO_REFRESH_LIMIT (30 * SECONDS_PER_MINUTE)
// Ab diesem Alter wird der Stand farbig markiert
#define STALE_AFTER (20 * SECONDS_PER_MINUTE)
// Ohne Antwort vom Handy gilt die Abfrage als gescheitert
#define REQUEST_TIMEOUT_MS 45000
// So lange gilt eine Wahl ohne Ankunftszeit, danach kommt NEUES ZIEL?
#define WAHL_GILT (60 * SECONDS_PER_MINUTE)
// Mit Ankunftszeit: NEUES ZIEL? erst so lange nach der berechneten Abfahrt
#define NACH_ABFAHRT (10 * SECONDS_PER_MINUTE)
// Ruhe in Listen und Zeitwahl, danach zurück zur Seite bzw. raus; NEUES ZIEL? zählt dann als NEIN
#define RUHE_MS 30000
#define RUHE_FRAGE_MS 10000
// Solange die Einstellungen am Handy offen sind, nicht von selbst beenden: sonst schließt die Pebble-App die Seite.
// Höchstens so lange, falls die Meldung „geschlossen“ ausbleibt
#define EINSTELLUNGEN_MAX (15 * SECONDS_PER_MINUTE)

// persist: alte Schlüssel 1–5 (0.1.0) werden beim ersten Start gelöscht
#define PK_FORMAT 10
#define PK_WAHL 11
#define PK_DATEN 12
#define PK_ERINNERUNG 13
#define PK_ZIELE_N 14
#define PK_ANSICHT 15
#define PK_ZIEL0 100
#define FORMAT 2

#define MAX_ZIELE 30
#define NAMELEN 25

typedef struct {
  int32_t minutes;   // Fahrzeit mit Verkehr und Korrektur, -1 = noch keine Daten
  int32_t delay;     // Verzögerung durch Stau in Sekunden
  int32_t distance;  // Strecke in Metern
  int32_t updated;   // Zeitpunkt der Abfrage bei TomTom
  int8_t trend;      // -1 kürzer, 0 gleich, 1 länger als die vorige Abfrage
} RouteData;

typedef struct {     // ein Eintrag der Liste WOHIN?, vom Handy
  int16_t id;        // fest je Favorit bzw. Strecke (vergibt das Handy)
  char typ;          // 'F' Favorit (ab Standort), 'S' Strecke (fester Start)
  char name[NAMELEN];
} Ziel;

typedef struct {     // die aktuelle Wahl
  int16_t id;        // 0 = keine
  char typ;
  char name[NAMELEN];
  int32_t gewaehlt;  // seit wann (bzw. seit dem letzten NEIN)
  int32_t ankunft;   // gewünschte Ankunft, 0 = keine
  int32_t abfahrt;   // daraus berechnete Abfahrt, 0 = noch unbekannt
  uint8_t erinnert;  // Erinnerung für diese Abfahrt schon gegeben
} Wahl;

typedef enum { M_SEITE, M_LISTE, M_FRAGE, M_ZEIT } Modus;
typedef enum { L_WOHIN, L_ZIELMENUE, L_MENUE, L_EINST, L_ERINNERUNG, L_NEUES_ZIEL, L_ANSICHT } ListenArt;

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_timeout, *s_ruhe, *s_blink_timer;
static time_t s_einstellungen_bis;   // bis dahin gelten die Einstellungen am Handy als offen

static RouteData s_route = { .minutes = -1 };
static RouteData *s_data = &s_route;
static Wahl s_wahl;
static Ziel s_ziele[MAX_ZIELE];
static int s_ziele_n;
static int s_erinnerung = 10;          // Minuten vor der Abfahrt, 0 = aus
static time_t s_wecker;                // geplanter Weckzeitpunkt, 0 = keiner
static char s_status[48];
static bool s_loading, s_paused, s_js_bereit;
static time_t s_session_start, s_last_request;
static Modus s_modus = M_SEITE;
static uint8_t s_ansicht;              // 0 Toni (Standard), 1 Papier (bis 0.3.1: 1 = Phosphor, wird Papier)
#define PAPIER (s_ansicht == 1)

// Liste wie Linie 8: wert -1 = Überschrift, -2 = Hinweis, beide nicht wählbar.
#define L_MAX (MAX_ZIELE + 4)
static ListenArt s_lart;
static char s_ltitel[64], s_linfo[64];
static char s_ltext[L_MAX][36];
static int8_t s_lwert[L_MAX];
static int s_lanz, s_lsel, s_loben;
static int s_wohin_sel;                // gewählte Zeile in WOHIN?, für die Rückkehr aus dem Eintragsmenü
static int s_ziel_nr = -1;             // Eintrag (Index in s_ziele), für den das Eintragsmenü offen ist

// Bestätigungsseite (Löschen)
static char s_fkopf[32], s_ftext[64];

// Zeitwahl Ankunft
static int s_z_h, s_z_m, s_z_phase, s_z_sel;   // Phase 0 Stunden, 1 Minuten, 2 Frage ZEIT OK?
static bool s_blink_an = true;
static bool s_z_aus_menue;             // true: aus dem Menü der Seite (Wahl bleibt), false: aus dem Eintragsmenü

static void zeige_seite(void);
static void zeige_wohin(void);
static void request_refresh(bool zaehlen);
static void ruhe_starten(void);
static int s_einst_sel;                // gewählte Zeile in den Einstellungen
static void ansicht_setzen(int a);
static void liste_sichtbar(void);

// --- Hilfen -----------------------------------------------------------------

static void kopiere(char *ziel, const char *quelle, size_t n) {
  strncpy(ziel, quelle, n - 1);
  ziel[n - 1] = '\0';
}

static void set_status(const char *text) { kopiere(s_status, text, sizeof(s_status)); }

static void hm_text(char *buf, size_t n, time_t t) { strftime(buf, n, "%H:%M", localtime(&t)); }

static void app_verlassen(void) {
  exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);   // zurück aufs Watchface wie Linie 8
  window_stack_pop_all(true);
}

static void wahl_speichern(void) { persist_write_data(PK_WAHL, &s_wahl, sizeof(s_wahl)); }

static int ziel_index(int16_t id) {
  for (int i = 0; i < s_ziele_n; i++) {
    if (s_ziele[i].id == id) {
      return i;
    }
  }
  return -1;
}

// --- Wecker (Erinnerung vor der Abfahrt) -------------------------------------

static time_t wecker_zeit(void) {
  if (!s_wahl.id || !s_wahl.ankunft || !s_wahl.abfahrt || !s_erinnerung || s_wahl.erinnert) {
    return 0;
  }
  return s_wahl.abfahrt - s_erinnerung * SECONDS_PER_MINUTE;
}

static void erinnern(void) {
  static const uint32_t muster[] = { 400, 200, 400, 200, 400 };
  vibes_enqueue_custom_pattern((VibePattern) { .durations = muster, .num_segments = 5 });
  s_wahl.erinnert = 1;
  wahl_speichern();
}

// Wecker neu stellen, wenn sich die Abfahrt um eine Minute oder mehr verschoben hat
static void wecker_stellen(void) {
  const time_t t = wecker_zeit();
  const time_t now = time(NULL);
  if (t && t <= now) {                       // schon fällig, App ist offen: jetzt erinnern
    if (s_modus == M_SEITE) {
      erinnern();
    }
    return;
  }
  if (t == s_wecker || (t && s_wecker && t - s_wecker < 60 && s_wecker - t < 60)) {
    return;
  }
  wakeup_cancel_all();
  s_wecker = 0;
  if (!t) {
    return;
  }
  // Fällt eine andere App in dieselbe Minute, eine Minute früher bzw. später versuchen
  static const int versatz[] = { 0, -60, 60, -120 };
  for (int i = 0; i < 4; i++) {
    if (t + versatz[i] > now + 30 && wakeup_schedule(t + versatz[i], 1, true) >= 0) {
      s_wecker = t;
      return;
    }
  }
}

static void wecker_handler(WakeupId id, int32_t cookie) {
  s_wecker = 0;
  if (!s_wahl.ankunft || s_wahl.erinnert) {
    return;
  }
  erinnern();
  zeige_seite();
  request_refresh(false);
}

// --- Wahl -------------------------------------------------------------------

static bool wahl_abgelaufen(void) {
  if (!s_wahl.id) {
    return false;
  }
  const time_t now = time(NULL);
  if (s_wahl.ankunft) {
    return s_wahl.abfahrt ? now >= s_wahl.abfahrt + NACH_ABFAHRT : now >= s_wahl.ankunft;
  }
  return now >= s_wahl.gewaehlt + WAHL_GILT;
}

static void route_leeren(void) {
  s_route = (RouteData) { .minutes = -1 };
  persist_write_data(PK_DATEN, &s_route, sizeof(s_route));
}

static void wahl_setzen(const Ziel *z, time_t ankunft) {
  const bool anders = s_wahl.id != z->id;
  s_wahl.id = z->id;
  s_wahl.typ = z->typ;
  kopiere(s_wahl.name, z->name, sizeof(s_wahl.name));
  s_wahl.gewaehlt = time(NULL);
  s_wahl.ankunft = ankunft;
  s_wahl.abfahrt = (ankunft && s_route.minutes >= 0 && !anders) ? ankunft - s_route.minutes * SECONDS_PER_MINUTE : 0;
  s_wahl.erinnert = 0;
  if (anders) {
    route_leeren();
  }
  wahl_speichern();
  wecker_stellen();
}

static void wahl_verlaengern(void) {        // NEIN auf NEUES ZIEL?: altes Ziel eine weitere Stunde
  s_wahl.gewaehlt = time(NULL);
  if (s_wahl.ankunft && wahl_abgelaufen()) {  // Abfahrt vorbei: Ankunft gilt nicht mehr
    s_wahl.ankunft = 0;
    s_wahl.abfahrt = 0;
  }
  wahl_speichern();
  wecker_stellen();
}

static void wahl_loeschen(void) {
  memset(&s_wahl, 0, sizeof(s_wahl));
  wahl_speichern();
  route_leeren();
  wecker_stellen();
}

static void abfahrt_nachrechnen(void) {
  if (!s_wahl.ankunft || s_route.minutes < 0) {
    return;
  }
  const int32_t neu = s_wahl.ankunft - s_route.minutes * SECONDS_PER_MINUTE;
  if (neu != s_wahl.abfahrt) {
    s_wahl.abfahrt = neu;
    wahl_speichern();
  }
  wecker_stellen();
}

// --- Verbindung zum Handy ---------------------------------------------------

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

// Abfrage für die aktuelle Wahl; zaehlen = gerade neu gewählt (Handy zählt die Häufigkeit)
static void request_refresh(bool zaehlen) {
  if (!s_wahl.id) {
    return;
  }
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
  dict_write_int32(iter, MESSAGE_KEY_ZIEL, s_wahl.id);
  dict_write_int32(iter, MESSAGE_KEY_ANKUNFT, s_wahl.ankunft);
  if (zaehlen) {
    dict_write_uint8(iter, MESSAGE_KEY_ZAEHLEN, 1);
  }
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_loading = true;
    s_last_request = time(NULL);
    start_timeout();
  }
  layer_mark_dirty(s_canvas);
}

static void senden_int(uint32_t key, int32_t wert) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_int32(iter, key, wert);
  app_message_outbox_send();
}

static void store_route(DictionaryIterator *iter) {
  Tuple *minutes = dict_find(iter, MESSAGE_KEY_MINUTES);
  Tuple *updated = dict_find(iter, MESSAGE_KEY_UPDATED);
  if (!minutes || !updated) {
    return;
  }
  Tuple *delay = dict_find(iter, MESSAGE_KEY_DELAY);
  Tuple *distance = dict_find(iter, MESSAGE_KEY_DISTANCE);
  RouteData *route = &s_route;

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
  persist_write_data(PK_DATEN, route, sizeof(*route));
  abfahrt_nachrechnen();
}

// Liste vom Handy: Zeilen "id|T|Name", durch '\n' getrennt, Favoriten und Strecken je nach Häufigkeit
static void liste_lesen(const char *s) {
  s_ziele_n = 0;
  while (*s && s_ziele_n < MAX_ZIELE) {
    Ziel *z = &s_ziele[s_ziele_n];
    int id = 0;
    while (*s >= '0' && *s <= '9') {
      id = id * 10 + (*s++ - '0');
    }
    if (*s != '|' || !s[1] || s[2] != '|') {   // kaputte Zeile überspringen
      while (*s && *s != '\n') s++;
      if (*s) s++;
      continue;
    }
    z->id = (int16_t)id;
    z->typ = s[1];
    s += 3;
    size_t n = 0;
    while (*s && *s != '\n') {
      if (n + 1 < sizeof(z->name)) {
        z->name[n++] = *s;
      }
      s++;
    }
    z->name[n] = '\0';
    if (*s) s++;
    if (id > 0) {
      s_ziele_n++;
    }
  }
  persist_write_int(PK_ZIELE_N, s_ziele_n);
  for (int i = 0; i < s_ziele_n; i++) {
    persist_write_data(PK_ZIEL0 + i, &s_ziele[i], sizeof(Ziel));
  }
  // Wahl am Handy gelöscht oder umbenannt?
  if (s_wahl.id) {
    const int k = ziel_index(s_wahl.id);
    if (k < 0) {
      wahl_loeschen();
      if (s_modus == M_SEITE) {
        zeige_wohin();
      }
    } else if (strcmp(s_wahl.name, s_ziele[k].name) != 0) {
      kopiere(s_wahl.name, s_ziele[k].name, sizeof(s_wahl.name));
      wahl_speichern();
    }
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  const bool bereit = dict_find(iter, MESSAGE_KEY_JS_READY) != NULL;
  if (bereit) {
    s_js_bereit = true;
  }
  Tuple *t = dict_find(iter, MESSAGE_KEY_ANSICHT);
  if (t && (uint8_t)t->value->int32 != s_ansicht) {
    ansicht_setzen(t->value->int32);
    if (s_modus == M_LISTE) {
      liste_sichtbar();
    }
  }
  t = dict_find(iter, MESSAGE_KEY_ERINNERUNG);
  if (t) {
    s_erinnerung = t->value->int32;
    persist_write_int(PK_ERINNERUNG, s_erinnerung);
    wecker_stellen();
  }
  t = dict_find(iter, MESSAGE_KEY_EINSTELLUNGEN);
  if (t) {
    s_einstellungen_bis = t->value->int32 ? time(NULL) + EINSTELLUNGEN_MAX : 0;
    if (!s_einstellungen_bis && s_modus != M_SEITE) {
      ruhe_starten();   // nach dem Schließen wieder die vollen 30 s
    }
  }
  t = dict_find(iter, MESSAGE_KEY_LISTE);
  if (t) {
    liste_lesen(t->length > 1 ? t->value->cstring : "");
    if (s_modus == M_LISTE && s_lart == L_WOHIN) {
      zeige_wohin();
    }
  }

  // PebbleKit JS ist bereit oder hat neue Einstellungen: jetzt abfragen
  if (bereit) {
    set_status("");
    if (s_modus == M_SEITE && s_wahl.id) {
      request_refresh(false);
    } else {
      stop_loading();
    }
    layer_mark_dirty(s_canvas);
    return;
  }

  Tuple *ziel = dict_find(iter, MESSAGE_KEY_ZIEL);
  if (!ziel || ziel->value->int32 != s_wahl.id) {   // Antwort auf eine frühere Wahl: verwerfen
    layer_mark_dirty(s_canvas);
    return;
  }
  store_route(iter);
  stop_loading();
  Tuple *status = dict_find(iter, MESSAGE_KEY_STATUS);
  set_status(status && status->length > 1 ? status->value->cstring : "");
  layer_mark_dirty(s_canvas);
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  stop_loading();
  set_status("Handy nicht verbunden");
  layer_mark_dirty(s_canvas);
}

// --- Zeichnen: gemeinsame Hilfen ---------------------------------------------

// Titel eines Ziels wie Linie 8: Favorit ab Standort "> Zuhause", Strecke mit ihrem Namen
static void ziel_titel(char typ, const char *name, char *out, size_t n) {
  snprintf(out, n, typ == 'F' ? "> %s" : "%s", name);
}

// sichtbare Listenzeilen, eine weniger mit Info-Zeile
static int zeilen(void) {
  return s_linfo[0] ? 4 : 5;
}

// "3/7" im Fuß, wenn die Liste nicht auf einen Blick passt (nur wählbare Zeilen)
static void liste_zaehler(char *buf, size_t n) {
  buf[0] = '\0';
  if (s_lanz > zeilen()) {
    int nr = 0, cnt = 0;
    for (int i = 0; i < s_lanz; i++) {
      if (s_lwert[i] >= 0) {
        cnt++;
        if (i <= s_lsel) nr++;
      }
    }
    snprintf(buf, n, "%d/%d", nr, cnt);
  }
}

// Titel der Zeitwahl: das Ziel, für das die Ankunft gilt
static void zeit_titel(char *out, size_t n) {
  if (s_z_aus_menue) {
    ziel_titel(s_wahl.typ, s_wahl.name, out, n);
  } else {
    ziel_titel(s_ziele[s_ziel_nr].typ, s_ziele[s_ziel_nr].name, out, n);
  }
}

static time_t zeit_ziel(void);

// --- Zeichnen: Ansicht „Toni“ (Standard seit 0.3.0) und „Papier“ (0.4.0) -----
// Tonis Original-Skin (github.com/T-o-n-i/pebble-fahrzeit, Commit 9c57221): Systemschriften Gothic und LECO,
// Kopfbalken in der Farbe der Stau-Stufe, Stau als Pille. Die Seite ist sein Code, Listen, Bestätigungsseite und
// Zeitwahl sind im selben Stil nachgebaut (Kopfbalken dunkelgrau, gewählte Zeile als weiße Pille).
// Papier zeichnet mit denselben Funktionen, nur Farben und Schriften kommen aus farben_setzen().

static GFont s_g18, s_g18b, s_g24, s_g24b, s_g28b, s_leco60, s_roboto60;
#define ZIFFERN_DY_PAPIER 2   // Roboto Bold 60 gegen LECO 60: Ziffern gleich hoch, im Emulator gemessen

// Ansicht Papier (wie in Simzeit und Linie 8): Tonis Aufbau, aber schwarz auf weiß, alles fett, kein Grau in der
// Schrift, Ziffern in Roboto Bold statt LECO. Toni behält seine Farben unverändert.
static GColor s_hg, s_vg, s_neben, s_linie, s_warn, s_fehler, s_kopf_hg, s_kopf_vg, s_wahl_hg, s_wahl_vg, s_abschnitt_hg, s_abschnitt_vg;
static GFont s_t18, s_t24, s_ziffern;
static int s_ziffern_dy;                 // Roboto sitzt im Rahmen anders als LECO
static void farben_setzen(void) {
  if (PAPIER && !s_roboto60) s_roboto60 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_ROBOTO_BOLD_60));
  s_hg = PAPIER ? GColorWhite : GColorBlack;
  s_vg = PAPIER ? GColorBlack : GColorWhite;
  s_neben = PAPIER ? GColorBlack : GColorLightGray;
  s_linie = PAPIER ? GColorLightGray : GColorDarkGray;
  s_warn = PAPIER ? GColorOrange : GColorChromeYellow;
  s_fehler = PAPIER ? GColorRed : GColorMelon;
  s_kopf_hg = PAPIER ? GColorWhite : GColorDarkGray;
  s_kopf_vg = PAPIER ? GColorBlack : GColorWhite;
  s_wahl_hg = PAPIER ? GColorBlack : GColorWhite;
  s_wahl_vg = PAPIER ? GColorWhite : GColorBlack;
  s_abschnitt_hg = PAPIER ? GColorLightGray : GColorOrange;   // Orange wie die Einstellungsseite in Dunkel
  s_abschnitt_vg = PAPIER ? GColorBlack : GColorWhite;
  s_t18 = PAPIER ? s_g18b : s_g18;
  s_t24 = PAPIER ? s_g24b : s_g24;
  s_ziffern = PAPIER ? s_roboto60 : s_leco60;
  s_ziffern_dy = PAPIER ? ZIFFERN_DY_PAPIER : 0;
  if (s_window) window_set_background_color(s_window, s_hg);
}
static GPath *s_arrow_up, *s_arrow_down;
static GBitmap *s_hinweis;              // „halten: Menü“ hochkant neben Select, Bild aus resources/images
static GPoint s_up_points[] = { { 0, -10 }, { 10, 6 }, { -10, 6 } };
static GPoint s_down_points[] = { { 0, 10 }, { 10, -6 }, { -10, -6 } };
static const GPathInfo ARROW_UP_INFO = { .num_points = 3, .points = s_up_points };
static const GPathInfo ARROW_DOWN_INFO = { .num_points = 3, .points = s_down_points };
#define T_KOPF  32         // Höhe des Kopfbalkens
#define T_Y0    36         // Listen: erste Zeile
#define T_ZEILE 30         // Listen: Zeilenhöhe
#define T_FUSS  203        // Linie über dem Fuß

static GColor t_level(void) {
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

static GColor t_level_text(void) {
  return gcolor_equal(t_level(), GColorChromeYellow) ? GColorBlack : GColorWhite;
}

static GSize t_size(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 200, 80),
                                               GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
}

static void t_text(GContext *ctx, const char *text, GFont font, GRect box, GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis, align, NULL);
}

// Kopfbalken wie Tonis Seite: links der Titel, rechts die Uhrzeit
static void t_kopf(GContext *ctx, const char *titel, GColor farbe, GColor schrift, time_t now) {
  graphics_context_set_fill_color(ctx, farbe);
  graphics_fill_rect(ctx, GRect(0, 0, 200, T_KOPF), 0, GCornerNone);
  if (PAPIER) {
    graphics_context_set_stroke_color(ctx, s_linie);
    graphics_draw_line(ctx, GPoint(0, T_KOPF - 1), GPoint(200, T_KOPF - 1));
  }
  char clock[8];
  hm_text(clock, sizeof(clock), now);
  t_text(ctx, titel, s_g24b, GRect(8, 0, 130, 30), GTextAlignmentLeft, schrift);
  t_text(ctx, clock, s_g24b, GRect(200 - 78, 0, 70, 30), GTextAlignmentRight, schrift);
}

static void t_fuss(GContext *ctx, const char *text, GColor farbe) {
  graphics_context_set_stroke_color(ctx, s_linie);
  graphics_draw_line(ctx, GPoint(8, T_FUSS), GPoint(192, T_FUSS));
  t_text(ctx, text, s_t18, GRect(0, T_FUSS + 1, 200, 22), GTextAlignmentCenter, farbe);
}

static void t_trend(GContext *ctx, GPoint center) {
  if (s_data->trend > 0) {
    gpath_move_to(s_arrow_up, center);
    graphics_context_set_fill_color(ctx, PAPIER ? GColorRed : GColorMelon);
    gpath_draw_filled(ctx, s_arrow_up);
  } else if (s_data->trend < 0) {
    gpath_move_to(s_arrow_down, center);
    graphics_context_set_fill_color(ctx, PAPIER ? GColorIslamicGreen : GColorMintGreen);
    gpath_draw_filled(ctx, s_arrow_down);
  } else {
    graphics_context_set_fill_color(ctx, s_neben);
    graphics_fill_rect(ctx, GRect(center.x - 9, center.y - 2, 18, 5), 2, GCornersAll);
  }
}

static void t_minutes(GContext *ctx) {
  char number[12];
  if (s_data->minutes < 0) {
    strcpy(number, "--");
  } else {
    snprintf(number, sizeof(number), "%d", (int)s_data->minutes);
  }
  GSize number_size = t_size(number, s_ziffern);
  GSize unit_size = t_size("min", s_g28b);
  // Ohne Daten gibt es keinen Trend, die Striche stehen dann mittig
  const int arrow_w = s_data->minutes < 0 ? 0 : 20;
  const int gap = s_data->minutes < 0 ? 0 : 8;
  int x = (200 - (arrow_w + gap + number_size.w + 4 + unit_size.w)) / 2;
  const int top = 42;
  if (s_data->minutes >= 0) {
    t_trend(ctx, GPoint(x + arrow_w / 2, top + 34));
  }
  x += arrow_w + gap;
  t_text(ctx, number, s_ziffern, GRect(x, top + s_ziffern_dy, number_size.w + 4, 70), GTextAlignmentLeft, s_vg);
  x += number_size.w + 4;
  t_text(ctx, "min", s_g28b, GRect(x, top + 30, unit_size.w + 4, 34), GTextAlignmentLeft, s_vg);
}

static void t_delay(GContext *ctx) {
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
  GSize size = t_size(text, s_g24b);
  int w = size.w + 24;
  GRect pill = GRect((200 - w) / 2, 116, w, 28);
  graphics_context_set_fill_color(ctx, t_level());
  graphics_fill_rect(ctx, pill, 8, GCornersAll);
  t_text(ctx, text, s_g24b, GRect(pill.origin.x, pill.origin.y - 3, pill.size.w, 30), GTextAlignmentCenter, t_level_text());
}

// Ohne Ankunftszeit Tonis Zeilen; mit Ankunftszeit „Los 07:56“ und darunter Ankunft und Restzeit
static void t_details(GContext *ctx, time_t now) {
  char zeile[40], an[8];
  if (s_data->minutes < 0) {
    t_text(ctx, "Noch keine Daten", s_g24b, GRect(0, 148, 200, 30), GTextAlignmentCenter, s_neben);
    if (s_wahl.ankunft) {
      hm_text(an, sizeof(an), s_wahl.ankunft);
      snprintf(zeile, sizeof(zeile), "an %s", an);
      t_text(ctx, zeile, s_t18, GRect(0, 178, 200, 22), GTextAlignmentCenter, s_neben);
    }
    return;
  }
  if (s_wahl.ankunft) {
    hm_text(an, sizeof(an), s_wahl.ankunft);
    const time_t los = s_wahl.ankunft - s_data->minutes * SECONDS_PER_MINUTE;
    const int rest = (int)((los - now) / SECONDS_PER_MINUTE);
    GColor farbe = s_vg;
    if (los < now) {
      farbe = GColorRed;
    } else if (s_erinnerung && rest < s_erinnerung) {
      farbe = s_warn;
    }
    strftime(zeile, sizeof(zeile), "Los %H:%M", localtime(&los));
    t_text(ctx, zeile, s_g28b, GRect(0, 146, 200, 32), GTextAlignmentCenter, farbe);
    if (los < now) {
      snprintf(zeile, sizeof(zeile), "an %s, %d min zu spät", an, (int)((now - los + 59) / SECONDS_PER_MINUTE));
    } else if (rest < 1) {
      snprintf(zeile, sizeof(zeile), "an %s, jetzt los", an);
    } else {
      snprintf(zeile, sizeof(zeile), "an %s, los in %d min", an, rest);
    }
    t_text(ctx, zeile, s_t18, GRect(0, 178, 200, 22), GTextAlignmentCenter, los < now ? s_fehler : s_neben);
    return;
  }
  time_t at = now + s_data->minutes * SECONDS_PER_MINUTE;
  strftime(zeile, sizeof(zeile), "Ankunft %H:%M", localtime(&at));
  t_text(ctx, zeile, s_g28b, GRect(0, 146, 200, 32), GTextAlignmentCenter, s_vg);
  int free_min = s_data->minutes - (s_data->delay + 30) / SECONDS_PER_MINUTE;
  snprintf(zeile, sizeof(zeile), "%d,%d km, ohne Stau %d min", (int)(s_data->distance / 1000),
           (int)((s_data->distance % 1000) / 100), free_min);
  t_text(ctx, zeile, s_t18, GRect(0, 178, 200, 22), GTextAlignmentCenter, s_neben);
}

static void t_footer(GContext *ctx, time_t now) {
  char text[48];
  GColor color = s_neben;
  if (s_loading) {
    strcpy(text, "Aktualisiere ...");
  } else if (s_status[0]) {
    kopiere(text, s_status, sizeof(text));
    color = s_fehler;
  } else if (s_paused) {
    strcpy(text, "Pausiert, SELECT = neu");
    color = s_warn;
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
      color = s_warn;
    }
  } else {
    text[0] = '\0';
  }
  t_fuss(ctx, text, color);
}

// Kleiner Hinweis am rechten Rand auf Höhe von Select: Select lang öffnet das Menü (dort die Einstellungen).
// Pebble dreht keinen Text, deshalb ein fertig gedrehtes Bild mit zwei Farben, die Schriftfarbe kommt aus der Palette.
static void t_hinweis(GContext *ctx) {
  if (!s_hinweis) return;
  GColor *pal = gbitmap_get_palette(s_hinweis);
  const int n = gbitmap_get_format(s_hinweis) == GBitmapFormat1BitPalette ? 2 : 4;
  for (int i = 0; pal && i < n; i++) {
    if (pal[i].a) pal[i] = s_neben;
  }
  const GSize sz = gbitmap_get_bounds(s_hinweis).size;
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, s_hinweis, GRect(200 - sz.w - 1, 114 - sz.h / 2, sz.w, sz.h));
  graphics_context_set_compositing_mode(ctx, GCompOpAssign);
}

static void t_seite(GContext *ctx, time_t now) {
  char titel[40];
  ziel_titel(s_wahl.typ, s_wahl.name, titel, sizeof(titel));
  t_kopf(ctx, titel, PAPIER ? s_kopf_hg : t_level(), PAPIER ? s_kopf_vg : t_level_text(), now);
  t_minutes(ctx);
  t_delay(ctx);
  t_details(ctx, now);
  t_footer(ctx, now);
  t_hinweis(ctx);
}

// Gewählte Zeile als weiße Pille (Rundung wie Tonis Stau-Pille), Schrift schwarz und fett
static void t_zeile(GContext *ctx, const char *text, int y, bool gewaehlt) {
  if (gewaehlt) {
    graphics_context_set_fill_color(ctx, s_wahl_hg);
    graphics_fill_rect(ctx, GRect(2, y + 1, 196, T_ZEILE - 2), 8, GCornersAll);
    t_text(ctx, text, s_g24b, GRect(10, y - 1, 182, 30), GTextAlignmentLeft, s_wahl_vg);
  } else {
    t_text(ctx, text, s_t24, GRect(10, y - 1, 182, 30), GTextAlignmentLeft, s_vg);
  }
}

static void t_liste(GContext *ctx, time_t now) {
  char buf[16];
  t_kopf(ctx, s_ltitel, s_kopf_hg, s_kopf_vg, now);
  const int tv = s_linfo[0] ? 22 : 0;
  if (tv) {
    t_text(ctx, s_linfo, s_t18, GRect(10, T_KOPF, 182, 22), GTextAlignmentLeft, s_neben);
  }
  for (int k = 0; k < zeilen() && s_loben + k < s_lanz; k++) {
    const int i = s_loben + k, y = T_Y0 + tv + k * T_ZEILE;
    if (y + T_ZEILE > T_FUSS + 1) {
      break;
    }
    if (s_lwert[i] == -1) {               // Abschnitt (Favoriten, Strecken): farbiges Band über die ganze Breite
      graphics_context_set_fill_color(ctx, s_abschnitt_hg);
      graphics_fill_rect(ctx, GRect(0, y + 3, 200, 24), 0, GCornerNone);
      t_text(ctx, s_ltext[i], s_g18b, GRect(10, y + 3, 182, 22), GTextAlignmentLeft, s_abschnitt_vg);
    } else if (s_lwert[i] < 0) {
      t_text(ctx, s_ltext[i], s_t18, GRect(10, y + 3, 182, 22), GTextAlignmentLeft, s_neben);
    } else {
      t_zeile(ctx, s_ltext[i], y, i == s_lsel);
    }
  }
  liste_zaehler(buf, sizeof(buf));
  t_fuss(ctx, buf, s_neben);
}

// Bestätigungsseite wie Linie 8 (nur Hoch bestätigt): rechts auf Höhe der oberen Taste ein weißes Schild mit Pfeil
static void t_frage(GContext *ctx) {
  char q[sizeof(s_ftext) + 2];
  graphics_context_set_fill_color(ctx, s_kopf_hg);
  graphics_fill_rect(ctx, GRect(0, 0, 200, T_KOPF), 0, GCornerNone);
  if (PAPIER) {
    graphics_context_set_stroke_color(ctx, s_linie);
    graphics_draw_line(ctx, GPoint(0, T_KOPF - 1), GPoint(200, T_KOPF - 1));
  }
  t_text(ctx, s_fkopf, s_g24b, GRect(8, 0, 184, 30), GTextAlignmentLeft, s_kopf_vg);
  graphics_context_set_fill_color(ctx, s_wahl_hg);
  graphics_fill_rect(ctx, GRect(84, 38, 104, 40), 8, GCornersLeft);
  for (int i = 0; i <= 11; i++) {
    graphics_fill_rect(ctx, GRect(188 + i, 38 + i * 20 / 11, 1, 40 - 2 * (i * 20 / 11)), 0, GCornerNone);
  }
  t_text(ctx, "Hier", s_g18b, GRect(90, 37, 96, 22), GTextAlignmentRight, s_wahl_vg);
  t_text(ctx, "bestätigen", s_g18b, GRect(84, 55, 102, 22), GTextAlignmentRight, s_wahl_vg);
  snprintf(q, sizeof(q), "%s?", s_ftext);
  graphics_context_set_text_color(ctx, s_vg);
  graphics_draw_text(ctx, q, s_g28b, GRect(8, 88, 184, 110), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  t_fuss(ctx, "Zurück: nein", s_neben);
}

// Zeitwahl: Stunden und Minuten in LECO wie Tonis Fahrzeit, der Teil in Arbeit blinkt
static void t_zeit(GContext *ctx, time_t now) {
  char titel[40], hh[4], mm[4];
  t_kopf(ctx, "Ankunft", s_kopf_hg, s_kopf_vg, now);
  zeit_titel(titel, sizeof(titel));
  t_text(ctx, titel, s_g24b, GRect(8, 34, 184, 30), GTextAlignmentLeft, s_vg);

  snprintf(hh, sizeof(hh), "%02d", s_z_h);
  snprintf(mm, sizeof(mm), "%02d", s_z_m);
  // Breiten der echten Ziffern (LECO: „1“ schmal), Doppelpunkt als zwei Quadrate dazwischen
  const int wh = t_size(hh, s_ziffern).w, wm = t_size(mm, s_ziffern).w, dp = 16;
  const int x = (200 - (wh + dp + wm)) / 2, y = 56;
  const bool h_an = s_z_phase != 0 || s_blink_an, m_an = s_z_phase != 1 || s_blink_an;
  const GColor fest = s_z_phase == 2 ? s_vg : s_neben;
  if (h_an) t_text(ctx, hh, s_ziffern, GRect(x, y + s_ziffern_dy, wh + 4, 70), GTextAlignmentLeft, s_z_phase == 0 ? s_vg : fest);
  graphics_context_set_fill_color(ctx, s_neben);
  graphics_fill_rect(ctx, GRect(x + wh + 5, y + 28, 6, 6), 0, GCornerNone);   // Ziffern im Emulator: y+19 bis y+61
  graphics_fill_rect(ctx, GRect(x + wh + 5, y + 46, 6, 6), 0, GCornerNone);
  if (m_an) t_text(ctx, mm, s_ziffern, GRect(x + wh + dp, y + s_ziffern_dy, wm + 4, 70), GTextAlignmentLeft, s_z_phase == 1 ? s_vg : fest);

  const time_t t = zeit_ziel();
  const int tag_jetzt = localtime(&now)->tm_yday;   // localtime nutzt einen Puffer: Wert erst merken
  const bool morgen = localtime(&t)->tm_yday != tag_jetzt;
  if (s_z_phase < 2) {
    t_text(ctx, morgen ? "morgen" : "heute", s_t18, GRect(0, 124, 200, 22), GTextAlignmentCenter, s_neben);
    t_text(ctx, s_z_phase == 0 ? "Hoch/Runter: Stunde" : "Hoch/Runter: 5 min", s_t18, GRect(0, 152, 200, 22),
           GTextAlignmentCenter, s_neben);
    t_text(ctx, "Select: weiter", s_t18, GRect(0, 172, 200, 22), GTextAlignmentCenter, s_neben);
    t_fuss(ctx, s_z_phase == 0 ? "Zurück: abbrechen" : "Zurück: Stunde", s_neben);
    return;
  }
  t_text(ctx, morgen ? "Zeit ok? (morgen)" : "Zeit ok? (heute)", s_g18b, GRect(10, 122, 182, 22), GTextAlignmentLeft, s_vg);
  t_zeile(ctx, "Ja", 144, s_z_sel == 0);
  t_zeile(ctx, "Nein, nochmal", 172, s_z_sel == 1);
  t_fuss(ctx, "Zurück: Minuten", s_neben);
}

static void update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  time_t now = time(NULL);

  graphics_context_set_fill_color(ctx, s_hg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  switch (s_modus) {
    case M_SEITE: t_seite(ctx, now); break;
    case M_LISTE: t_liste(ctx, now); break;
    case M_FRAGE: t_frage(ctx); break;
    case M_ZEIT: t_zeit(ctx, now); break;
  }
}

// --- Listen -----------------------------------------------------------------

static void liste_beginnen(ListenArt art, const char *titel, const char *info) {
  s_lart = art;
  kopiere(s_ltitel, titel, sizeof(s_ltitel));
  kopiere(s_linfo, info ? info : "", sizeof(s_linfo));
  s_lanz = 0;
}

static void liste_zeile(const char *text, int8_t wert) {
  if (s_lanz >= L_MAX) {
    return;
  }
  kopiere(s_ltext[s_lanz], text, sizeof(s_ltext[0]));
  s_lwert[s_lanz++] = wert;
}

static void liste_sichtbar(void) {
  const int z = zeilen();
  if (s_lsel >= 0 && s_lsel < s_loben) s_loben = s_lsel;
  if (s_lsel >= s_loben + z) s_loben = s_lsel - z + 1;
  // Überschrift direkt über der ersten Zeile mitzeigen
  if (s_loben > 0 && s_lsel == s_loben && s_lwert[s_loben - 1] < 0) s_loben--;
}

static void liste_fertig(int sel) {
  if (sel < 0 || sel >= s_lanz || s_lwert[sel] < 0) {   // erste wählbare Zeile
    sel = -1;
    for (int i = 0; i < s_lanz; i++) {
      if (s_lwert[i] >= 0) { sel = i; break; }
    }
  }
  s_lsel = sel;
  s_loben = 0;
  liste_sichtbar();
  s_modus = M_LISTE;
  ruhe_starten();
  layer_mark_dirty(s_canvas);
}

static void liste_bewegen(int d) {
  if (s_lsel < 0) return;
  for (int i = s_lsel + d; i >= 0 && i < s_lanz; i += d) {
    if (s_lwert[i] >= 0) {
      s_lsel = i;
      break;
    }
  }
  if (d < 0 && s_lsel == 0) s_loben = 0;
  liste_sichtbar();
  layer_mark_dirty(s_canvas);
}

static void zeige_wohin(void) {
  liste_beginnen(L_WOHIN, "Wohin?", NULL);
  int sel = -1;
  for (int block = 0; block < 2; block++) {
    const char typ = block ? 'S' : 'F';
    bool kopf = false;
    for (int i = 0; i < s_ziele_n; i++) {
      if (s_ziele[i].typ != typ) continue;
      if (!kopf) {
        liste_zeile(block ? "Strecken" : "Favoriten", -1);
        kopf = true;
      }
      if (s_ziele[i].id == s_wahl.id) sel = s_lanz;
      liste_zeile(s_ziele[i].name, (int8_t)i);
    }
  }
  if (!s_ziele_n) {
    liste_zeile(s_js_bereit ? "Noch keine Ziele" : "Warte aufs Handy", -2);
    liste_zeile("Am Handy anlegen:", -2);
    liste_zeile("Pebble-App > Fahrzeit", -2);
  }
  liste_fertig(sel);
}

static void zeige_neues_ziel(void) {
  char info[64], t[40];
  ziel_titel(s_wahl.typ, s_wahl.name, t, sizeof(t));
  snprintf(info, sizeof(info), "Bisher %s", t);
  liste_beginnen(L_NEUES_ZIEL, "Neues Ziel?", info);
  liste_zeile("Ja", 1);
  liste_zeile("Nein", 0);
  liste_fertig(0);
}

static void zeige_zielmenue(int nr) {
  char t[40];
  s_ziel_nr = nr;
  ziel_titel(s_ziele[nr].typ, s_ziele[nr].name, t, sizeof(t));
  liste_beginnen(L_ZIELMENUE, t, NULL);
  liste_zeile("Ankunft um ...", 0);
  liste_zeile("Löschen", 1);
  liste_fertig(0);
}

static void zeige_menue(int sel) {
  char t[40], a[32];
  ziel_titel(s_wahl.typ, s_wahl.name, t, sizeof(t));
  liste_beginnen(L_MENUE, t, NULL);
  liste_zeile("Neues Ziel", 0);
  if (s_wahl.ankunft) {
    char hm[8];
    hm_text(hm, sizeof(hm), s_wahl.ankunft);
    snprintf(a, sizeof(a), "Ankunft %s ändern", hm);
    liste_zeile(a, 1);
    liste_zeile("Ankunft löschen", 2);
  } else {
    liste_zeile("Ankunft um ...", 1);
  }
  liste_zeile("Einstellungen >", 3);
  liste_fertig(sel);
}

static void zeige_einst(void) {
  char a[32];
  liste_beginnen(L_EINST, "Einstellungen", NULL);
  if (s_erinnerung) {
    snprintf(a, sizeof(a), "Erinnerung: %d min", s_erinnerung);
  } else {
    strcpy(a, "Erinnerung: aus");
  }
  liste_zeile(a, 0);
  liste_zeile(PAPIER ? "Ansicht Papier" : "Ansicht Dunkel", 1);
  liste_zeile("Am Handy", -1);
  liste_zeile("Favoriten, Strecken,", -2);
  liste_zeile("Puffer, Zuschlag", -2);
  liste_fertig(s_einst_sel);
}

static void zeige_ansicht(void) {
  liste_beginnen(L_ANSICHT, "Ansicht", NULL);
  liste_zeile("Dunkel", 0);
  liste_zeile("Papier", 1);
  liste_fertig(s_ansicht);
}

static void ansicht_setzen(int a) {
  s_ansicht = a ? 1 : 0;
  persist_write_int(PK_ANSICHT, s_ansicht);
  farben_setzen();
  layer_mark_dirty(s_canvas);
}

static const int8_t ERINNERUNGEN[] = { 0, 5, 10, 15, 20, 30 };
static void zeige_erinnerung(void) {
  liste_beginnen(L_ERINNERUNG, "Erinnerung", "vor der Abfahrt");
  int sel = 0;
  for (unsigned i = 0; i < sizeof(ERINNERUNGEN); i++) {
    char a[16];
    if (ERINNERUNGEN[i]) {
      snprintf(a, sizeof(a), "%d min", ERINNERUNGEN[i]);
    } else {
      strcpy(a, "aus");
    }
    if (ERINNERUNGEN[i] == s_erinnerung) sel = s_lanz;
    liste_zeile(a, (int8_t)i);
  }
  liste_fertig(sel);
}

static void restart_session(void) {
  s_session_start = time(NULL);
  s_paused = false;
  set_status("");
}

static void zeige_seite(void) {
  s_modus = M_SEITE;
  if (s_ruhe) {
    app_timer_cancel(s_ruhe);
    s_ruhe = NULL;
  }
  layer_mark_dirty(s_canvas);
}

// Ziel gewählt (aus WOHIN? oder nach der Zeitwahl): Seite zeigen und sofort abfragen
static void ziel_starten(int nr, time_t ankunft) {
  wahl_setzen(&s_ziele[nr], ankunft);
  restart_session();
  zeige_seite();
  request_refresh(true);
}

// --- Zeitwahl ---------------------------------------------------------------

static time_t zeit_ziel(void) {     // gewählte Stunde/Minute als nächster solcher Zeitpunkt (heute oder morgen)
  const time_t now = time(NULL);
  const struct tm *lt = localtime(&now);
  const int heute = lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;
  time_t t = now - heute + s_z_h * 3600 + s_z_m * 60;
  if (t <= now + 60) {
    t += 24 * 3600;
  }
  return t;
}

static void blink_tick(void *ctx) {
  s_blink_timer = NULL;
  if (s_modus != M_ZEIT || s_z_phase > 1) {
    s_blink_an = true;
    return;
  }
  s_blink_an = !s_blink_an;
  layer_mark_dirty(s_canvas);
  s_blink_timer = app_timer_register(s_blink_an ? 600 : 300, blink_tick, NULL);
}

static void blink_neu(void) {           // nach jeder Taste sichtbar, dann weiter blinken
  s_blink_an = true;
  if (s_blink_timer) app_timer_cancel(s_blink_timer);
  s_blink_timer = app_timer_register(600, blink_tick, NULL);
  layer_mark_dirty(s_canvas);
}

static void zeige_zeit(bool aus_menue) {
  s_z_aus_menue = aus_menue;
  time_t start = aus_menue && s_wahl.ankunft ? s_wahl.ankunft : time(NULL) + 30 * SECONDS_PER_MINUTE;
  const struct tm *lt = localtime(&start);
  s_z_h = lt->tm_hour;
  s_z_m = (lt->tm_min + 4) / 5 * 5;      // auf 5 min aufrunden
  if (s_z_m >= 60) {
    s_z_m = 0;
    s_z_h = (s_z_h + 1) % 24;
  }
  s_z_phase = 0;
  s_z_sel = 0;
  s_modus = M_ZEIT;
  ruhe_starten();
  blink_neu();
}

static void zeit_zurueck(void) {        // Zurück während der Stundenwahl: dorthin, woher die Zeitwahl kam
  if (s_z_aus_menue) {
    zeige_menue(1);
  } else {
    zeige_zielmenue(s_ziel_nr);
  }
}

static void zeit_taste(ButtonId b) {
  if (s_z_phase == 2) {
    if (b == BUTTON_ID_UP || b == BUTTON_ID_DOWN) {
      s_z_sel = b == BUTTON_ID_DOWN ? 1 : 0;
    } else if (b == BUTTON_ID_SELECT) {
      if (s_z_sel == 1) {                 // NEIN: von vorne, mit den eingestellten Werten
        s_z_phase = 0;
        s_z_sel = 0;
        blink_neu();
        return;
      }
      const time_t t = zeit_ziel();
      if (s_z_aus_menue) {
        s_wahl.ankunft = t;
        s_wahl.abfahrt = s_route.minutes >= 0 ? t - s_route.minutes * SECONDS_PER_MINUTE : 0;
        s_wahl.erinnert = 0;
        s_wahl.gewaehlt = time(NULL);
        wahl_speichern();
        wecker_stellen();
        restart_session();
        zeige_seite();
        request_refresh(false);
      } else {
        ziel_starten(s_ziel_nr, t);
      }
      return;
    } else if (b == BUTTON_ID_BACK) {
      s_z_phase = 1;
      blink_neu();
      return;
    }
    layer_mark_dirty(s_canvas);
    return;
  }
  switch (b) {
    case BUTTON_ID_UP:
    case BUTTON_ID_DOWN: {
      const int d = b == BUTTON_ID_UP ? 1 : -1;
      if (s_z_phase == 0) {
        s_z_h = (s_z_h + d + 24) % 24;
      } else {
        s_z_m = (s_z_m + 5 * d + 60) % 60;
      }
      blink_neu();
      break;
    }
    case BUTTON_ID_SELECT:
      s_z_phase++;
      if (s_z_phase == 2) {
        s_blink_an = true;
        s_z_sel = 0;
      }
      blink_neu();
      break;
    case BUTTON_ID_BACK:
      if (s_z_phase == 1) {
        s_z_phase = 0;
        blink_neu();
      } else {
        zeit_zurueck();
      }
      break;
    default:
      break;
  }
}

// --- Ruhe -------------------------------------------------------------------

static void ruhe_abgelaufen(void *ctx) {
  s_ruhe = NULL;
  if (s_modus == M_SEITE) {
    return;
  }
  if (time(NULL) < s_einstellungen_bis) {
    ruhe_starten();
    return;
  }
  if (s_modus == M_LISTE && s_lart == L_NEUES_ZIEL) {
    wahl_verlaengern();
  }
  if (s_wahl.id && !wahl_abgelaufen()) {
    zeige_seite();
  } else {
    app_verlassen();
  }
}

static void ruhe_starten(void) {
  if (s_ruhe) {
    app_timer_cancel(s_ruhe);
  }
  const bool frage = s_modus == M_LISTE && s_lart == L_NEUES_ZIEL;
  s_ruhe = app_timer_register(frage ? RUHE_FRAGE_MS : RUHE_MS, ruhe_abgelaufen, NULL);
}

// --- Auswahl in den Listen --------------------------------------------------

static void liste_waehlen(void) {
  if (s_lsel < 0) return;
  const int w = s_lwert[s_lsel];
  switch (s_lart) {
    case L_WOHIN:
      s_wohin_sel = s_lsel;
      ziel_starten(w, 0);
      break;
    case L_NEUES_ZIEL:
      if (w) {
        zeige_wohin();
      } else {
        wahl_verlaengern();
        zeige_seite();
        if (s_route.minutes < 0 || time(NULL) - s_route.updated > REFRESH_SECONDS) {
          restart_session();
          request_refresh(false);
        }
      }
      break;
    case L_ZIELMENUE:
      if (w == 0) {
        zeige_zeit(false);
      } else {
        kopiere(s_fkopf, "Wohin?", sizeof(s_fkopf));
        snprintf(s_ftext, sizeof(s_ftext), "Löschen: %s", s_ziele[s_ziel_nr].name);
        s_modus = M_FRAGE;
        ruhe_starten();
        layer_mark_dirty(s_canvas);
      }
      break;
    case L_MENUE:
      if (w == 0) {
        zeige_wohin();
      } else if (w == 1) {
        zeige_zeit(true);
      } else if (w == 2) {
        s_wahl.ankunft = 0;
        s_wahl.abfahrt = 0;
        s_wahl.gewaehlt = time(NULL);
        wahl_speichern();
        wecker_stellen();
        zeige_seite();
      } else {
        s_einst_sel = 0;
        zeige_einst();
      }
      break;
    case L_EINST:
      s_einst_sel = s_lsel;
      if (w == 0) {
        zeige_erinnerung();
      } else {
        zeige_ansicht();
      }
      break;
    case L_ANSICHT:
      ansicht_setzen(w);
      senden_int(MESSAGE_KEY_ANSICHT, s_ansicht);
      zeige_einst();
      break;
    case L_ERINNERUNG:
      s_erinnerung = ERINNERUNGEN[w];
      persist_write_int(PK_ERINNERUNG, s_erinnerung);
      s_wahl.erinnert = 0;
      wecker_stellen();
      senden_int(MESSAGE_KEY_ERINNERUNG, s_erinnerung);
      zeige_einst();
      break;
  }
}

static void liste_zurueck(void) {
  switch (s_lart) {
    case L_WOHIN:
      if (s_wahl.id) {
        if (wahl_abgelaufen()) wahl_verlaengern();   // zurück aus NEUES ZIEL > JA zählt als NEIN
        zeige_seite();
      } else {
        app_verlassen();
      }
      break;
    case L_NEUES_ZIEL:
      s_lsel = 1;
      liste_waehlen();
      break;
    case L_ZIELMENUE:
      zeige_wohin();
      liste_fertig(s_wohin_sel);
      break;
    case L_MENUE: zeige_seite(); break;
    case L_EINST: zeige_menue(s_wahl.ankunft ? 3 : 2); break;
    case L_ERINNERUNG: zeige_einst(); break;
    case L_ANSICHT: zeige_einst(); break;
  }
}

static void loeschen_bestaetigt(void) {
  const int nr = s_ziel_nr;
  const int16_t id = s_ziele[nr].id;
  senden_int(MESSAGE_KEY_LOESCHEN, id);
  for (int i = nr; i + 1 < s_ziele_n; i++) {
    s_ziele[i] = s_ziele[i + 1];
  }
  s_ziele_n--;
  persist_write_int(PK_ZIELE_N, s_ziele_n);
  for (int i = nr; i < s_ziele_n; i++) {
    persist_write_data(PK_ZIEL0 + i, &s_ziele[i], sizeof(Ziel));
  }
  if (s_wahl.id == id) {
    wahl_loeschen();
  }
  zeige_wohin();
  liste_fertig(s_wohin_sel);
}

// --- Tasten -----------------------------------------------------------------

static void taste(ButtonId b) {
  switch (s_modus) {
    case M_SEITE:
      if (b == BUTTON_ID_SELECT) {
        restart_session();
        request_refresh(false);
      } else if (b == BUTTON_ID_BACK) {
        app_verlassen();
      }
      return;
    case M_LISTE:
      ruhe_starten();
      if (b == BUTTON_ID_UP || b == BUTTON_ID_DOWN) {
        liste_bewegen(b == BUTTON_ID_UP ? -1 : 1);
      } else if (b == BUTTON_ID_SELECT) {
        liste_waehlen();
      } else {
        liste_zurueck();
      }
      return;
    case M_FRAGE:                          // nur Hoch bestätigt, Zurück = nein
      ruhe_starten();
      if (b == BUTTON_ID_UP) {
        loeschen_bestaetigt();
      } else if (b == BUTTON_ID_BACK) {
        zeige_wohin();
        liste_fertig(s_wohin_sel);
      }
      return;
    case M_ZEIT:
      ruhe_starten();
      zeit_taste(b);
      return;
  }
}

static void click_up(ClickRecognizerRef r, void *c) { taste(BUTTON_ID_UP); }
static void click_down(ClickRecognizerRef r, void *c) { taste(BUTTON_ID_DOWN); }
static void click_select(ClickRecognizerRef r, void *c) { taste(BUTTON_ID_SELECT); }
static void click_back(ClickRecognizerRef r, void *c) { taste(BUTTON_ID_BACK); }

// Select lang = Menü dessen, was man sieht (wie Linie 8)
static void click_select_lang(ClickRecognizerRef r, void *c) {
  if (s_modus == M_SEITE && s_wahl.id) {
    zeige_menue(0);
  } else if (s_modus == M_LISTE && s_lart == L_WOHIN && s_lsel >= 0) {
    s_wohin_sel = s_lsel;
    zeige_zielmenue(s_lwert[s_lsel]);
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, click_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, click_select_lang, NULL);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, click_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, click_down);
  window_single_click_subscribe(BUTTON_ID_BACK, click_back);
}

// --- Ablauf -----------------------------------------------------------------

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  time_t now = time(NULL);
  if (s_modus == M_SEITE) {
    if (wahl_abgelaufen()) {
      zeige_neues_ziel();
      return;
    }
    const time_t w = wecker_zeit();
    if (w && now >= w) {
      erinnern();
    }
    if (!s_paused && now - s_session_start >= AUTO_REFRESH_LIMIT) {
      s_paused = true;
    }
    if (!s_paused && !s_loading && now - s_last_request >= REFRESH_SECONDS - 5) {
      request_refresh(false);
    }
  }
  layer_mark_dirty(s_canvas);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, update_proc);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void laden(void) {
  if (!persist_exists(PK_FORMAT) || persist_read_int(PK_FORMAT) != FORMAT) {
    for (uint32_t k = 1; k <= 5; k++) {        // Schlüssel aus 0.1.0 (Hin- und Rückweg)
      persist_delete(k);
    }
    persist_write_int(PK_FORMAT, FORMAT);
    return;
  }
  if (persist_exists(PK_WAHL)) persist_read_data(PK_WAHL, &s_wahl, sizeof(s_wahl));
  if (persist_exists(PK_DATEN)) persist_read_data(PK_DATEN, &s_route, sizeof(s_route));
  if (persist_exists(PK_ERINNERUNG)) s_erinnerung = persist_read_int(PK_ERINNERUNG);
  if (persist_exists(PK_ANSICHT)) s_ansicht = persist_read_int(PK_ANSICHT) ? 1 : 0;
  s_ziele_n = persist_exists(PK_ZIELE_N) ? persist_read_int(PK_ZIELE_N) : 0;
  if (s_ziele_n > MAX_ZIELE) s_ziele_n = MAX_ZIELE;
  for (int i = 0; i < s_ziele_n; i++) {
    persist_read_data(PK_ZIEL0 + i, &s_ziele[i], sizeof(Ziel));
  }
  if (!s_wahl.id) {
    s_route = (RouteData) { .minutes = -1 };
  }
}

static void init(void) {
  laden();
  s_g18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  s_g18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_g24 = fonts_get_system_font(FONT_KEY_GOTHIC_24);
  s_g24b = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  s_g28b = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  s_leco60 = fonts_get_system_font(FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM);
  farben_setzen();
  s_arrow_up = gpath_create(&ARROW_UP_INFO);
  s_arrow_down = gpath_create(&ARROW_DOWN_INFO);
  s_hinweis = gbitmap_create_with_resource(RESOURCE_ID_HINWEIS_MENUE);

  s_session_start = time(NULL);
  s_last_request = s_session_start;

  s_window = window_create();
  window_set_background_color(s_window, s_hg);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(2048, 128);

  // Start: geweckt = Erinnerung; keine Wahl = WOHIN?; Wahl abgelaufen = NEUES ZIEL?; sonst die Seite.
  // Die erste Abfrage startet, sobald PebbleKit JS JS_READY meldet.
  if (launch_reason() == APP_LAUNCH_WAKEUP && s_wahl.id && s_wahl.ankunft) {
    erinnern();
    zeige_seite();
    s_loading = true;
    start_timeout();
  } else if (!s_wahl.id) {
    zeige_wohin();
  } else if (wahl_abgelaufen()) {
    zeige_neues_ziel();
  } else {
    zeige_seite();
    s_loading = true;
    start_timeout();
  }
  wakeup_service_subscribe(wecker_handler);
  wecker_stellen();

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
  if (s_roboto60) fonts_unload_custom_font(s_roboto60);
  gpath_destroy(s_arrow_up);
  gpath_destroy(s_arrow_down);
  if (s_hinweis) gbitmap_destroy(s_hinweis);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
