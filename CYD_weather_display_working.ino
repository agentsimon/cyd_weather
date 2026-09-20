/*
  CYD (JC2432W328) - Weather Forecast Graph
  --------------------------------------------------
  Fetches an hourly forecast (temperature, humidity, precipitation, rain,
  pressure) plus current conditions from Open-Meteo (free, no API key
  needed). Shows a main menu of data names; touch one to view its 12h
  graph (or the current-conditions summary), then touch anywhere to
  return to the menu. Data refetches every 30 minutes in the background.

  The menu's "Ephemeris" entry instead calls the FreeAstroAPI ephemeris
  endpoint (https://www.freeastroapi.com/docs/western/ephemeris) for a
  single snapshot - the planetary positions at the exact moment the
  entry is tapped - using the board's NTP-synced clock for the
  timestamp. Requires an EPHEMERIS_API_KEY in secrets.h.

  WiFi setup uses WiFiManager (captive portal) instead of hardcoded
  credentials: on first boot (or if saved credentials fail), the board
  starts an access point named "CYD-Setup". Connect a phone or laptop
  to it, a setup page should open automatically (or browse to the IP
  shown on screen), pick your WiFi network and enter the password.
  Credentials are then saved to flash for future boots.

  Hold your finger on the touchscreen for the first 2 seconds after
  power-on to erase saved WiFi credentials and force the setup portal
  again (e.g. to switch networks). Touch is read directly over I2C
  (CST820 controller) - no extra touch library required.

  Libraries required (Arduino IDE > Tools > Manage Libraries):
    - TFT_eSPI      (already configured for this board via User_Setup.h)
    - Arduino_JSON  (by Arduino, v0.2.x)
    - WiFiManager   (by tzapu)

  Put secrets.h in the SAME FOLDER as this .ino file, with your
  location filled in (WiFi credentials are no longer stored there -
  they're entered via the setup portal and saved to flash instead).

  NOTE ON INCLUDE ORDER: Arduino_JSON's JSONVar.h #defines "typeof" as
  "typeof_" internally, which breaks the ESP32 core's own gpio_ll.h if
  that header hasn't been parsed yet. TFT_eSPI.h pulls in gpio_ll.h, so
  it MUST be included before Arduino_JSON.h below - don't reorder these.
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <Arduino_JSON.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESP_Mail_Client.h> // by Mobizt - install via Library Manager
#include "secrets.h"

TFT_eSPI tft = TFT_eSPI();
WiFiManager wifiManager;

// ---------- LOCATION (set via a small web form, not secrets.h) ----------
// secrets.h's WEATHER_LAT/WEATHER_LON are now only the one-time default
// used the very first time the board boots. After that, whatever's saved
// in flash (via Preferences/NVS) takes over. Change the location any time
// by visiting http://<board-ip>/ in a browser while it's on your network.
Preferences prefs;
WebServer   locationServer(80);
String currentLat = WEATHER_LAT;
String currentLon = WEATHER_LON;

#define BACKLIGHT_PIN 21
// NOTE: GPIO21 backlight control does not work on this board unit -
// confirmed via isolated pin test (screen brightness never changed
// regardless of HIGH/LOW). Likely hard-wired always-on with no
// switching transistor on this clone revision. Left HIGH permanently;
// see the screen-content blanking logic in loop() for the burn-in
// defense used instead.

// ---------- CST820 CAPACITIVE TOUCH (I2C) ----------
#define TOUCH_SDA 33
#define TOUCH_SCL 32
#define CST820_ADDR 0x15

#define REFRESH_INTERVAL_MS (30UL * 60UL * 1000UL)      // refetch data every 30 min
#define RETRY_INTERVAL_MS   (2UL * 60UL * 1000UL)       // retry sooner if we have no valid data yet

float currentTemp = 0;
float currentHumidity = 0;
float currentPrecipitation = 0;
float currentRain = 0;
float currentPressure = 0;
float currentWindSpeed = 0;
int   currentWeatherCode = 0;
String currentWeatherTime = ""; // Open-Meteo's own timestamp for this reading (local, from &timezone=auto)
bool  weatherDataValid = false; // false until the first successful fetch

// ---------- KITE WIND ALERT (email) ----------
// Sends one email per day the first time wind speed reaches this
// threshold. Adjust to taste - ~15 km/h is a reasonable minimum for most
// kites, ~35+ km/h starts getting into "too strong" territory depending
// on the kite.
#define KITE_MIN_WIND_KMH 8.0

// Check/alert window (KITE_WINDOW_START_MIN/END_MIN) and the local
// timezone offset (DANANG_UTC_OFFSET_MIN) used to interpret it are
// defined in secrets.h. The board's own clock is kept in UTC (Ephemeris
// needs UTC timestamps), so the offset converts UTC to Da Nang local time
// just for this window comparison.

SMTPSession smtp;

unsigned long lastFetch = 0;

// Moon phase constants - see the full MOON PHASE comment block further
// down (near drawMenu()) for why these live here rather than there:
// they're used in setup()/loop(), which appear earlier in this file than
// that comment block, and unlike functions, Arduino doesn't auto-forward-
// declare plain variables or #defines.
#define MOON_SYNODIC_DAYS 29.530588
#define MOON_REF_EPOCH 947182440UL // a known New Moon: 2000-01-06 18:14 UTC
#define MOON_SIGN_REFRESH_INTERVAL_MS (4UL * 60UL * 60UL * 1000UL) // every 4 hours
unsigned long lastMoonSignFetch = 0;

// ---------- NTP (needed for the Ephemeris call's timestamp) ----------
// Three servers so a single slow/blocked one doesn't stall the sync -
// the ESP32 core tries them in order.
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.google.com";
const char* NTP_SERVER3 = "time.cloudflare.com";

// ---------- EPHEMERIS (FreeAstroAPI) ----------
// One snapshot fetched fresh each time the Ephemeris menu row is tapped -
// unlike the weather data, this deliberately is NOT cached/refetched on
// REFRESH_INTERVAL_MS, since the whole point is "positions right now".
#define NUM_EPHEMERIS_BODIES 10
const char* EPHEMERIS_BODIES[NUM_EPHEMERIS_BODIES] = {
  "Sun", "Moon", "Mercury", "Venus", "Mars",
  "Jupiter", "Saturn", "Uranus", "Neptune", "Pluto"
};
String ephemerisName[NUM_EPHEMERIS_BODIES];
String ephemerisSign[NUM_EPHEMERIS_BODIES];
float  ephemerisDegree[NUM_EPHEMERIS_BODIES];
bool   ephemerisRetro[NUM_EPHEMERIS_BODIES];
int    ephemerisCount = 0;
bool   ephemerisValid = false;
String ephemerisTimeLabel = ""; // human-readable UTC timestamp for the snapshot shown

// Daily call cap for the Ephemeris API (paid/rate-limited service - keep
// well clear of any per-day quota). Tracked in-memory only, keyed to the
// UTC calendar date from the NTP-synced clock, so it resets automatically
// at UTC midnight; a reboot also resets it since nothing is persisted.
#define EPHEMERIS_MAX_CALLS_PER_DAY 60
int  ephemerisCallsToday = 0;
int  ephemerisCallDayKey = -1;   // encodes the UTC date the counter applies to
bool ephemerisLimitReached = false;

// ---------- MENU / SCREEN STATE ----------
enum AppScreen {
  SCREEN_MENU,
  SCREEN_EPHEMERIS,
  SCREEN_CURRENT
};
AppScreen currentScreen = SCREEN_MENU;

// Weather icon categories, mapped from Open-Meteo's WMO weather_code.
// Declared here (near the top) rather than near the functions that use
// it, because Arduino's auto-generated function prototypes are inserted
// at the very top of the file - if this enum were declared later, the
// prototype for weatherCodeToIcon() would reference it before it exists.
enum WeatherIconType {
  ICON_CLEAR,
  ICON_PARTLY_CLOUDY,
  ICON_CLOUDY,
  ICON_FOG,
  ICON_RAIN,
  ICON_SNOW,
  ICON_THUNDERSTORM
};

const char* MENU_LABELS[2] = {
  "Current Weather", "Ephemeris"
};
const uint16_t MENU_COLORS[2] = {
  TFT_YELLOW, TFT_MAGENTA
};

// A lighter/brighter red than TFT_eSPI's built-in TFT_RED (0xF800), which
// reads too dark against black at small font sizes. Used for the
// retrograde indicator on the Ephemeris screen. RGB(255,90,90) in RGB565.
#define TFT_LIGHT_RED 0xFACB

bool wasTouched = false; // for edge-detecting a new touch-down, not a held touch

// ---------- SCREEN BLANKING (burn-in prevention) ----------
#define SCREEN_TIMEOUT_MS (30UL * 1000UL) // blank after 30s of no touches
unsigned long lastActivityTime = 0;
bool screenBlanked = false;

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  scanI2C(); // diagnostic: lists any I2C devices found, to verify CST820 wiring

  tft.init();
  tft.setRotation(1);

  screenTest();

  // Give a 2-second window right at boot to touch the screen and
  // force a WiFi credentials reset (equivalent to the old BOOT-button hold).
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Touch screen now to");
  tft.setCursor(10, 35);
  tft.println("reset WiFi setup...");

  bool resetRequested = false;
  unsigned long touchWindowStart = millis();
  int checkCount = 0;
  while (millis() - touchWindowStart < 2000) {
    uint8_t fingerNum = readTouchFingerCount();
    checkCount++;
    if (checkCount % 5 == 0) { // print every ~250ms so we can see it's alive without flooding
      Serial.print("Touch check, fingerNum=");
      Serial.println(fingerNum);
    }
    if (fingerNum > 0 && fingerNum != 0xFF) {
      resetRequested = true;
      break;
    }
    delay(50);
  }

  if (resetRequested) {
    Serial.println("Touch detected at boot - erasing saved WiFi credentials.");
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 10);
    tft.println("Resetting WiFi setup...");
    wifiManager.resetSettings();
    delay(500);
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Connecting WiFi...");

  connectWiFi();

  loadLocation();
  locationServer.on("/", handleLocationRoot);
  locationServer.on("/save", HTTP_POST, handleLocationSave);
  locationServer.begin();
  Serial.println("Location config server started on port 80.");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("WiFi connected!");
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 40);
  tft.println("To set your location, visit:");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, 55);
  tft.print("http://");
  tft.println(WiFi.localIP());
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 75);
  tft.printf("Current: %s, %s\n", currentLat.c_str(), currentLon.c_str());
  delay(4000);

  Serial.println("Syncing time via NTP (needed for Ephemeris timestamps)...");
  configTime(0, 0, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3); // UTC, no DST offset - API wants UTC

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Syncing time...");

  // configTime() only *starts* the sync in the background - it doesn't wait
  // for it. Actively poll for up to ~15s so we know (and log) whether it
  // actually succeeded, rather than silently sailing on with no time set.
  struct tm timeinfo;
  bool timeSynced = false;
  unsigned long ntpStart = millis();
  while (millis() - ntpStart < 15000) {
    if (getLocalTime(&timeinfo, 1000)) {
      timeSynced = true;
      break;
    }
    Serial.println("  ...waiting for NTP sync");
  }

  if (timeSynced) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.printf("NTP sync OK: %s UTC\n", buf);
  } else {
    Serial.println("NTP sync FAILED after 15s - Ephemeris won't work until it");
    Serial.println("catches up in the background (or check network/firewall,");
    Serial.println("NTP needs outbound UDP port 123).");
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.println("Time sync failed - Ephemeris");
    tft.println("may not work yet. Continuing...");
    delay(2000);
  }

  bool ok = fetchWeather();
  lastFetch = millis();
  Serial.println(ok ? ">>> Initial fetch OK." : ">>> Initial fetch failed.");

  Serial.println("Fetching initial Moon sign for the menu icon...");
  bool moonOk = fetchEphemeris();
  lastMoonSignFetch = millis();
  Serial.println(moonOk ? ">>> Initial moon sign fetch OK." : ">>> Initial moon sign fetch failed - will retry in the background.");

  currentScreen = SCREEN_MENU;
  drawMenu();
  lastActivityTime = millis();
}

// -----------------------------------------------------------------
// Reads the CST820 touch controller directly over I2C (register 0x02
// holds the current finger count). Returns true if the screen is
// currently being touched.
// -----------------------------------------------------------------
// -----------------------------------------------------------------
// Diagnostic: scans the I2C bus and prints any responding addresses.
// Used to confirm the CST820 touch controller is actually reachable
// at CST820_ADDR (0x15) on the configured SDA/SCL pins.
// -----------------------------------------------------------------
void scanI2C() {
  Serial.println("=== I2C scan ===");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("  Device found at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  No I2C devices found - check TOUCH_SDA/TOUCH_SCL pin numbers.");
  }
  Serial.println("=== I2C scan done ===");
}

bool isTouched() {
  uint8_t fingerNum = readTouchFingerCount();
  return fingerNum > 0 && fingerNum != 0xFF;
}

// -----------------------------------------------------------------
// Reads a touch position from the CST820, starting at register 0x01
// (gesture ID) through 0x06 (Y low byte) in one 6-byte block read.
// Returns true and fills x,y (in SCREEN coordinates, 0-319 x 0-239)
// if a finger is currently down.
//
// CALIBRATION: the CST820 reports raw coordinates in the panel's
// native PORTRAIT orientation (240 x 320), not adjusted for
// setRotation(1) landscape mode - so the axes are swapped, and one
// is inverted, relative to the rotated screen. Direction was
// confirmed by tapping the two screen corners:
//   top-left  (screen 0,0)     -> raw x=207, y=31
//   bottom-right (screen 319,239) -> raw x=76,  y=149
// This confirms screenX derives from raw Y (increasing), and screenY
// derives from raw X (inversely). However, using those exact tapped
// values as the calibration RANGE was too narrow - human taps rarely
// land on the literal edge pixel, so ordinary mid-screen taps were
// saturating past them and clamping to the screen edges. Using the
// panel's full native resolution (0-239 raw X, 0-319 raw Y) as the
// range instead gives proper full-screen coverage.
// -----------------------------------------------------------------
#define TOUCH_NATIVE_X_MAX 239 // native panel short axis (portrait width)
#define TOUCH_NATIVE_Y_MAX 319 // native panel long axis (portrait height)

bool getTouchXY(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(CST820_ADDR);
  Wire.write(0x01);
  uint8_t err = Wire.endTransmission(true); // full stop
  if (err != 0) return false;

  delay(1);

  Wire.requestFrom((int)CST820_ADDR, 6);
  if (Wire.available() < 6) return false;

  Wire.read();                 // gesture ID, unused
  uint8_t fingerNum = Wire.read();
  uint8_t xH = Wire.read();
  uint8_t xL = Wire.read();
  uint8_t yH = Wire.read();
  uint8_t yL = Wire.read();

  if (fingerNum == 0) return false;

  uint16_t rawX = ((xH & 0x0F) << 8) | xL;
  uint16_t rawY = ((yH & 0x0F) << 8) | yL;

  // screenX increases with rawY; screenY increases as rawX DEcreases (inverted)
  long sx = map(rawY, 0, TOUCH_NATIVE_Y_MAX, 0, tft.width() - 1);
  long sy = map(rawX, TOUCH_NATIVE_X_MAX, 0, 0, tft.height() - 1);

  x = constrain(sx, 0, tft.width() - 1);
  y = constrain(sy, 0, tft.height() - 1);
  return true;
}

// -----------------------------------------------------------------
// Reads the CST820's finger-count register (0x02) over I2C.
// Uses a full stop between the register-address write and the
// data read (rather than a repeated start), since some touch
// controllers handle that more reliably. Returns 0xFF on I2C error.
// -----------------------------------------------------------------
uint8_t readTouchFingerCount() {
  Wire.beginTransmission(CST820_ADDR);
  Wire.write(0x02); // "finger number" register
  uint8_t err = Wire.endTransmission(true); // full stop
  if (err != 0) return 0xFF; // I2C error - controller not responding

  delay(1); // brief settle before the follow-up read

  Wire.requestFrom((int)CST820_ADDR, 1);
  if (Wire.available()) {
    return Wire.read();
  }
  return 0xFF;
}

void screenTest() {
  Serial.println("=== Screen self-test ===");
  Serial.printf("tft.width()=%d tft.height()=%d\n", tft.width(), tft.height());

  uint16_t barColors[] = { TFT_BLACK, TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE };
  int numBars = 5;
  int barWidth = tft.width() / numBars;

  for (int i = 0; i < numBars; i++) {
    tft.fillRect(i * barWidth, 0, barWidth, tft.height(), barColors[i]);
  }

  Serial.println("  Showing black/red/green/blue/white bars");
  delay(2000);
  Serial.println("=== Screen self-test complete ===");
}

void loop() {
  locationServer.handleClient(); // serves the location-config page at any time, not just at boot

  if (millis() - lastFetch > REFRESH_INTERVAL_MS ||
      (!weatherDataValid && millis() - lastFetch > RETRY_INTERVAL_MS)) {
    Serial.println("\n=== Periodic refetch ===");
    bool ok = fetchWeather();
    lastFetch = millis();
    Serial.println(ok ? ">>> Refetch OK." : ">>> Refetch failed, will retry sooner than the usual 30 min.");
    if (currentScreen != SCREEN_MENU && !screenBlanked) {
      redrawCurrentScreen(); // refresh whatever's currently on screen with the new data
    }
  }

  if (millis() - lastMoonSignFetch > MOON_SIGN_REFRESH_INTERVAL_MS) {
    // Only refreshes ephemerisSign[1] (Moon) for the menu icon - the full
    // Ephemeris screen still fetches its own always-fresh snapshot on tap,
    // independent of this. Runs silently in the background (no on-screen
    // "fetching" message) since it doesn't disturb whatever's on screen.
    Serial.println("\n=== Periodic Ephemeris refresh (moon sign for menu) ===");
    bool ok = fetchEphemeris();
    lastMoonSignFetch = millis();
    Serial.println(ok ? ">>> Moon sign refresh OK." : ">>> Moon sign refresh failed - will retry next cycle.");
    if (currentScreen == SCREEN_MENU && !screenBlanked) {
      drawMenu(); // refresh the icon if we're sitting on the menu right now
    }
  }

  uint16_t touchX, touchY;
  bool touchedNow = getTouchXY(touchX, touchY);
  bool pressEdge = touchedNow && !wasTouched; // only act on a NEW touch-down, not a held one
  wasTouched = touchedNow;

  if (pressEdge) {
    lastActivityTime = millis();

    if (screenBlanked) {
      // Wake up: any touch while blanked just wakes the display and
      // shows the menu - it doesn't also count as a menu selection.
      Serial.println("Touch detected - waking display.");
      screenBlanked = false;
      currentScreen = SCREEN_MENU;
      drawMenu();
    } else {
      Serial.print("Touch (screen coords): x=");
      Serial.print(touchX);
      Serial.print(" y=");
      Serial.println(touchY);
      if (currentScreen == SCREEN_MENU) {
        selectMenuRow(menuRowFromY(touchY));
      } else {
        currentScreen = SCREEN_MENU;
        drawMenu();
      }
    }
  }

  if (!screenBlanked && millis() - lastActivityTime > SCREEN_TIMEOUT_MS) {
    // NOTE: this board's backlight (GPIO21) doesn't actually respond to
    // software control on this unit (confirmed via isolated pin test) -
    // it's likely hard-wired always-on with no switching transistor.
    // Blanking the screen CONTENT to solid black is the fallback
    // burn-in defense: no static bright shapes sit in one place for
    // hours, even though the backlight itself stays lit throughout.
    Serial.println("No activity for 30s - blanking screen content.");
    tft.fillScreen(TFT_BLACK);
    screenBlanked = true;
  }

  delay(50);
}

// -----------------------------------------------------------------
// Draws the main menu: one full-width touchable row per data type.
// -----------------------------------------------------------------
// -----------------------------------------------------------------
// MOON PHASE (for the main menu icon)
//
// Phase is computed entirely locally from the board's NTP-synced clock -
// no API call needed, so it's always available and free to recompute
// any time. It's independent of the Ephemeris API/its daily call cap.
//
// The Moon's zodiac SIGN, on the other hand, does need the Ephemeris
// API (it requires actual orbital position, not just elapsed time), so
// it's just read from ephemerisSign[1] ("Moon" is always index 1 in
// EPHEMERIS_BODIES) - whatever the last successful fetchEphemeris()
// call found, whether that was a manual tap into the full Ephemeris
// screen or the periodic background refresh in loop(). The sign only
// changes every ~2.3 days, so a refresh every few hours is plenty -
// this deliberately does NOT piggyback on the 30-min weather cycle,
// to keep well clear of the Ephemeris API's 60-calls/day cap.
//
// (MOON_SYNODIC_DAYS, MOON_REF_EPOCH, MOON_SIGN_REFRESH_INTERVAL_MS, and
// lastMoonSignFetch are declared near the top of the file, not here -
// see the comment there for why.)
// -----------------------------------------------------------------
const char* MOON_PHASE_NAMES[8] = {
  "New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
  "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent"
};

// Returns the Moon's age in days (0-29.53, 0=new, ~14.77=full) based on
// the current UTC time, and writes the matching phase name. Returns
// false (and leaves both outputs untouched) if NTP time isn't synced yet.
bool computeMoonPhase(float &outAgeDays, const char* &outPhaseName) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 200)) return false; // short timeout - this gets called on every menu draw

  time_t now = mktime(&timeinfo); // NOTE: mktime treats tm as UTC here since our clock IS UTC (see configTime(0,0,...) in setup)
  double daysSinceRef = (double)(now - (time_t)MOON_REF_EPOCH) / 86400.0;
  double age = fmod(daysSinceRef, MOON_SYNODIC_DAYS);
  if (age < 0) age += MOON_SYNODIC_DAYS;

  int phaseIndex = ((int)round(age / (MOON_SYNODIC_DAYS / 8.0))) % 8;

  outAgeDays = (float)age;
  outPhaseName = MOON_PHASE_NAMES[phaseIndex];
  return true;
}

// Draws a small moon-phase icon centered at (cx, cy) with radius r,
// shading the illuminated portion via the standard "terminator ellipse"
// approximation (good enough for an icon this size; not full lunar theory).
void drawMoonPhaseIcon(int cx, int cy, int r, float ageDays) {
  float phaseFrac = ageDays / MOON_SYNODIC_DAYS;   // 0..1, 0=new, 0.5=full
  float phaseAngle = phaseFrac * 2.0 * PI;          // 0=new, PI=full, 2PI=new again
  bool waxing = (phaseFrac < 0.5);
  float k = cos(phaseAngle);

  tft.fillCircle(cx, cy, r, tft.color565(25, 25, 35)); // unlit disk (dim, not pure black - stays visible against the black background)
  tft.drawCircle(cx, cy, r, TFT_DARKGREY);

  for (int y = -r; y <= r; y++) {
    int halfW = (int)sqrt((float)(r * r - y * y));
    if (halfW <= 0) continue;
    int xLeft  = cx - halfW;
    int xRight = cx + halfW;
    int termOffset = (int)(k * halfW);

    int litLeft, litRight;
    if (waxing) {
      litLeft  = cx + termOffset;
      litRight = xRight;
    } else {
      litLeft  = xLeft;
      litRight = cx - termOffset;
    }
    if (litLeft < xLeft)   litLeft = xLeft;
    if (litRight > xRight) litRight = xRight;
    if (litRight >= litLeft) {
      tft.drawFastHLine(litLeft, cy + y, litRight - litLeft + 1, TFT_WHITE);
    }
  }
}

void drawMenu() {
  tft.fillScreen(TFT_BLACK);
  int rowHeight = tft.height() / 2;

  for (int i = 0; i < 2; i++) {
    int y = i * rowHeight;
    tft.drawRect(0, y, tft.width(), rowHeight, TFT_WHITE);
    tft.setTextColor(MENU_COLORS[i], TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(15, y + rowHeight / 2 - 8);
    tft.println(MENU_LABELS[i]);
  }

  // Weather icon next to "Current Weather" (row 0)
  if (weatherDataValid) {
    drawWeatherIcon(255, rowHeight / 2, 30, currentWeatherCode);
  }

  // Moon phase icon + position next to "Ephemeris" (row 1)
  float moonAge;
  const char* moonPhaseName;
  if (computeMoonPhase(moonAge, moonPhaseName)) {
    int row1CenterY = rowHeight + rowHeight / 2;
    drawMoonPhaseIcon(255, row1CenterY - 10, 26, moonAge);

    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(195, row1CenterY + 25);
    if (ephemerisCount > 1) {
      // Same degree/sign/minute format as the full Ephemeris screen.
      int wholeDeg = (int)ephemerisDegree[1];
      int minutes = (int)round((ephemerisDegree[1] - wholeDeg) * 60.0);
      if (minutes >= 60) { // rounding can push e.g. 10.999 -> 11d60' - carry it
        minutes = 0;
        wholeDeg++;
      }
      tft.printf("%2d %s %02d", wholeDeg, ephemerisSign[1].c_str(), minutes);
    } else {
      tft.print("--");
    }
  }
}

// Maps a touch Y coordinate to one of the 2 menu rows (0-1)
int menuRowFromY(uint16_t y) {
  int rowHeight = tft.height() / 2;
  int row = y / rowHeight;
  if (row < 0) row = 0;
  if (row > 1) row = 1;
  return row;
}

// Switches to the screen for the selected menu row and draws it
void selectMenuRow(int row) {
  switch (row) {
    case 0: currentScreen = SCREEN_CURRENT;   break;
    case 1: currentScreen = SCREEN_EPHEMERIS; break;
  }

  if (currentScreen == SCREEN_EPHEMERIS) {
    // Live snapshot for "right now" - fetch on every visit rather than
    // relying on the 30-minute weather refetch cycle. Show a brief
    // loading message first since the request can take a second or two.
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("Fetching ephemeris...");
    ephemerisValid = fetchEphemeris();
  }

  redrawCurrentScreen();
}

// Dispatches to the right draw function for whatever currentScreen is
void redrawCurrentScreen() {
  switch (currentScreen) {
    case SCREEN_EPHEMERIS: drawEphemeris();            break;
    case SCREEN_CURRENT:   drawCurrentConditions();    break;
    case SCREEN_MENU:      drawMenu();                 break;
  }
}

// -----------------------------------------------------------------
// Location persistence (NVS via Preferences) and the tiny web form
// used to set it. Runs entirely in the background via locationServer -
// visit http://<board-ip>/ any time after WiFi connects.
// -----------------------------------------------------------------

// Reads the saved lat/lon from flash, falling back to secrets.h's
// defaults if nothing has been saved yet (e.g. first boot).
void loadLocation() {
  prefs.begin("weatherapp", true); // read-only
  currentLat = prefs.getString("lat", WEATHER_LAT);
  currentLon = prefs.getString("lon", WEATHER_LON);
  prefs.end();
  Serial.printf("Loaded location: lat=%s, lon=%s\n", currentLat.c_str(), currentLon.c_str());
}

// Validates and persists a new lat/lon. Returns false (and saves nothing)
// if either value doesn't parse as a number or is out of range.
bool saveLocation(const String& lat, const String& lon) {
  char* endLat;
  char* endLon;
  double latVal = strtod(lat.c_str(), &endLat);
  double lonVal = strtod(lon.c_str(), &endLon);
  bool latOk = (endLat != lat.c_str()) && (latVal >= -90.0 && latVal <= 90.0);
  bool lonOk = (endLon != lon.c_str()) && (lonVal >= -180.0 && lonVal <= 180.0);
  if (!latOk || !lonOk) {
    Serial.printf("Rejected invalid location: lat='%s' lon='%s'\n", lat.c_str(), lon.c_str());
    return false;
  }

  prefs.begin("weatherapp", false);
  prefs.putString("lat", lat);
  prefs.putString("lon", lon);
  prefs.end();

  currentLat = lat;
  currentLon = lon;
  Serial.printf("Saved new location: lat=%s, lon=%s\n", lat.c_str(), lon.c_str());
  return true;
}

// GET / - shows a small form pre-filled with the current location.
void handleLocationRoot() {
  String html =
    "<html><head><title>CYD Weather - Location</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px;}"
    "label{font-weight:bold;} input{width:100%;padding:8px;margin:6px 0 16px;"
    "font-size:16px;box-sizing:border-box;} button{padding:10px 20px;font-size:16px;}"
    "</style></head><body>"
    "<h2>Set Weather Location</h2>"
    "<form action='/save' method='POST'>"
    "<label>Latitude (-90 to 90)</label>"
    "<input type='text' name='lat' value='" + currentLat + "'>"
    "<label>Longitude (-180 to 180)</label>"
    "<input type='text' name='lon' value='" + currentLon + "'>"
    "<button type='submit'>Save</button>"
    "</form></body></html>";
  locationServer.send(200, "text/html", html);
}

// POST /save - validates, persists, and immediately refetches weather
// for the new location so the change shows up without waiting for the
// next 30-minute refresh cycle.
void handleLocationSave() {
  if (!locationServer.hasArg("lat") || !locationServer.hasArg("lon")) {
    locationServer.send(400, "text/plain", "Missing lat/lon");
    return;
  }

  String lat = locationServer.arg("lat");
  String lon = locationServer.arg("lon");

  if (!saveLocation(lat, lon)) {
    locationServer.send(400, "text/html",
      "<html><body><h3>Invalid latitude/longitude.</h3>"
      "<p>Latitude must be between -90 and 90, longitude between -180 and 180.</p>"
      "<a href='/'>Back</a></body></html>");
    return;
  }

  locationServer.send(200, "text/html",
    "<html><body><h3>Location saved!</h3>"
    "<p>Lat: " + currentLat + ", Lon: " + currentLon + "</p>"
    "<p>Refetching weather for the new location now...</p>"
    "<a href='/'>Back</a></body></html>");

  bool ok = fetchWeather();
  lastFetch = millis();
  Serial.println(ok ? ">>> Refetch after location change OK." : ">>> Refetch after location change failed.");
  if (currentScreen != SCREEN_MENU) {
    redrawCurrentScreen();
  }
}

void connectWiFi() {
  Serial.println("=== WiFi connect (WiFiManager) ===");

  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(180); // give up and restart after 3 min unconfigured
  wifiManager.setBreakAfterConfig(true);   // return after ONE attempt, success or fail,
                                            // instead of silently re-looping the portal
                                            // forever with no on-screen feedback

  // Tries saved credentials first; if that fails, starts a "CYD-Setup"
  // access point + captive portal for the user to configure WiFi.
  bool connected = wifiManager.autoConnect("CYD-Setup");

  if (connected) {
    Serial.println();
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Signal strength (RSSI): %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println();
    Serial.println("WiFi connection attempt failed (bad password, SSID out of range, or timed out).");
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 10);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.println("WiFi connection");
    tft.println("failed!");
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("");
    tft.println("Check the password was typed");
    tft.println("correctly. Restarting to try");
    tft.println("the setup portal again...");
    delay(4000);
    ESP.restart();
  }
}

// Fires as soon as the user submits credentials in the captive portal,
// right before WiFiManager attempts to connect with them - gives us a
// chance to show something other than the static instructions screen.
void saveConfigCallback() {
  Serial.println("New WiFi credentials submitted - attempting to connect...");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Connecting to");
  tft.println("your network...");
  tft.setTextSize(1);
  tft.setCursor(10, 60);
  tft.println("This may take a few seconds.");
}

// Shown on the TFT while the WiFiManager config portal is active,
// i.e. while waiting for the user to connect and pick a network.
void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered WiFi config mode.");
  Serial.print("Config portal SSID: ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.print("Config portal IP: ");
  Serial.println(WiFi.softAPIP());

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.println("WiFi Setup Needed");

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 45);
  tft.println("1. Connect your phone/laptop to:");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(20, 60);
  tft.println(myWiFiManager->getConfigPortalSSID());

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 80);
  tft.println("2. A setup page should open");
  tft.setCursor(10, 93);
  tft.println("   automatically. If not, browse to:");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(20, 108);
  tft.println(WiFi.softAPIP().toString());

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 128);
  tft.println("3. Choose your WiFi network,");
  tft.setCursor(10, 141);
  tft.println("   enter the password, and Save.");
}

bool fetchWeatherOnce() {
  Serial.println("=== fetchWeatherOnce() ===");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, attempting reconnect...");
    WiFi.reconnect(); // reuse saved credentials rather than re-launching the setup portal
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(300);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Still not connected, aborting fetch.");
      return false;
    }
  }

  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" +
               currentLat + "&longitude=" + currentLon +
               "&current=temperature_2m,relative_humidity_2m,precipitation,rain,pressure_msl,wind_speed_10m,weather_code"
               "&timezone=auto";

  Serial.print("Requesting URL: ");
  Serial.println(url);

  http.begin(url);
  int httpCode = http.GET();
  Serial.printf("HTTP response code: %d\n", httpCode);

  if (httpCode != 200) {
    Serial.println("Non-200 response, dumping body for debugging:");
    Serial.println(http.getString());
    http.end();
    return false;
  }

  // Arduino_JSON parses from a String (no stream/filter support like
  // ArduinoJson has), so pull the whole body first.
  String payload = http.getString();
  http.end();

  Serial.printf("Payload length: %d bytes\n", payload.length());
  Serial.println("First 200 chars of payload:");
  Serial.println(payload.substring(0, 200));

  JSONVar doc = JSON.parse(payload);

  if (JSON.typeof(doc) == "undefined") {
    Serial.println("JSON parse FAILED - JSON.parse returned undefined");
    return false;
  }
  Serial.println("JSON parsed OK");

  JSONVar current = doc["current"];
  if (JSON.typeof(current) == "undefined") {
    Serial.println("Response missing 'current' field.");
    return false;
  }

  currentTemp          = (float)(double)current["temperature_2m"];
  currentHumidity      = (float)(double)current["relative_humidity_2m"];
  currentPrecipitation = (float)(double)current["precipitation"];
  currentRain          = (float)(double)current["rain"];
  currentPressure      = (float)(double)current["pressure_msl"];
  currentWindSpeed     = (float)(double)current["wind_speed_10m"];
  currentWeatherCode   = (int)(double)current["weather_code"];
  currentWeatherTime    = (const char*)current["time"];
  Serial.printf("Current: %.1fC, %.0f%% humidity, %.1fmm precip, %.1fmm rain, %.0fhPa, wind=%.1fkm/h, weather_code=%d, time=%s\n",
                currentTemp, currentHumidity, currentPrecipitation, currentRain, currentPressure, currentWindSpeed, currentWeatherCode,
                currentWeatherTime.c_str());

  return true;
}

// Open-Meteo occasionally returns a transient 503 ("service overloaded")
// under load - retry a couple of times with a short backoff before giving
// up, rather than leaving the screen stuck on stale/zeroed data for a
// full 30-minute cycle over what's usually a few-second blip.
#define WEATHER_FETCH_MAX_ATTEMPTS 3
bool fetchWeather() {
  for (int attempt = 1; attempt <= WEATHER_FETCH_MAX_ATTEMPTS; attempt++) {
    if (attempt > 1) {
      Serial.printf("Retrying weather fetch (attempt %d/%d)...\n", attempt, WEATHER_FETCH_MAX_ATTEMPTS);
      delay(3000 * (attempt - 1)); // 3s, then 6s
    }
    if (fetchWeatherOnce()) {
      weatherDataValid = true;
      checkKiteWindAlert();
      return true;
    }
  }
  Serial.printf("Weather fetch failed after %d attempts.\n", WEATHER_FETCH_MAX_ATTEMPTS);
  weatherDataValid = false;
  return false;
}

// -----------------------------------------------------------------
// Kite wind alert - checks the current wind speed against
// KITE_MIN_WIND_KMH and emails EMAIL_RECIPIENT (via Gmail SMTP) the
// first time it's crossed each day. Uses the same UTC-day-key reset
// trick as the Ephemeris call counter so it only fires once per day
// even though weather refetches every 30 min.
// -----------------------------------------------------------------
void checkKiteWindAlert() {
  Serial.println("=== checkKiteWindAlert() ===");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 500)) {
    Serial.println("  NTP time not available yet - skipping this check.");
    return;
  }

  int utcMinutesOfDay = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int localMinutesOfDay = (utcMinutesOfDay + DANANG_UTC_OFFSET_MIN) % 1440;
  bool inWindow = (localMinutesOfDay >= KITE_WINDOW_START_MIN &&
                    localMinutesOfDay <= KITE_WINDOW_END_MIN);

  Serial.printf("  UTC time: %02d:%02d  ->  Local time: %02d:%02d  (window: %02d:%02d-%02d:%02d, inWindow=%s)\n",
                timeinfo.tm_hour, timeinfo.tm_min,
                localMinutesOfDay / 60, localMinutesOfDay % 60,
                KITE_WINDOW_START_MIN / 60, KITE_WINDOW_START_MIN % 60,
                KITE_WINDOW_END_MIN / 60, KITE_WINDOW_END_MIN % 60,
                inWindow ? "yes" : "no");

  if (!inWindow) {
    Serial.println("  Outside the alert window - skipping.");
    return;
  }

  Serial.printf("  Wind speed: %.1f km/h  (threshold: %.1f km/h)\n",
                currentWindSpeed, KITE_MIN_WIND_KMH);

  if (currentWindSpeed < KITE_MIN_WIND_KMH) {
    Serial.println("  Below threshold - skipping.");
    return;
  }

  Serial.println("  Threshold met and in window - attempting to send email.");

  if (sendKiteAlertEmail()) {
    Serial.println("  >>> Kite alert email sent OK.");
  } else {
    Serial.println("  >>> Kite alert email FAILED to send.");
  }
}

// Callback ESP_Mail_Client uses to report SMTP progress/errors to Serial.
void smtpCallback(SMTP_Status status) {
  Serial.print("  [SMTP] ");
  Serial.println(status.info());
}

bool sendKiteAlertEmail() {
  Serial.println("=== sendKiteAlertEmail() ===");
  Serial.printf("  Sender:    %s\n", EMAIL_SENDER_ACCOUNT);
  Serial.printf("  Recipient: %s\n", EMAIL_RECIPIENT);
  Serial.println("  SMTP host: smtp.gmail.com:465 (SSL)");

  ESP_Mail_Session session;
  session.server.host_name   = "smtp.gmail.com";
  session.server.port        = 465; // SSL
  session.login.email        = EMAIL_SENDER_ACCOUNT;
  session.login.password     = EMAIL_SENDER_PASSWORD;
  session.login.user_domain  = "";

  SMTP_Message message;
  message.sender.name    = "CYD Weather Display";
  message.sender.email   = EMAIL_SENDER_ACCOUNT;
  message.subject        = "Kite alert: wind is up!";
  message.addRecipient("Kite Flyer", EMAIL_RECIPIENT);

  char body[220];
  snprintf(body, sizeof(body),
           "Wind speed is currently %.1f km/h (threshold: %.1f km/h) at %s, %s.\n"
           "Data timestamp: %s\n"
           "Might be a good time to fly a kite!",
           currentWindSpeed, KITE_MIN_WIND_KMH, currentLat.c_str(), currentLon.c_str(),
           currentWeatherTime.c_str());
  message.text.content = body;
  Serial.println("  Message body:");
  Serial.println(body);

  smtp.callback(smtpCallback);

  Serial.println("  Connecting to SMTP server...");
  if (!smtp.connect(&session)) {
    Serial.printf("  SMTP connect FAILED: %s\n", smtp.errorReason().c_str());
    return false;
  }
  Serial.println("  SMTP connected OK. Sending message...");

  bool ok = MailClient.sendMail(&smtp, &message);
  if (ok) {
    Serial.println("  Message sent OK.");
  } else {
    Serial.printf("  Send FAILED: %s\n", smtp.errorReason().c_str());
  }

  Serial.println("  Closing SMTP session.");
  smtp.closeSession();
  return ok;
}

// -----------------------------------------------------------------
// Fetch a single planetary-positions snapshot for right now from
// FreeAstroAPI (https://www.freeastroapi.com/docs/western/ephemeris).
//
// Only "start" is sent (no "end"/"step"), so the API returns one
// snapshot rather than a date-range table - exactly the current
// positions, which is all this screen shows. "start" is the board's
// NTP-synced UTC time, colon-encoded as %%3A since HTTPClient doesn't
// URL-encode query strings for us.
//
// Reads numeric degree_in_sign/sign_abbr/retrograde fields rather than
// the ready-made "position_text" string, since position_text contains
// a Unicode degree symbol (°) that the TFT's built-in fonts can't
// render.
// -----------------------------------------------------------------
bool fetchEphemeris() {
  Serial.println("=== fetchEphemeris() ===");
  ephemerisCount = 0;
  ephemerisLimitReached = false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, attempting reconnect...");
    WiFi.reconnect();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(300);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Still not connected, aborting ephemeris fetch.");
      return false;
    }
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("NTP time not available yet - aborting ephemeris fetch.");
    return false;
  }

  // Reset the counter if the UTC calendar date has rolled over since the
  // last call (encode as YYYY*1000+day-of-year so it's a single int compare).
  int todayKey = (timeinfo.tm_year + 1900) * 1000 + timeinfo.tm_yday;
  if (todayKey != ephemerisCallDayKey) {
    ephemerisCallDayKey = todayKey;
    ephemerisCallsToday = 0;
  }

  if (ephemerisCallsToday >= EPHEMERIS_MAX_CALLS_PER_DAY) {
    Serial.printf("Ephemeris daily call limit reached (%d/%d) - skipping request.\n",
                  ephemerisCallsToday, EPHEMERIS_MAX_CALLS_PER_DAY);
    ephemerisLimitReached = true;
    return false;
  }

  char urlTime[32];    // colon-encoded, for the request
  char displayTime[24]; // plain, for on-screen display
  snprintf(urlTime, sizeof(urlTime), "%04d-%02d-%02dT%02d%%3A%02d%%3A%02dZ",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  snprintf(displayTime, sizeof(displayTime), "%04d-%02d-%02d %02d:%02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  ephemerisTimeLabel = String(displayTime);

  String bodiesParam = "";
  for (int i = 0; i < NUM_EPHEMERIS_BODIES; i++) {
    if (i > 0) bodiesParam += ",";
    bodiesParam += EPHEMERIS_BODIES[i];
  }

  String url = "https://api.freeastroapi.com/api/v1/ephemeris?start=" + String(urlTime) +
               "&bodies=" + bodiesParam;

  // Count the call as soon as we're committed to sending it, so a failed
  // request still counts against the cap - it still cost a call at the API.
  ephemerisCallsToday++;
  Serial.printf("Ephemeris calls today: %d/%d\n", ephemerisCallsToday, EPHEMERIS_MAX_CALLS_PER_DAY);

  Serial.print("Requesting URL: ");
  Serial.println(url);

  HTTPClient http;
  http.begin(url);
  http.addHeader("x-api-key", EPHEMERIS_API_KEY);
  int httpCode = http.GET();
  Serial.printf("HTTP response code: %d\n", httpCode);

  if (httpCode != 200) {
    Serial.println("Non-200 response, dumping body for debugging:");
    Serial.println(http.getString());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  Serial.printf("Payload length: %d bytes\n", payload.length());

  JSONVar doc = JSON.parse(payload);
  if (JSON.typeof(doc) == "undefined") {
    Serial.println("Ephemeris JSON parse FAILED");
    return false;
  }

  JSONVar bodiesObj = doc["data"]["bodies"];
  if (JSON.typeof(bodiesObj) == "undefined") {
    Serial.println("Ephemeris JSON missing 'data.bodies' field.");
    return false;
  }

  for (int i = 0; i < NUM_EPHEMERIS_BODIES; i++) {
    JSONVar body = bodiesObj[EPHEMERIS_BODIES[i]];
    if (JSON.typeof(body) == "undefined") {
      Serial.printf("  Ephemeris response missing body: %s\n", EPHEMERIS_BODIES[i]);
      continue;
    }

    ephemerisName[ephemerisCount]   = String(EPHEMERIS_BODIES[i]);
    ephemerisSign[ephemerisCount]   = (const char*)body["sign_abbr"];
    ephemerisDegree[ephemerisCount] = (float)(double)body["degree_in_sign"];
    ephemerisRetro[ephemerisCount]  = (bool)body["retrograde"];

    Serial.printf("  %-8s %.1f %s%s\n", EPHEMERIS_BODIES[i], ephemerisDegree[ephemerisCount],
                  ephemerisSign[ephemerisCount].c_str(), ephemerisRetro[ephemerisCount] ? " Rx" : "");

    ephemerisCount++;
  }

  Serial.printf("Ephemeris bodies parsed: %d\n", ephemerisCount);
  return ephemerisCount > 0;
}

void drawEphemeris() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 4);
  tft.println("Ephemeris");

  if (ephemerisLimitReached) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setCursor(10, 26);
    tft.println("Daily API limit reached");
    tft.setCursor(10, 40);
    tft.printf("(%d/%d calls used today)", ephemerisCallsToday, EPHEMERIS_MAX_CALLS_PER_DAY);
    tft.setCursor(10, 54);
    tft.println("Resets at UTC midnight.");
    return;
  }

  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 26);
  tft.println(ephemerisTimeLabel + " UTC");

  if (!ephemerisValid || ephemerisCount == 0) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 60);
    tft.println("No data available");
    return;
  }

  int y = 42;
  int rowHeight = 17;
  tft.setTextSize(2);
  for (int i = 0; i < ephemerisCount; i++) {
    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.setCursor(10, y);
    tft.print(ephemerisName[i]);

    // The Moon (index 1) can't actually be retrograde - that's an
    // apparent-motion effect from Earth and another body orbiting the Sun
    // at different speeds, which doesn't apply to a body orbiting Earth
    // directly. Ignore whatever the API says for it, just in case.
    bool showRetro = ephemerisRetro[i] && (i != 1);

    int wholeDeg = (int)ephemerisDegree[i];
    int minutes = (int)round((ephemerisDegree[i] - wholeDeg) * 60.0);
    if (minutes >= 60) { // rounding can push e.g. 10.999 -> 11d60' - carry it
      minutes = 0;
      wholeDeg++;
    }

    char posBuf[24];
    snprintf(posBuf, sizeof(posBuf), "%2d %s %02d%s", wholeDeg,
             ephemerisSign[i].c_str(), minutes, showRetro ? " Rx" : "");

    tft.setTextColor(showRetro ? TFT_LIGHT_RED : TFT_WHITE, TFT_BLACK);
    tft.setCursor(130, y);
    tft.println(posBuf);

    y += rowHeight;
  }

  // Small remaining-calls readout at the bottom so it's easy to see how
  // close the daily cap is without checking the Serial log.
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(10, y + 2);
  tft.printf("%d/%d calls used today", ephemerisCallsToday, EPHEMERIS_MAX_CALLS_PER_DAY);
}

void drawCurrentConditions() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.println("Current Conditions");

  if (!weatherDataValid) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 60);
    tft.println("No data available");
    tft.setTextSize(1);
    tft.setCursor(10, 90);
    tft.println("Weather fetch failed - will retry");
    tft.setCursor(10, 104);
    tft.println("automatically in the background.");
    return;
  }

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(4);
  tft.setCursor(20, 40);
  tft.printf("%.1fC", currentTemp);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(20, 90);
  tft.printf("Humidity:     %.0f %%", currentHumidity);

  tft.setCursor(20, 115);
  tft.printf("Precipitation:%.1f mm", currentPrecipitation);

  tft.setCursor(20, 140);
  tft.printf("Rain:         %.1f mm", currentRain);

  tft.setCursor(20, 165);
  tft.printf("Pressure:     %.0f hPa", currentPressure);

  tft.setCursor(20, 190);
  tft.printf("Wind:         %.1f km/h", currentWindSpeed);

  drawWeatherIcon(265, 60, 40, currentWeatherCode);
}
// -----------------------------------------------------------------
// Maps a WMO weather_code (from Open-Meteo) to a simplified icon
// category. Reference: https://open-meteo.com/en/docs (WMO code table)
// -----------------------------------------------------------------
WeatherIconType weatherCodeToIcon(int code) {
  switch (code) {
    case 0:                          return ICON_CLEAR;
    case 1: case 2:                  return ICON_PARTLY_CLOUDY;
    case 3:                          return ICON_CLOUDY;
    case 45: case 48:                return ICON_FOG;
    case 51: case 53: case 55:
    case 56: case 57:
    case 61: case 63: case 65:
    case 66: case 67:
    case 80: case 81: case 82:       return ICON_RAIN;
    case 71: case 73: case 75:
    case 77: case 85: case 86:       return ICON_SNOW;
    case 95: case 96: case 99:       return ICON_THUNDERSTORM;
    default:                         return ICON_CLOUDY; // unknown code, safe fallback
  }
}

// -----------------------------------------------------------------
// Draws a simple vector weather icon centered at (cx, cy) with the
// given size (roughly the icon's radius in pixels). No image/bitmap
// assets needed - just basic shapes.
// -----------------------------------------------------------------
void drawWeatherIcon(int cx, int cy, int size, int weatherCode) {
  WeatherIconType icon = weatherCodeToIcon(weatherCode);

  switch (icon) {
    case ICON_CLEAR:
      drawSun(cx, cy, size);
      break;

    case ICON_PARTLY_CLOUDY:
      drawSun(cx - size / 3, cy - size / 4, size * 2 / 3);
      drawCloud(cx + size / 4, cy + size / 4, size, TFT_LIGHTGREY);
      break;

    case ICON_CLOUDY:
      drawCloud(cx, cy, size, TFT_LIGHTGREY);
      break;

    case ICON_FOG:
      drawCloud(cx, cy - size / 4, size * 3 / 4, TFT_DARKGREY);
      drawFogLines(cx, cy + size / 3, size);
      break;

    case ICON_RAIN:
      drawCloud(cx, cy - size / 4, size * 3 / 4, TFT_LIGHTGREY);
      drawRainDrops(cx, cy + size / 3, size);
      break;

    case ICON_SNOW:
      drawCloud(cx, cy - size / 4, size * 3 / 4, TFT_LIGHTGREY);
      drawSnowflakes(cx, cy + size / 3, size);
      break;

    case ICON_THUNDERSTORM:
      drawCloud(cx, cy - size / 4, size * 3 / 4, TFT_DARKGREY);
      drawLightningBolt(cx, cy + size / 4, size);
      break;
  }
}

void drawSun(int cx, int cy, int size) {
  int r = size / 2;
  tft.fillCircle(cx, cy, r, TFT_YELLOW);
  // rays
  for (int angle = 0; angle < 360; angle += 45) {
    float rad = angle * PI / 180.0;
    int x1 = cx + (int)(cos(rad) * (r + 4));
    int y1 = cy + (int)(sin(rad) * (r + 4));
    int x2 = cx + (int)(cos(rad) * (r + 10));
    int y2 = cy + (int)(sin(rad) * (r + 10));
    tft.drawLine(x1, y1, x2, y2, TFT_YELLOW);
  }
}

void drawCloud(int cx, int cy, int size, uint16_t color) {
  int r = size / 3;
  tft.fillCircle(cx - r, cy, r, color);
  tft.fillCircle(cx, cy - r / 2, r * 6 / 5, color);
  tft.fillCircle(cx + r, cy, r, color);
  tft.fillRect(cx - r, cy, r * 2, r, color);
}

void drawRainDrops(int cx, int cy, int size) {
  int spacing = size / 3;
  for (int i = -1; i <= 1; i++) {
    int x = cx + i * spacing;
    tft.drawLine(x, cy, x - 3, cy + 10, TFT_CYAN);
  }
}

void drawSnowflakes(int cx, int cy, int size) {
  int spacing = size / 3;
  for (int i = -1; i <= 1; i++) {
    int x = cx + i * spacing;
    tft.drawLine(x - 4, cy + 4, x + 4, cy + 4, TFT_WHITE);
    tft.drawLine(x, cy, x, cy + 8, TFT_WHITE);
    tft.drawLine(x - 3, cy + 1, x + 3, cy + 7, TFT_WHITE);
    tft.drawLine(x + 3, cy + 1, x - 3, cy + 7, TFT_WHITE);
  }
}

void drawFogLines(int cx, int cy, int size) {
  int halfWidth = size / 2;
  for (int i = 0; i < 3; i++) {
    int y = cy + i * 6;
    tft.drawLine(cx - halfWidth, y, cx + halfWidth, y, TFT_LIGHTGREY);
  }
}

void drawLightningBolt(int cx, int cy, int size) {
  int s = size / 4;
  tft.fillTriangle(cx - s / 2, cy, cx + s / 2, cy, cx - s / 4, cy + s, TFT_YELLOW);
  tft.fillTriangle(cx - s / 4, cy + s, cx + s / 4, cy + s, cx, cy + s * 2, TFT_YELLOW);
}