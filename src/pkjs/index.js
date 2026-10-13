// Fahrzeit — Handy-Teil (PebbleKit JS).
// Hält Favoriten (Ziel ab Standort) und Strecken (fester Start), fragt die TomTom Routing API ab und schickt der Uhr
// fertige Minuten mit Zuschlag, Puffer und Parkzeit. Die TomTom-Abfrage stammt aus Tonis Original (0.1.0),
// seit 0.2.0 (mit Seb 2026-10-11) gibt es statt Hin- und Rückweg die Wahl WOHIN? an der Uhr.
//
// Ankunftszeit: bis 90 min vorher mit dem aktuellen Verkehr (departAt jetzt), weiter weg mit arriveAt (TomTom rechnet
// dann mit Erfahrungswerten für diese Uhrzeit). Ob arriveAt auch Live-Verkehr nutzt, sagt die Doku nicht.

var keys = require('message_keys');
var SEITE_HTML = require('./einstellungen_seite');
var L = require('./logik')();

var SPEICHER = 'fahrzeit';
var TIMEOUT_MS = 15000;
var ARRIVE_AB_S = 90 * 60;            // ab so weit in der Zukunft arriveAt statt Verkehr jetzt

// ---------- Daten ----------
function datenLesen() {
  var d = null;
  try { d = JSON.parse(localStorage.getItem(SPEICHER)); } catch (e) { d = null; }
  if (d) return L.bereinigen(d);
  // erster Start nach 0.1.0: Clay-Einstellungen übernehmen
  var alt = null, orte = null;
  try { alt = JSON.parse(localStorage.getItem('clay-settings')); } catch (e) { alt = null; }
  try { orte = JSON.parse(localStorage.getItem('resolved-places')); } catch (e) { orte = null; }
  d = L.ausAlt(alt, orte);
  datenSchreiben(d);
  if (alt) console.log('Einstellungen aus 0.1.0 übernommen: ' + d.favoriten.length + ' Favoriten');
  return d;
}
function datenSchreiben(d) { localStorage.setItem(SPEICHER, JSON.stringify(d)); }

// ---------- Senden der Reihe nach ----------
var schlange = [], sendetGerade = false;
function senden(msg) {
  schlange.push(msg);
  if (!sendetGerade) weiter();
}
function weiter() {
  var msg = schlange.shift();
  if (!msg) { sendetGerade = false; return; }
  sendetGerade = true;
  Pebble.sendAppMessage(msg, weiter, function () {
    console.log('Senden an die Uhr fehlgeschlagen');
    weiter();
  });
}

function einrichtungSenden(bereit) {
  var d = datenLesen(), m = {};
  m[keys.LISTE] = L.listeText(d);
  m[keys.ERINNERUNG] = d.erinnerung;
  m[keys.ANSICHT] = d.ansicht === 'papier' ? 1 : 0;
  if (bereit) m[keys.JS_READY] = 1;
  senden(m);
}

function statusSenden(id, text) {
  var m = {};
  m[keys.ZIEL] = id;
  m[keys.STATUS] = L.utf8Kuerzen(text, 46);
  senden(m);
}

// ---------- TomTom ----------
function tomtomGet(url, fertig) {
  var xhr = new XMLHttpRequest(), done = false;
  function ende(fehler, json) { if (!done) { done = true; fertig(fehler, json); } }
  xhr.open('GET', url, true);
  xhr.timeout = TIMEOUT_MS;
  xhr.onload = function () {
    if (xhr.status === 400) return ende('Keine Route gefunden');
    // TomTom meldet ein aufgebrauchtes Kontingent ebenfalls mit 403
    if (xhr.status === 401 || xhr.status === 403) return ende('Key falsch oder Kontingent leer');
    if (xhr.status === 429) return ende('Zu viele Anfragen, kurz warten');
    if (xhr.status < 200 || xhr.status > 299) return ende('TomTom-Fehler ' + xhr.status);
    var json;
    try { json = JSON.parse(xhr.responseText); } catch (e) { return ende('Antwort unlesbar'); }
    ende(null, json);
  };
  xhr.onerror = function () { ende('TomTom nicht erreichbar'); };
  xhr.ontimeout = function () { ende('TomTom antwortet nicht'); };
  xhr.send();
}

// Koordinaten eines Orts; Adressen ohne Koordinaten werden einmal gesucht und im Ort gespeichert
function aufloesen(d, o, nahe, fertig) {
  if (typeof o.lat === 'number' && typeof o.lon === 'number') return fertig(null, o);
  var k = L.koordinaten(o.adresse);
  if (k) { o.lat = k.lat; o.lon = k.lon; datenSchreiben(d); return fertig(null, o); }
  tomtomGet(L.suchUrl(o.adresse, d.key, nahe), function (fehler, json) {
    if (fehler) return fertig(fehler);
    var t = L.treffer(json)[0];
    if (!t) return fertig(L.kurzName(o) + ' nicht gefunden');
    o.lat = t.lat; o.lon = t.lon;
    datenSchreiben(d);
    console.log('Ort gefunden: ' + t.adresse);
    fertig(null, o);
  });
}

function standort(fertig, ms) {
  if (!navigator.geolocation) return fertig('Kein Standort');
  var offen = true;
  function ende(f, p) { if (offen) { offen = false; fertig(f, p); } }
  ms = ms || 12000;
  setTimeout(function () { ende('Kein Standort'); }, ms);
  navigator.geolocation.getCurrentPosition(
    function (p) { ende(null, { lat: p.coords.latitude, lon: p.coords.longitude }); },
    function () { ende('Kein Standort'); },
    { timeout: ms - 2000, maximumAge: 120000, enableHighAccuracy: false });
}

function iso(sekunden) { return new Date(sekunden * 1000).toISOString().replace(/\.\d{3}Z$/, 'Z'); }

function routeUrl(von, nach, key, ankunft) {
  var zeit = ankunft && ankunft - Date.now() / 1000 > ARRIVE_AB_S ? '&arriveAt=' + encodeURIComponent(iso(ankunft)) : '';
  return 'https://api.tomtom.com/routing/1/calculateRoute/' +
    von.lat + ',' + von.lon + ':' + nach.lat + ',' + nach.lon + '/json' +
    '?traffic=true&travelMode=car&routeType=fastest' + zeit + '&key=' + encodeURIComponent(key);
}

// ---------- Abfrage für die Uhr ----------
var laeuft = false, wartend = null, nr = 0;

function abfrage(id, ankunft) {
  if (laeuft) { wartend = { id: id, ankunft: ankunft }; return; }
  var d = datenLesen();
  if (!d.key) return statusSenden(id, 'API-Key fehlt: Einstellungen');
  var z = L.finden(d, id);
  if (!z) { einrichtungSenden(false); return statusSenden(id, 'Ziel gibt es nicht mehr'); }
  laeuft = true;
  var meine = ++nr;
  function fertig() {
    laeuft = false;
    if (wartend) { var w = wartend; wartend = null; abfrage(w.id, w.ankunft); }
  }
  function fehler(text) { if (meine === nr) statusSenden(id, text); fertig(); }

  function start(weiter) {
    if (z.typ === 'F') return standort(weiter);
    var o = L.ort(d, z.ziel.von);
    if (!o) return weiter('Start fehlt: Einstellungen');
    aufloesen(d, o, null, weiter);
  }
  start(function (f1, von) {
    if (f1) return fehler(f1);
    var nach = z.typ === 'F' ? z.ziel : L.ort(d, z.ziel.nach);
    if (!nach) return fehler('Ziel fehlt: Einstellungen');
    aufloesen(d, nach, von, function (f2, ziel) {
      if (f2) return fehler(f2);
      route(von, ziel, ankunft, true);
    });
  });

  function route(von, ziel, ank, mitAnkunft) {
    tomtomGet(routeUrl(von, ziel, d.key, mitAnkunft ? ank : 0), function (f, json) {
      if (f && mitAnkunft && ank) return route(von, ziel, ank, false);   // arriveAt abgelehnt: mit Verkehr jetzt
      if (f) return fehler(f);
      var r = json.routes && json.routes[0];
      if (!r || !r.summary) return fehler('Keine Route gefunden');
      var s = r.summary, m = {};
      m[keys.ZIEL] = id;
      m[keys.MINUTES] = L.minuten(s.travelTimeInSeconds, L.korrektur(z.ziel, d));
      m[keys.DELAY] = Math.round(s.trafficDelayInSeconds || 0);
      m[keys.DISTANCE] = Math.round(s.lengthInMeters || 0);
      m[keys.UPDATED] = Math.floor(Date.now() / 1000);
      m[keys.STATUS] = '';
      senden(m);
      fertig();
    });
  }
}

function zaehlen(id) {
  var d = datenLesen(), z = L.finden(d, id);
  if (!z) return;
  z.ziel.n = (z.ziel.n || 0) + 1;
  datenSchreiben(d);
  einrichtungSenden(false);             // Reihenfolge nach Häufigkeit nachziehen
}

Pebble.addEventListener('ready', function () { einrichtungSenden(true); });

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload, wert = function (k) { return p[k] !== undefined ? p[k] : p[keys[k]]; };
  if (wert('LOESCHEN') !== undefined) {
    var d = datenLesen();
    L.loeschen(d, wert('LOESCHEN'));
    datenSchreiben(d);
    einrichtungSenden(false);
  }
  if (wert('ERINNERUNG') !== undefined && wert('REQUEST') === undefined) {
    var d2 = datenLesen();
    if (L.ERINNERUNGEN.indexOf(wert('ERINNERUNG')) >= 0) { d2.erinnerung = wert('ERINNERUNG'); datenSchreiben(d2); }
  }
  if (wert('ANSICHT') !== undefined && wert('REQUEST') === undefined) {   // an der Uhr umgestellt
    var d3 = datenLesen();
    d3.ansicht = wert('ANSICHT') ? 'papier' : 'toni';
    datenSchreiben(d3);
  }
  if (wert('REQUEST')) {
    if (wert('ZAEHLEN')) zaehlen(wert('ZIEL'));
    abfrage(wert('ZIEL'), wert('ANKUNFT') || 0);
  }
});

// ---------- Einstellungen: eingebettete Seite (wie Linie 8) ----------
// Die Uhr beendet sich in Listen nach 30 s ohne Taste; dann schließt die Pebble-App auch die Einstellungen.
// Deshalb meldet das Handy, solange die Seite offen ist.
function einstellungenOffen(offen) {
  var m = {};
  m[keys.EINSTELLUNGEN] = offen ? 1 : 0;
  senden(m);
}

Pebble.addEventListener('showConfiguration', function () {
  einstellungenOffen(true);
  // Standort hier holen und mitgeben: die Seite selbst darf es nicht (Android: nur sichere Herkunft)
  standort(function (f, ort) {          // nur für die Suche in der Nähe, höchstens 4 s warten
    var zustand = datenLesen();
    zustand.standort = ort || null;
    var html = SEITE_HTML.replace('/*ZUSTAND*/null', function () { return JSON.stringify(zustand); });
    Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
  }, 4000);
});

Pebble.addEventListener('webviewclosed', function (e) {
  einstellungenOffen(false);
  if (!e || !e.response) return;
  try {
    var neu = L.bereinigen(JSON.parse(decodeURIComponent(e.response)));
    datenSchreiben(neu);
    einrichtungSenden(true);            // JS_READY: die Uhr fragt mit den neuen Werten neu ab
  } catch (err) {
    console.log('Antwort der Seite unlesbar: ' + err);
  }
});
