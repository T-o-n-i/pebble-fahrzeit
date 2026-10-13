#!/bin/zsh
# Baut Fahrzeit außerhalb des Quellordners (~/.local/share/fahrzeit-build) und installiert sie.
#   tools/bauen.sh            nur bauen
#   tools/bauen.sh emu        bauen + Emulator emery + Logs
#   tools/bauen.sh uhr <IP>   bauen + über das Handy im WLAN (Entwicklerverbindung in der Pebble-App)
# Die fertige App liegt danach als fahrzeit.pbw neben dem Quellordner, wenn dieser in einem Ordner "dateien"
# liegt (Notizablage, per Sync aufs Handy), sonst in dist/fahrzeit.pbw. Vorlage: tools/bauen.sh aus Linie 8.
set -e
export PATH="$HOME/.local/bin:$PATH"
QUELLE="${0:A:h:h}/"
ZIEL="$HOME/.local/share/fahrzeit-build"
mkdir -p "$ZIEL"
if [ "${QUELLE:h:t}" = "dateien" ]; then
  AUSGABE="${QUELLE}../fahrzeit.pbw"
  PRUEF_ORDNER="${QUELLE:h:h}"
else
  AUSGABE="${QUELLE}dist/fahrzeit.pbw"
  PRUEF_ORDNER="${QUELLE}"
fi
python3 "${QUELLE}tools/seite_erzeugen.py"
# Sperre: ein TomTom-Key darf nie in Quellen, Doku oder App landen (er gehört nur in die Einstellungen am Handy).
SCHLUESSEL="${TOMTOM_SCHLUESSEL_DATEI:-$HOME/.config/tomtom/key}"
if [ -s "$SCHLUESSEL" ] && grep -rqF --exclude-dir=.git -f "$SCHLUESSEL" "$PRUEF_ORDNER" 2>/dev/null; then
  echo "ABBRUCH: TomTom-Key steht in einer Datei unter $PRUEF_ORDNER:"
  grep -rlF --exclude-dir=.git -f "$SCHLUESSEL" "$PRUEF_ORDNER"
  exit 1
fi
# Seit 0.2.0 ohne Clay: eigene Einstellungsseite wie Linie 8, kein npm mehr
rsync -a --delete --exclude build --exclude tools --exclude dist --exclude .git "$QUELLE" "$ZIEL/"
cd "$ZIEL"
rm -rf build   # pebble clean reicht nicht: rsync --delete entfernt die waf-Statusdatei, dann bleibt build/ veraltet
pebble build 2>&1 | grep -v "PebblePacket\|Represents" | tail -3
if [ -s "$SCHLUESSEL" ] && unzip -p build/*.pbw | grep -qaF -f "$SCHLUESSEL"; then
  echo "ABBRUCH: TomTom-Key steckt in der gebauten App — nicht kopiert"
  exit 1
fi
# Quellkarte (Debug) raus: enthält Build-Pfade mit dem Benutzernamen des Rechners, die App braucht sie nicht
zip -q -d build/*.pbw pebble-js-app.js.map >/dev/null 2>&1 || true
if [ -x "$HOME/.local/bin/github-sperre" ] && ! "$HOME/.local/bin/github-sperre" build/*.pbw >/dev/null; then
  echo "ABBRUCH: github-sperre schlägt bei der gebauten App an — nicht kopiert"
  "$HOME/.local/bin/github-sperre" build/*.pbw
  exit 1
fi
mkdir -p "${AUSGABE:h}"
cp build/*.pbw "$AUSGABE"
case "$1" in
  emu) pebble install --emulator emery --logs ;;
  uhr) if [ -n "$2" ]; then pebble install --phone "$2" --logs; else pebble install --cloudpebble --logs; fi ;;
esac
