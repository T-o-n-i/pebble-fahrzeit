// Fahrzeit — gemeinsame Logik für das Handy-Skript (index.js) und die Einstellungsseite (einstellungen.html).
// Muster wie Linie 8: tools/seite_erzeugen.py setzt diese Datei an die Marke /*LOGIK*/ der Seite.
//
// Daten (localStorage 'fahrzeit'):
//   key          TomTom-Schlüssel
//   zuschlag     % auf die TomTom-Zeit, puffer: Minuten obendrauf — allgemein, je Ziel abweichend möglich
//   erinnerung   Minuten vor der Abfahrt (0 = aus), an Uhr und Handy einstellbar
//   ansicht      'toni' (Standard, Tonis Original-Skin) oder 'papier' (seit 0.4.0; 'phosphor' bis 0.3.1 wird 'papier')
//                — an Uhr und Handy, gilt auch für diese Seite
//   nid          nächste freie Nummer für Favoriten und Strecken (fest, die Uhr merkt sich ihre Wahl daran)
//   favoriten    [{ id, name, adresse, lat, lon, n, zuschlag, puffer, park }]   Ziel ab Standort
//   strecken     [{ id, name, von, nach, n, zuschlag, puffer, park }]           fester Start → Ziel
//                von/nach: { fav: id } oder { adresse, lat, lon, name }
//   n = wie oft an der Uhr gewählt (Reihenfolge nach Häufigkeit), zuschlag/puffer/park: null = wie allgemein
//   Toleranz gehört zum Ziel: Endet eine Strecke an einem Favoriten, gelten dessen Werte, die der Strecke bleiben null.
//   Nur Strecken zu einer anderen Adresse haben eigene Werte.
function Logik() {
  var MAXF = 20, MAXS = 10;
  var ERINNERUNGEN = [0, 5, 10, 15, 20, 30];

  function leer() {
    return { key: '', zuschlag: 0, puffer: 0, erinnerung: 10, ansicht: 'toni', nid: 1, favoriten: [], strecken: [] };
  }

  // Zahl aus einem Eingabefeld, auch mit Komma; leer oder Unsinn = null
  function zahl(text, min, max) {
    if (text === null || text === undefined || String(text).trim() === '') return null;
    var v = parseFloat(String(text).replace(',', '.'));
    if (isNaN(v)) return null;
    return Math.min(Math.max(v, min), max);
  }

  var TOL = ['zuschlag', 'puffer', 'park'];

  // Wo die Toleranz eines Eintrags steht: bei einer Strecke zu einem Favoriten beim Favoriten, sonst beim Eintrag selbst
  function toleranzVon(d, z) {
    return (z && z.nach && z.nach.fav !== undefined && favorit(d, z.nach.fav)) || z;
  }

  // Zuschlag, Puffer, Parken für ein Ziel: eigener Wert, sonst der allgemeine (Parken allgemein 0)
  function korrektur(ziel, d) {
    function w(eigen, allg) { return eigen === null || eigen === undefined ? (allg || 0) : eigen; }
    var t = toleranzVon(d, ziel);
    return { zuschlag: w(t.zuschlag, d.zuschlag), puffer: w(t.puffer, d.puffer), park: w(t.park, 0) };
  }

  // TomTom-Sekunden → Minuten auf der Uhr
  function minuten(sekunden, k) {
    return Math.round(sekunden * (1 + k.zuschlag / 100) / 60 + k.puffer + k.park);
  }

  function sortDe(a, b) {               // ohne localeCompare (Emulator-JS wirft ICU-Fehler, siehe Linie 8)
    function n(s) { return String(s).toLowerCase().replace(/ä/g, 'ae').replace(/ö/g, 'oe').replace(/ü/g, 'ue').replace(/ß/g, 'ss'); }
    var x = n(a), y = n(b);
    return x < y ? -1 : x > y ? 1 : 0;
  }

  // Index-Reihenfolge nach Häufigkeit, bei Gleichstand nach Name
  function reihe(liste) {
    return liste.map(function (x, i) { return i; }).sort(function (a, b) {
      return (liste[b].n || 0) - (liste[a].n || 0) || sortDe(liste[a].name || '', liste[b].name || '');
    });
  }

  function favorit(d, id) {
    for (var i = 0; i < d.favoriten.length; i++) if (d.favoriten[i].id === id) return d.favoriten[i];
    return null;
  }

  // Ende einer Strecke als Ort { name, adresse, lat, lon } bzw. der Favorit selbst; null = Favorit gelöscht
  function ort(d, ende) {
    if (!ende) return null;
    if (ende.fav !== undefined) return favorit(d, ende.fav);
    return ende;
  }

  function kurzName(o) {
    if (!o) return '?';
    if (o.name) return o.name;
    return String(o.adresse || '?').split(',')[0];
  }

  function streckenName(d, s) {
    return s.name || (kurzName(ort(d, s.von)) + ' > ' + kurzName(ort(d, s.nach)));
  }

  // Auf Bytes kürzen, ohne ein UTF-8-Zeichen zu zerschneiden (wie utf8Kuerzen in Linie 8)
  function utf8Kuerzen(s, max) {
    var out = '', n = 0;
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i), b = c < 0x80 ? 1 : c < 0x800 ? 2 : (c >= 0xD800 && c <= 0xDBFF) ? 4 : 3;
      if (n + b > max) break;
      out += b === 4 ? s.substr(i++, 2) : s.charAt(i);
      n += b;
    }
    return out;
  }

  function uhrName(s) { return utf8Kuerzen(String(s).replace(/[\n|]/g, ' ').trim() || '?', 24); }

  // Liste für die Uhr: "id|F|Name" bzw. "id|S|Name", Favoriten und Strecken je nach Häufigkeit
  function listeText(d) {
    var z = [];
    reihe(d.favoriten).forEach(function (i) { var f = d.favoriten[i]; z.push(f.id + '|F|' + uhrName(f.name || kurzName(f))); });
    reihe(d.strecken).forEach(function (i) { var s = d.strecken[i]; z.push(s.id + '|S|' + uhrName(streckenName(d, s))); });
    return z.join('\n');
  }

  // Favorit löschen; Strecken, die ihn nutzen, behalten eine Kopie des Orts (wie Linie 8), als Ziel auch seine Toleranz
  function favoritLoeschen(d, id) {
    var f = favorit(d, id);
    if (!f) return;
    d.strecken.forEach(function (s) {
      if (s.nach && s.nach.fav === id) TOL.forEach(function (t) { s[t] = f[t]; });
      ['von', 'nach'].forEach(function (k) {
        if (s[k] && s[k].fav === id) s[k] = { name: f.name, adresse: f.adresse, lat: f.lat, lon: f.lon };
      });
    });
    d.favoriten = d.favoriten.filter(function (x) { return x.id !== id; });
  }

  function loeschen(d, id) {
    if (favorit(d, id)) return favoritLoeschen(d, id);
    d.strecken = d.strecken.filter(function (s) { return s.id !== id; });
  }

  function finden(d, id) {
    var f = favorit(d, id);
    if (f) return { typ: 'F', ziel: f };
    for (var i = 0; i < d.strecken.length; i++) if (d.strecken[i].id === id) return { typ: 'S', ziel: d.strecken[i] };
    return null;
  }

  // "50.04885, 8.55722", "50.04885 8.55722" oder mit Dezimalkomma "50,04885; 8,55722" → {lat, lon}, sonst null
  function koordinaten(text) {
    var lat, lon;
    var m = /^\s*(-?\d+(?:\.\d+)?)\s*[,;\s]\s*(-?\d+(?:\.\d+)?)\s*$/.exec(text || '');
    var g = /^\s*(-?\d+,\d+)\s*(?:;\s*|,\s+|\s+)(-?\d+,\d+)\s*$/.exec(text || '');
    if (m) { lat = parseFloat(m[1]); lon = parseFloat(m[2]); }
    else if (g) { lat = parseFloat(g[1].replace(',', '.')); lon = parseFloat(g[2].replace(',', '.')); }
    else return null;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return null;
    return { lat: lat, lon: lon };
  }

  // Suche bei TomTom (Adressen und Orte wie „Parkhaus Kurhaus Wiesbaden“), nahe dem Standort bevorzugt
  function suchUrl(text, key, nahe) {
    return 'https://api.tomtom.com/search/2/search/' + encodeURIComponent(text) + '.json?limit=5&language=de-DE' +
      (nahe ? '&lat=' + nahe.lat + '&lon=' + nahe.lon : '') + '&key=' + encodeURIComponent(key);
  }

  function treffer(json) {
    return ((json && json.results) || []).filter(function (r) { return r.position; }).map(function (r) {
      var a = r.address || {};
      return { name: (r.poi && r.poi.name) || a.streetName || a.municipality || '', adresse: a.freeformAddress || '',
               lat: r.position.lat, lon: r.position.lon };
    });
  }

  // Einstellungen aus 0.1.0 (Clay: Ort 1/2, Puffer je Richtung) als zwei Favoriten übernehmen
  function ausAlt(alt, orte) {
    var d = leer();
    if (!alt) return d;
    d.key = String(alt.TOMTOM_KEY || '').trim();
    d.zuschlag = zahl(alt.SURCHARGE, 0, 100) || 0;
    [['PLACE_A', 'Ort 1', 'BUFFER_BACK', 'A'], ['PLACE_B', 'Ort 2', 'BUFFER_OUT', 'B']].forEach(function (x) {
      var text = String(alt[x[0]] || '').trim();
      if (!text) return;
      var f = { id: d.nid++, name: x[1], adresse: text, lat: null, lon: null, n: 0, zuschlag: null, puffer: zahl(alt[x[2]], 0, 120), park: null };
      var k = koordinaten(text), c = orte && orte[x[3]];
      if (k) { f.lat = k.lat; f.lon = k.lon; }
      else if (c && c.query === text) { f.lat = c.lat; f.lon = c.lon; }
      d.favoriten.push(f);
    });
    return d;
  }

  // Daten von der Seite prüfen und begrenzen
  function bereinigen(x) {
    var d = leer();
    if (!x) return d;
    d.key = String(x.key || '').trim();
    d.zuschlag = zahl(x.zuschlag, 0, 100) || 0;
    d.puffer = zahl(x.puffer, 0, 120) || 0;
    d.erinnerung = ERINNERUNGEN.indexOf(x.erinnerung) >= 0 ? x.erinnerung : 10;
    d.ansicht = x.ansicht === 'papier' || x.ansicht === 'phosphor' ? 'papier' : 'toni';
    d.nid = Math.max(1, parseInt(x.nid, 10) || 1);
    function tol(o, z) {
      z.zuschlag = zahl(o.zuschlag, 0, 100); z.puffer = zahl(o.puffer, 0, 120); z.park = zahl(o.park, 0, 60);
      z.n = Math.max(0, parseInt(o.n, 10) || 0);
      return z;
    }
    function lage(o, z) {
      z.lat = typeof o.lat === 'number' ? o.lat : null; z.lon = typeof o.lon === 'number' ? o.lon : null;
      return z;
    }
    (x.favoriten || []).slice(0, MAXF).forEach(function (f) {
      if (!f || !String(f.adresse || '').trim()) return;
      d.favoriten.push(tol(f, lage(f, { id: parseInt(f.id, 10), name: String(f.name || '').trim(), adresse: String(f.adresse).trim() })));
    });
    function ende(e) {
      if (!e) return null;
      if (e.fav !== undefined) return favorit(d, parseInt(e.fav, 10)) ? { fav: parseInt(e.fav, 10) } : null;
      if (!String(e.adresse || '').trim()) return null;
      return lage(e, { name: String(e.name || '').trim(), adresse: String(e.adresse).trim() });
    }
    (x.strecken || []).slice(0, MAXS).forEach(function (s) {
      if (!s) return;
      var von = ende(s.von), nach = ende(s.nach);
      if (!von || !nach) return;
      d.strecken.push(tol(s, { id: parseInt(s.id, 10), name: String(s.name || '').trim(), von: von, nach: nach }));
    });
    // Strecken zu einem Favoriten: Toleranz gehört dem Favoriten. Eigene Werte aus 0.4.0 gehen an ihn, wenn er dort
    // noch keinen hat (erste Strecke in der Liste gewinnt), danach sind sie bei der Strecke leer.
    d.strecken.forEach(function (s) {
      var f = toleranzVon(d, s);
      if (f === s) return;
      TOL.forEach(function (t) {
        if (f[t] === null && s[t] !== null) f[t] = s[t];
        s[t] = null;
      });
    });
    // Nummern eindeutig halten
    var gesehen = {};
    d.favoriten.concat(d.strecken).forEach(function (z) {
      if (!(z.id > 0) || gesehen[z.id]) z.id = d.nid++;
      gesehen[z.id] = 1;
      if (z.id >= d.nid) d.nid = z.id + 1;
    });
    return d;
  }

  return { MAXF: MAXF, MAXS: MAXS, ERINNERUNGEN: ERINNERUNGEN, TOL: TOL, leer: leer, zahl: zahl, toleranzVon: toleranzVon,
           korrektur: korrektur, minuten: minuten,
           sortDe: sortDe, reihe: reihe, favorit: favorit, ort: ort, kurzName: kurzName, streckenName: streckenName,
           utf8Kuerzen: utf8Kuerzen, listeText: listeText, loeschen: loeschen, finden: finden, koordinaten: koordinaten,
           suchUrl: suchUrl, treffer: treffer, ausAlt: ausAlt, bereinigen: bereinigen };
}
if (typeof module !== 'undefined') module.exports = Logik;
