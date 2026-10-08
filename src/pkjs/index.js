var Clay = require('@rebble/clay');
var clayConfig = require('./config');

// Die Einstellungen bleiben auf dem Handy; an die Uhr gehen nur Bezeichnungen und Umschaltzeit.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var DEFAULTS = {
  TOMTOM_KEY: '',
  PLACE_A: '',
  PLACE_B: '',
  LABEL_OUT: 'Zur Arbeit',
  LABEL_BACK: 'Nach Hause',
  SWITCH_TIME: '12:00'
};
var REQUEST_TIMEOUT_MS = 15000;
var PLACES_STORAGE = 'resolved-places';

// Richtungen wie auf der Uhr: 0 = Hinweg (Ort 1 → Ort 2), 1 = Rückweg
var OUT = 0;
var BACK = 1;

var inFlight = false;
var pending = null;  // Richtung, die während einer laufenden Abfrage angefragt wurde

function loadSettings() {
  var stored = {};
  try {
    stored = JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    stored = {};
  }
  var settings = {};
  Object.keys(DEFAULTS).forEach(function (key) {
    var value = typeof stored[key] === 'string' ? stored[key].trim() : '';
    settings[key] = value || DEFAULTS[key];
  });
  return settings;
}

// "HH:MM" in Minuten nach Mitternacht, bei Unsinn 12:00
function switchMinutes(settings) {
  var match = /^(\d{1,2}):(\d{2})/.exec(settings.SWITCH_TIME);
  if (!match) return 12 * 60;
  var minutes = parseInt(match[1], 10) * 60 + parseInt(match[2], 10);
  return minutes >= 0 && minutes < 24 * 60 ? minutes : 12 * 60;
}

function send(message) {
  Pebble.sendAppMessage(message, null, function () {
    console.log('Senden an die Uhr fehlgeschlagen');
  });
}

function sendStatus(direction, text) {
  send({ DIRECTION: direction, STATUS: text });
}

// Meldet der Uhr, dass PebbleKit JS bereit ist; die Uhr fragt dann ihre Richtung an.
function announceReady() {
  var settings = loadSettings();
  send({
    JS_READY: 1,
    SWITCH: switchMinutes(settings),
    LABEL_OUT: settings.LABEL_OUT.substring(0, 20),
    LABEL_BACK: settings.LABEL_BACK.substring(0, 20)
  });
}

// GET auf die TomTom-API; callback(fehlertext, json)
function tomtomGet(url, callback) {
  var xhr = new XMLHttpRequest();
  var done = false;
  function finish(error, response) {
    if (done) return;
    done = true;
    callback(error, response);
  }

  xhr.open('GET', url, true);
  xhr.timeout = REQUEST_TIMEOUT_MS;
  xhr.onload = function () {
    if (xhr.status === 400) return finish('Keine Route gefunden');
    // TomTom meldet ein aufgebrauchtes Kontingent ebenfalls mit 403
    if (xhr.status === 403) return finish('Key falsch oder Kontingent leer');
    if (xhr.status === 429) return finish('Zu viele Anfragen, kurz warten');
    if (xhr.status < 200 || xhr.status > 299) return finish('TomTom-Fehler ' + xhr.status);
    var json;
    try {
      json = JSON.parse(xhr.responseText);
    } catch (e) {
      return finish('Antwort unlesbar');
    }
    finish(null, json);
  };
  xhr.onerror = function () { finish('TomTom nicht erreichbar'); };
  xhr.ontimeout = function () { finish('TomTom antwortet nicht'); };
  xhr.send();
}

// "50.04885, 8.55722", "50.04885 8.55722" oder mit Dezimalkomma "50,04885; 8,55722"
// bzw. "50,04885 8,55722" → {lat, lon}, sonst null
function parseCoordinates(text) {
  var lat, lon;
  var match = /^\s*(-?\d+(?:\.\d+)?)\s*[,;\s]\s*(-?\d+(?:\.\d+)?)\s*$/.exec(text);
  var german = /^\s*(-?\d+,\d+)\s*(?:;\s*|,\s+|\s+)(-?\d+,\d+)\s*$/.exec(text);
  if (match) {
    lat = parseFloat(match[1]);
    lon = parseFloat(match[2]);
  } else if (german) {
    lat = parseFloat(german[1].replace(',', '.'));
    lon = parseFloat(german[2].replace(',', '.'));
  } else {
    return null;
  }
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return null;
  return { lat: lat, lon: lon };
}

function loadResolvedPlaces() {
  try {
    return JSON.parse(localStorage.getItem(PLACES_STORAGE)) || {};
  } catch (e) {
    return {};
  }
}

// Liefert Koordinaten für einen Ort. Adressen werden nur einmal gesucht und dann gemerkt.
function resolvePlace(slot, text, apiKey, callback) {
  var coordinates = parseCoordinates(text);
  if (coordinates) return callback(null, coordinates);

  var cache = loadResolvedPlaces();
  if (cache[slot] && cache[slot].query === text) {
    return callback(null, cache[slot]);
  }

  var label = slot === 'A' ? 'Ort 1' : 'Ort 2';
  var url = 'https://api.tomtom.com/search/2/geocode/' + encodeURIComponent(text) +
            '.json?limit=1&language=de-DE&key=' + encodeURIComponent(apiKey);
  tomtomGet(url, function (error, json) {
    if (error) return callback(error);  // ohne Ortsangabe, sonst zu lang für die Fußzeile
    var result = json.results && json.results[0];
    if (!result || !result.position) return callback(label + ' nicht gefunden');

    cache[slot] = { query: text, lat: result.position.lat, lon: result.position.lon };
    localStorage.setItem(PLACES_STORAGE, JSON.stringify(cache));
    console.log(label + ' gefunden: ' + (result.address && result.address.freeformAddress));
    callback(null, cache[slot]);
  });
}

function finishRefresh() {
  inFlight = false;
  if (pending !== null) {
    var next = pending;
    pending = null;
    refresh(next);
  }
}

function fail(direction, text) {
  sendStatus(direction, text);
  finishRefresh();
}

function requestRoute(from, to, apiKey, direction) {
  var url = 'https://api.tomtom.com/routing/1/calculateRoute/' +
            from.lat + ',' + from.lon + ':' + to.lat + ',' + to.lon + '/json' +
            '?traffic=true&travelMode=car&routeType=fastest&computeTravelTimeFor=all' +
            '&key=' + encodeURIComponent(apiKey);
  tomtomGet(url, function (error, json) {
    if (error) return fail(direction, error);
    var route = json.routes && json.routes[0];
    if (!route || !route.summary) return fail(direction, 'Keine Route gefunden');

    var summary = route.summary;
    send({
      DIRECTION: direction,
      MINUTES: Math.round(summary.travelTimeInSeconds / 60),
      DELAY: Math.round(summary.trafficDelayInSeconds || 0),
      DISTANCE: Math.round(summary.lengthInMeters || 0),
      UPDATED: Math.floor(Date.now() / 1000),
      STATUS: ''
    });
    finishRefresh();
  });
}

function refresh(direction) {
  if (inFlight) {
    pending = direction;
    return;
  }
  var settings = loadSettings();
  if (!settings.TOMTOM_KEY) return sendStatus(direction, 'API-Key fehlt: Einstellungen');
  if (!settings.PLACE_A || !settings.PLACE_B) return sendStatus(direction, 'Orte fehlen: Einstellungen');

  inFlight = true;
  resolvePlace('A', settings.PLACE_A, settings.TOMTOM_KEY, function (errorA, placeA) {
    if (errorA) return fail(direction, errorA);
    resolvePlace('B', settings.PLACE_B, settings.TOMTOM_KEY, function (errorB, placeB) {
      if (errorB) return fail(direction, errorB);
      if (direction === BACK) {
        requestRoute(placeB, placeA, settings.TOMTOM_KEY, direction);
      } else {
        requestRoute(placeA, placeB, settings.TOMTOM_KEY, direction);
      }
    });
  });
}

Pebble.addEventListener('ready', function () {
  announceReady();
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.REQUEST) {
    refresh(e.payload.DIRECTION === BACK ? BACK : OUT);
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  clay.getSettings(e.response, false);
  announceReady();
});
