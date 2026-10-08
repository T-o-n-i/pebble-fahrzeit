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
    "type": "submit",
    "defaultValue": "Speichern"
  }
];
