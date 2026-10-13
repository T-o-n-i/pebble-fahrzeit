// Spielt das Handy-Skript (src/pkjs/index.js) in Node durch: nachgebaute Uhr, nachgebautes TomTom (keine echten Abfragen).
// Aufruf: node tools/test/handy_test.js     Beispielorte neutral (Wiesbaden Hbf / Kurhaus).
var fs = require('fs'), path = require('path'), vm = require('vm'), assert = require('assert');
var basis = path.join(__dirname, '../../src/pkjs');
var paket = JSON.parse(fs.readFileSync(path.join(__dirname, '../../package.json'), 'utf8'));
var keys = {}, namen = {}, nr = 10000;
paket.pebble.messageKeys.forEach(function (k) { keys[k] = nr; namen[nr] = k; nr++; });

var speicher = {}, urls = [], uhr = [], lauscher = {}, offeneSeite = null;
var localStorage = { getItem: function (k) { return speicher[k] === undefined ? null : speicher[k]; }, setItem: function (k, v) { speicher[k] = String(v); } };
var HBF = { lat: 50.0707, lon: 8.2436 };
var tomtomAntwort = null;   // function(url) -> {status, json}
function XMLHttpRequest() {}
XMLHttpRequest.prototype.open = function (m, u) { this.url = u; };
XMLHttpRequest.prototype.send = function () {
  var self = this; urls.push(self.url.replace(/key=[^&]*/, 'key=***'));
  setTimeout(function () {
    var a = tomtomAntwort(self.url);
    self.status = a.status; self.responseText = JSON.stringify(a.json || {});
    self.onload();
  }, 5);
};
var Pebble = {
  addEventListener: function (t, f) { lauscher[t] = f; },
  sendAppMessage: function (msg, ok) {
    var m = {}; Object.keys(msg).forEach(function (k) { m[namen[k] || k] = msg[k]; });
    uhr.push(m); setTimeout(ok, 2);
  },
  openURL: function (u) { offeneSeite = u; }
};
var module_ = { exports: {} };
function req(n) {
  if (n === 'message_keys') return keys;
  if (n === './einstellungen_seite') return 'X/*ZUSTAND*/nullX';
  if (n === './logik') return require(path.join(basis, 'logik.js'));
  throw new Error(n);
}
var ctx = { Pebble: Pebble, localStorage: localStorage, XMLHttpRequest: XMLHttpRequest, console: { log: function (s) { if (process.env.LOG) console.log('  [js] ' + s); } },
  setTimeout: setTimeout, JSON: JSON, Math: Math, Date: Date, encodeURIComponent: encodeURIComponent, decodeURIComponent: decodeURIComponent,
  navigator: { geolocation: { getCurrentPosition: function (ok) { setTimeout(function () { ok({ coords: { latitude: HBF.lat, longitude: HBF.lon } }); }, 3); } } },
  require: req, module: module_ };
function warte(ms) { return new Promise(function (r) { setTimeout(r, ms || 60); }); }
function letzte() { return uhr[uhr.length - 1]; }
function senden(p) { var e = {}; Object.keys(p).forEach(function (k) { e[k] = p[k]; }); lauscher.appmessage({ payload: e }); }

tomtomAntwort = function (u) {
  if (u.indexOf('/search/') >= 0) return { status: 200, json: { results: [{ position: { lat: 50.085, lon: 8.245 }, address: { freeformAddress: 'Kurhausplatz 1, 65189 Wiesbaden' } }] } };
  if (u.indexOf('arriveAt=') >= 0 && process.env.OHNE_ARRIVE) return { status: 400 };
  return { status: 200, json: { routes: [{ summary: { travelTimeInSeconds: 1200, trafficDelayInSeconds: 300, lengthInMeters: 5400 } }] } };
};

(async function () {
  // 1. Erster Start nach 0.1.0: Clay-Einstellungen werden zu Favoriten Ort 1 / Ort 2
  speicher['clay-settings'] = JSON.stringify({ TOMTOM_KEY: 'TESTKEY', PLACE_A: '50.0707, 8.2436', PLACE_B: 'Kurhausplatz 1, Wiesbaden', SURCHARGE: '10', BUFFER_OUT: '5', BUFFER_BACK: '0' });
  vm.runInNewContext(fs.readFileSync(path.join(basis, 'index.js'), 'utf8'), ctx);
  lauscher.ready();
  await warte();
  var m = uhr[0];
  assert.strictEqual(m.JS_READY, 1);
  assert.strictEqual(m.LISTE, '1|F|Ort 1\n2|F|Ort 2');
  assert.strictEqual(m.ERINNERUNG, 10);
  assert.strictEqual(m.ANSICHT, 0, 'Standard: Toni');
  var d = JSON.parse(speicher.fahrzeit);
  assert.strictEqual(d.key, 'TESTKEY'); assert.strictEqual(d.zuschlag, 10);
  assert.strictEqual(d.favoriten[1].puffer, 5); assert.strictEqual(d.favoriten[0].lat, 50.0707);
  console.log('ok  Übernahme aus 0.1.0');

  // 2. Abfrage Favorit Ort 2 ab Standort, mit Zählen: Adresse wird einmal gesucht
  senden({ REQUEST: 1, ZIEL: 2, ANKUNFT: 0, ZAEHLEN: 1 });
  await warte(120);
  m = uhr.filter(function (x) { return x.MINUTES !== undefined; }).pop();
  // 1200 s × 1,1 = 22 min + Puffer 5 = 27
  assert.strictEqual(m.ZIEL, 2); assert.strictEqual(m.MINUTES, 27); assert.strictEqual(m.DELAY, 300); assert.strictEqual(m.STATUS, '');
  assert.ok(urls.some(function (u) { return u.indexOf('/search/') >= 0; }));
  assert.ok(urls[urls.length - 1].indexOf('calculateRoute/50.0707,8.2436:50.085,8.245') >= 0);
  assert.strictEqual(JSON.parse(speicher.fahrzeit).favoriten[1].n, 1);
  assert.ok(uhr.some(function (x) { return x.LISTE === '2|F|Ort 2\n1|F|Ort 1'; }), 'Liste nach Häufigkeit');
  console.log('ok  Favorit ab Standort, Häufigkeit, Korrektur');

  // 3. Einstellungsseite: Strecke mit Parkzeit anlegen, speichern
  lauscher.showConfiguration();
  await warte();
  assert.ok(offeneSeite.indexOf('data:text/html') === 0 && decodeURIComponent(offeneSeite).indexOf('"standort"') >= 0);
  d = JSON.parse(speicher.fahrzeit);
  d.strecken.push({ id: d.nid++, name: '', von: { fav: 2 }, nach: { name: 'Kollege', adresse: '50,07; 8,24' }, n: 0, zuschlag: null, puffer: '3', park: '10' });
  lauscher.webviewclosed({ response: encodeURIComponent(JSON.stringify(d)) });
  await warte();
  m = letzte();
  assert.strictEqual(m.JS_READY, 1);
  assert.strictEqual(m.LISTE, '2|F|Ort 2\n1|F|Ort 1\n3|S|Ort 2 > Kollege');
  senden({ REQUEST: 1, ZIEL: 3, ANKUNFT: 0 });
  await warte(120);
  m = uhr.filter(function (x) { return x.MINUTES !== undefined; }).pop();
  // 1200 s × 1,1 = 22 + Puffer 3 + Parken 10 = 35
  assert.strictEqual(m.ZIEL, 3); assert.strictEqual(m.MINUTES, 35);
  console.log('ok  Strecke mit eigener Toleranz');

  // 4. Ankunft weit weg: arriveAt; abgelehnt: Rückfall auf jetzt
  var ank = Math.floor(Date.now() / 1000) + 3 * 3600;
  urls = [];
  senden({ REQUEST: 1, ZIEL: 1, ANKUNFT: ank });
  await warte(120);
  assert.ok(urls.some(function (u) { return /arriveAt=\d{4}-\d\d-\d\dT\d\d%3A\d\d%3A\d\dZ/.test(u); }), urls.join('\n'));
  process.env.OHNE_ARRIVE = '1'; urls = [];
  senden({ REQUEST: 1, ZIEL: 1, ANKUNFT: ank });
  await warte(120);
  assert.strictEqual(urls.length, 2); assert.ok(urls[1].indexOf('arriveAt') < 0);
  assert.strictEqual(letzte().MINUTES, 22);
  delete process.env.OHNE_ARRIVE;
  urls = [];
  senden({ REQUEST: 1, ZIEL: 1, ANKUNFT: Math.floor(Date.now() / 1000) + 3600 });
  await warte(120);
  assert.ok(urls[0].indexOf('arriveAt') < 0, 'unter 90 min: Verkehr jetzt');
  console.log('ok  Ankunftszeit (arriveAt ab 90 min, Rückfall)');

  // 5. Löschen an der Uhr: Favorit 2 weg, Strecke behält Kopie; Erinnerung von der Uhr
  senden({ LOESCHEN: 2 });
  senden({ ERINNERUNG: 15 });
  senden({ ANSICHT: 1 });
  await warte();
  d = JSON.parse(speicher.fahrzeit);
  assert.strictEqual(d.favoriten.length, 1); assert.strictEqual(d.strecken[0].von.adresse, 'Kurhausplatz 1, Wiesbaden');
  assert.strictEqual(d.strecken[0].von.lat, 50.085); assert.strictEqual(d.erinnerung, 15); assert.strictEqual(d.ansicht, 'papier');
  assert.strictEqual(letzte().LISTE, '1|F|Ort 1\n3|S|Ort 2 > Kollege');
  senden({ REQUEST: 1, ZIEL: 2, ANKUNFT: 0 });
  await warte();
  assert.strictEqual(letzte().STATUS, 'Ziel gibt es nicht mehr');
  console.log('ok  Löschen an der Uhr, Erinnerung, Ansicht');
  // 0.4.0: gespeicherte Ansicht 'phosphor' aus 0.3.1 wird Papier (ANSICHT 1)
  var alt = JSON.parse(speicher.fahrzeit); alt.ansicht = 'phosphor'; speicher.fahrzeit = JSON.stringify(alt);
  uhr.length = 0; lauscher.ready(); await warte();
  assert.ok(uhr.some(function (m) { return m.ANSICHT === 1; }), 'Phosphor aus 0.3.1 wird Papier');
  console.log('ok  Ansicht Phosphor aus 0.3.1 wird Papier');

  // 6. Ohne Schlüssel
  d.key = ''; speicher.fahrzeit = JSON.stringify(d);
  senden({ REQUEST: 1, ZIEL: 1, ANKUNFT: 0 });
  await warte();
  assert.strictEqual(letzte().STATUS, 'API-Key fehlt: Einstellungen');
  console.log('ok  ohne Schlüssel');
  var groesste = Math.max.apply(null, uhr.map(function (x) { return JSON.stringify(x).length; }));
  console.log('größte Nachricht ~' + groesste + ' Zeichen');
})().catch(function (e) { console.log('FEHLER ' + e.stack); process.exit(1); });
