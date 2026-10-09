module.exports = [
  {
    "type": "heading",
    "defaultValue": "Fahrzeit"
  },
  {
    "type": "text",
    "defaultValue": "Zeigt die aktuelle Fahrzeit mit Verkehr zwischen zwei Orten. Die Daten kommen direkt von TomTom. Dafür braucht es einen kostenlosen API-Key von developer.tomtom.com, ohne Zahlungsdaten."
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "TomTom"
      },
      {
        "type": "input",
        "messageKey": "TOMTOM_KEY",
        "label": "API-Key",
        "defaultValue": "",
        "attributes": {
          "type": "password",
          "autocapitalize": "off",
          "autocorrect": "off"
        }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Orte"
      },
      {
        "type": "text",
        "defaultValue": "Koordinaten wie 50.04885, 8.55722 oder eine Adresse. Eine Adresse wird beim Speichern einmal über die TomTom-Suche aufgelöst. Am genauesten sind Koordinaten auf der Zufahrt, etwa zum Parkhaus: Ein Punkt mitten im Gebäude landet sonst auf der nächstgelegenen Straße, im schlechtesten Fall auf der Autobahn daneben."
      },
      {
        "type": "input",
        "messageKey": "PLACE_A",
        "label": "Ort 1 (Start des Hinwegs)",
        "defaultValue": "",
        "attributes": {
          "placeholder": "z. B. Musterstraße 1, Frankfurt",
          "autocorrect": "off"
        }
      },
      {
        "type": "input",
        "messageKey": "PLACE_B",
        "label": "Ort 2 (Ziel des Hinwegs)",
        "defaultValue": "",
        "attributes": {
          "placeholder": "z. B. 50.04885, 8.55722",
          "autocorrect": "off"
        }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Richtungen"
      },
      {
        "type": "input",
        "messageKey": "LABEL_OUT",
        "label": "Bezeichnung Hinweg (Ort 1 → Ort 2)",
        "defaultValue": "Zur Arbeit",
        "attributes": {
          "maxlength": 20
        }
      },
      {
        "type": "input",
        "messageKey": "LABEL_BACK",
        "label": "Bezeichnung Rückweg (Ort 2 → Ort 1)",
        "defaultValue": "Nach Hause",
        "attributes": {
          "maxlength": 20
        }
      },
      {
        "type": "input",
        "messageKey": "SWITCH_TIME",
        "label": "Rückweg ab",
        "description": "Ab dieser Uhrzeit zeigt die App beim Öffnen den Rückweg. UP/DOWN wechselt die Richtung jederzeit.",
        "defaultValue": "12:00",
        "attributes": {
          "type": "time"
        }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Korrektur"
      },
      {
        "type": "text",
        "defaultValue": "TomTom rechnet eher optimistisch. Angezeigt wird die Fahrzeit von TomTom plus Zuschlag plus Puffer. Der Zuschlag wächst mit der Fahrzeit, etwa für einen gemütlicheren Fahrstil. Der Puffer ist fest, etwa für Parkplatzsuche und Fußweg. Stau und Farbe bleiben bei den Werten von TomTom."
      },
      {
        "type": "input",
        "messageKey": "SURCHARGE",
        "label": "Zuschlag in %",
        "defaultValue": "0",
        "attributes": {
          "type": "number",
          "inputmode": "decimal",
          "min": 0,
          "max": 100,
          "step": "any"
        }
      },
      {
        "type": "input",
        "messageKey": "BUFFER_OUT",
        "label": "Puffer Hinweg in min",
        "defaultValue": "0",
        "attributes": {
          "type": "number",
          "inputmode": "numeric",
          "min": 0,
          "max": 120,
          "step": 1
        }
      },
      {
        "type": "input",
        "messageKey": "BUFFER_BACK",
        "label": "Puffer Rückweg in min",
        "defaultValue": "0",
        "attributes": {
          "type": "number",
          "inputmode": "numeric",
          "min": 0,
          "max": 120,
          "step": 1
        }
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Speichern"
  }
];
