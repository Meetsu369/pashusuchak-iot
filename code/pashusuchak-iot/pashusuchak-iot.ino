/*
 * ============================================================
 *   PASHU SUCHAK — ESP8266 (Animal Intrusion Detection)
 *   Triggers ONLY on living things (humans/animals).
 *   Ignores: fans, rolling objects, swinging doors, wind.
 *
 *   LOGIC:
 *   A living thing has BOTH:
 *     1. Heat/IR signature  → PIR = HIGH
 *     2. Physical presence  → Ultrasonic < threshold
 *
 *   Non-living moving objects (fans, balls, doors) move but
 *   have NO body heat → PIR stays LOW → ignored.
 *
 *   We also require the combined signal to persist for
 *   CONFIRM_DURATION ms before triggering, to avoid
 *   single-sample false positives.
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ─── PINS ────────────────────────────────────────────────────
#define PIR     D2    // HC-SR501 passive infrared sensor
#define TRIG    D5    // HC-SR04 ultrasonic trigger
#define ECHO    D6    // HC-SR04 ultrasonic echo
#define LIGHT   D1    // LED / relay for light
#define BUZZER  D7    // Active buzzer

// ─── WIFI & TELEGRAM ─────────────────────────────────────────
const char* ssid     = "OnePlus 9R";
const char* password = "123321123";

#define BOT_TOKEN  "8576519126:AAHPRlTIqGW61_06_H9LG-lwxY58suJYs-w"
#define CHAT_ID    "1242638323"

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ─── TUNABLE SETTINGS ────────────────────────────────────────
/*
 * DETECTION_DISTANCE (cm):
 *   Max range at which the ultrasonic sensor triggers.
 *   HC-SR04 reliable range: 2–400 cm.
 *   150 cm = ~5 feet. Increase to 250 for a larger room.
 */
const int   DETECTION_DISTANCE  = 150;

/*
 * CONFIRM_DURATION (ms):
 *   Both PIR AND ultrasonic must agree continuously for this
 *   long before we accept it as a living thing.
 *   500 ms eliminates single-frame false triggers.
 *   Increase to 800 ms in noisy environments.
 */
const unsigned long CONFIRM_DURATION   = 500;

/*
 * CLEAR_TIMEOUT (ms):
 *   How long after the last detection before we declare
 *   the area clear and silence the buzzer/light.
 *   8000 ms = 8 seconds. Adjust to your preference.
 */
const unsigned long CLEAR_TIMEOUT      = 8000;

/*
 * SAMPLE_INTERVAL (ms):
 *   How often we read sensors. 100 ms = 10 times/second.
 *   Don't go below 60 ms — ultrasonic needs time to settle.
 */
const unsigned long SAMPLE_INTERVAL    = 100;

/*
 * BUZZ_BEEP_MS / BUZZ_PAUSE_MS:
 *   Makes the buzzer beep rhythmically instead of
 *   one annoying continuous tone.
 *   Beep 200 ms ON, 300 ms OFF.
 */
const unsigned long BUZZ_BEEP_MS  = 200;
const unsigned long BUZZ_PAUSE_MS = 300;

/*
 * DIAG_INTERVAL (ms):
 *   Auto daily diagnostic Telegram message.
 *   86400000 ms = 24 hours.
 */
const unsigned long DIAG_INTERVAL = 86400000UL;

// ─── STATE ───────────────────────────────────────────────────
bool    alertActive       = false;  // Currently in alert mode?
unsigned long lastSeenMs  = 0;      // Last time living thing seen
unsigned long confirmStart = 0;     // When dual-confirm began
bool    confirming        = false;  // Waiting to confirm?

unsigned long lastSampleMs = 0;
unsigned long lastDiagMs   = 0;
unsigned long lastBuzzToggle = 0;
bool    buzzState          = false;

// ─── ULTRASONIC: GET DISTANCE (cm) ───────────────────────────
// Returns -1 if no echo (out of range or obstacle too close).
int getDistance() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(4);               // Ensure clean LOW

  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);              // 10 µs pulse
  digitalWrite(TRIG, LOW);

  // Timeout = 25000 µs → max ~425 cm, well beyond our threshold
  long duration = pulseIn(ECHO, HIGH, 25000);

  if (duration == 0) return -1;       // No echo

  int cm = (int)(duration * 0.0343f / 2.0f);

  // Sanity-check: HC-SR04 min reliable distance is 2 cm
  if (cm < 2 || cm > 400) return -1;

  return cm;
}

// ─── BUZZER: NON-BLOCKING BEEP PATTERN ───────────────────────
void handleBuzzer() {
  if (!alertActive) {
    digitalWrite(BUZZER, LOW);
    buzzState = false;
    return;
  }

  unsigned long now = millis();
  unsigned long elapsed = now - lastBuzzToggle;

  if (buzzState && elapsed >= BUZZ_BEEP_MS) {
    digitalWrite(BUZZER, LOW);
    buzzState = false;
    lastBuzzToggle = now;
  } else if (!buzzState && elapsed >= BUZZ_PAUSE_MS) {
    digitalWrite(BUZZER, HIGH);
    buzzState = true;
    lastBuzzToggle = now;
  }
}

// ─── TELEGRAM DIAGNOSTIC ─────────────────────────────────────
void runDiagnostics() {
  int d = getDistance();
  int pir = digitalRead(PIR);

  String msg = "🩺 *SYSTEM STATUS*\n";
  msg += "WiFi: "       + String(WiFi.status() == WL_CONNECTED ? "✅ OK" : "❌ FAIL") + "\n";
  msg += "Signal (RSSI): " + String(WiFi.RSSI()) + " dBm\n";
  msg += "Free Heap: "  + String(ESP.getFreeHeap()) + " bytes\n";
  msg += "PIR now: "    + String(pir ? "HIGH (heat)" : "LOW") + "\n";
  msg += "Distance now: " + (d == -1 ? "No echo" : String(d) + " cm") + "\n";
  msg += "Alert active: " + String(alertActive ? "YES" : "NO") + "\n";
  msg += "Threshold: "  + String(DETECTION_DISTANCE) + " cm\n";

  bot.sendMessage(CHAT_ID, msg, "Markdown");
}

// ─── SETUP ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== PASHU SUCHAK BOOT ===");

  pinMode(PIR,    INPUT);
  pinMode(TRIG,   OUTPUT);
  pinMode(ECHO,   INPUT);
  pinMode(LIGHT,  OUTPUT);
  pinMode(BUZZER, OUTPUT);

  // Safe OFF state at boot
  digitalWrite(TRIG,   LOW);
  digitalWrite(LIGHT,  LOW);
  digitalWrite(BUZZER, LOW);

  // ── WiFi ──
  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK: " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi FAILED — running offline");
  }

  client.setInsecure();  // No SSL cert verification (bot library requirement)

  // ── Warm-up delay for PIR ──
  // HC-SR501 needs 30–60 seconds to calibrate on first power-up.
  // We blink the light to show the user we are warming up.
  Serial.println("PIR warm-up: 30 seconds...");
  for (int i = 0; i < 30; i++) {
    digitalWrite(LIGHT, HIGH);
    delay(500);
    digitalWrite(LIGHT, LOW);
    delay(500);
    Serial.print(30 - i);
    Serial.print("s ");
  }
  Serial.println("\nWarm-up done.");

  bot.sendMessage(CHAT_ID,
    "🚀 *PashuSuchak Online*\n"
    "Watching for animals/humans only.\n"
    "Non-living motion (fans, objects) is ignored.\n"
    "Send `diagnostic` for system status.",
    "Markdown");

  runDiagnostics();
  lastDiagMs = millis();
}

// ─── LOOP ────────────────────────────────────────────────────
void loop() {

  // ── Telegram commands ──
  int newMsgs = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < newMsgs; i++) {
    String txt = bot.messages[i].text;
    txt.toLowerCase();
    txt.trim();

    if (txt == "diagnostic") {
      runDiagnostics();
    } else if (txt == "status") {
      bot.sendMessage(CHAT_ID,
        alertActive ? "⚠ ALERT: Living thing present!" : "✅ Area is clear.",
        "");
    } else if (txt == "test") {
      // Manual test: flash light and buzz once
      digitalWrite(LIGHT, HIGH);
      digitalWrite(BUZZER, HIGH);
      delay(500);
      digitalWrite(LIGHT, LOW);
      digitalWrite(BUZZER, LOW);
      bot.sendMessage(CHAT_ID, "🔔 Test OK — buzzer and light work.", "");
    }
  }

  // ── Non-blocking buzzer pattern ──
  handleBuzzer();

  // ── Sensor sampling (rate-limited) ──
  if (millis() - lastSampleMs < SAMPLE_INTERVAL) return;
  lastSampleMs = millis();

  // ── Read sensors ──
  int pirValue  = digitalRead(PIR);      // HIGH = heat detected
  int distance  = getDistance();         // cm, or -1

  Serial.print("PIR:");
  Serial.print(pirValue);
  Serial.print("  DIST:");
  Serial.print(distance == -1 ? -1 : distance);
  Serial.print(" cm");

  // ── LIVING THING GATE ─────────────────────────────────────
  // Rule: MUST have BOTH heat (PIR HIGH) AND physical presence
  // within threshold. This eliminates:
  //   - Fans:         ultrasonic sees motion, PIR = LOW  → ❌
  //   - Thrown ball:  ultrasonic sees it, PIR = LOW      → ❌
  //   - Swinging door: same                              → ❌
  //   - Human/animal: PIR = HIGH AND distance < thresh   → ✅
  // ──────────────────────────────────────────────────────────
  bool pirSees  = (pirValue == HIGH);
  bool sonarSees = (distance != -1 && distance <= DETECTION_DISTANCE);

  bool livingThing = pirSees && sonarSees;

  Serial.print(livingThing ? "  → LIVING ✅" : "  → not living");

  // ── CONFIRMATION WINDOW ──
  // Require the combined signal for CONFIRM_DURATION ms
  // before treating it as real.
  if (livingThing) {
    if (!confirming) {
      confirming   = true;
      confirmStart = millis();
      Serial.print("  [confirm start]");
    } else if (millis() - confirmStart >= CONFIRM_DURATION) {
      // Confirmed! It's a living thing.
      lastSeenMs = millis();

      if (!alertActive) {
        alertActive = true;

        digitalWrite(LIGHT, HIGH);
        lastBuzzToggle = millis();
        buzzState = true;
        digitalWrite(BUZZER, HIGH);

        Serial.println("\n*** LIVING THING DETECTED ***");
        bot.sendMessage(CHAT_ID,
          "🚨 *Animal/Human Detected!*\n"
          "Distance: " + String(distance) + " cm\n"
          "Heat signature confirmed.",
          "Markdown");
      } else {
        // Refresh the lastSeen timer while still present
        lastSeenMs = millis();
      }
    }
  } else {
    // Signal lost — reset confirmation window
    confirming = false;
  }

  Serial.println();

  // ── CLEAR TIMEOUT ──
  // After the living thing leaves, wait CLEAR_TIMEOUT ms
  // before declaring the area clear.
  if (alertActive && (millis() - lastSeenMs > CLEAR_TIMEOUT)) {
    alertActive = false;
    confirming  = false;

    digitalWrite(LIGHT,  LOW);
    digitalWrite(BUZZER, LOW);
    buzzState = false;

    Serial.println("=== AREA CLEAR ===");
    bot.sendMessage(CHAT_ID, "✅ *Area Clear* — living thing has left.", "Markdown");
  }

  // ── AUTO DAILY DIAGNOSTIC ──
  if (millis() - lastDiagMs >= DIAG_INTERVAL) {
    lastDiagMs = millis();
    runDiagnostics();
  }
}
