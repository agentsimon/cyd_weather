#pragma once

// --- WiFi credentials ---
//#define WIFI_SSID      "SSID"
//#define WIFI_PASSWORD  "Password"

// --- OpenWeatherMap ---
// Get a free API key at https://openweathermap.org/api
// (the free "5 day / 3 hour forecast" tier is all this sketch needs)
//#define WEATHER_API_KEY "FDIJASEDRYOZUB0S"

// --- Location (defaults to Da Nang, Vietnam) ---
#define WEATHER_LAT "16.0544"
#define WEATHER_LON "108.2022"

// --- FreeAstroAPI (Ephemeris) ---
// Get a free API key at https://www.freeastroapi.com/login
#define EPHEMERIS_API_KEY "Your_key"

// --- Kite Wind Alert (email via Gmail SMTP) ---
// EMAIL_SENDER_PASSWORD must be a Gmail App Password, NOT your normal
// account password - create one at https://myaccount.google.com/apppasswords
// (requires 2-Step Verification to be enabled on the Gmail account).
#define EMAIL_SENDER_ACCOUNT  "Your_emaol
#define EMAIL_SENDER_PASSWORD "Your_psswd"
#define EMAIL_RECIPIENT       "The_email"

// Only check wind speed / send an alert during this local-time window
// each day, checked roughly every 30 min (however often the normal
// weather refresh runs). All values in minutes, 24h clock.
// Local timezone offset from UTC (the board's clock is kept in UTC -
// Da Nang, Vietnam is UTC+7, no DST).
#define DANANG_UTC_OFFSET_MIN (7 * 60)
#define KITE_WINDOW_START_MIN 990   // 16:30
#define KITE_WINDOW_END_MIN   1110  // 18:30
