#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WiFi.h>
#include <HTTPClient.h>

// --- NEW: typy ikon ---
enum WeatherIcon { ICON_SUN, ICON_CLOUD, ICON_RAIN, ICON_SNOW, ICON_UNKNOWN };

// ---------- KONFIGURACJA HARDWARE ----------
#define I2C_SDA 21
#define I2C_SCL 22
#define BME_ADDR 0x76
#define OLED_ADDR 0x3C

Adafruit_BME280 bme;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ---------- WI-FI / THINGSPEAK ----------
const char* WIFI_SSID = "TWOJA_SIEC_WIFI";
const char* WIFI_PASS = "TWOJE_HASLO_WIFI";
#define THINGSPEAK_SERVER "http://api.thingspeak.com/update"
#define THINGSPEAK_APIKEY "TWOJ_API_KEY_THINGSPEAK"


// ---------- SUPABASE ----------
#define SUPABASE_URL "https://twoj-projekt.supabase.co/rest/v1/pogoda"
#define SUPABASE_APIKEY "TWOJ_SUPABASE_SERVICE_KEY"


// ---------- PARAMETRY SYSTEMU ----------
const unsigned long READ_INTERVAL_MS = 10000UL;   // odczyt czujnika co 10s
const unsigned long QUEUE_FLUSH_INTERVAL_MS = 10000UL; // wysyłka do ThingSpeak i Supabase co 10 minut
const unsigned long WIFI_BASE_RETRY_MS = 2000UL;
const unsigned long WIFI_MAX_RETRY_MS  = 60000UL;

const int MOVING_AVG_N = 5;    // okno średniej kroczącej

// proste zakresy walidacji
const float TEMP_MIN = -40.0;
const float TEMP_MAX = 85.0;
const float HUM_MIN  = 0.0;
const float HUM_MAX  = 100.0;
const float PRES_MIN = 300.0;
const float PRES_MAX = 1100.0;
const int MAX_INIT_TRIES = 5;


// ---------- BUFFERY I STANY ----------
float tempBuf[MOVING_AVG_N];
float humBuf[MOVING_AVG_N];
float presBuf[MOVING_AVG_N];
int bufIdx = 0;
int bufCount = 0;

unsigned long lastReadMillis = 0;
bool bmeDetected = false;
bool oledOk = false;

float lastGoodTemp = NAN;
float lastGoodHum = NAN;
float lastGoodPres = NAN;

// ekran
int screenMode = 0;
unsigned long lastScreenSwitch = 0;
const unsigned long SCREEN_INTERVAL = 3000UL; // 3s rotacja ekranów

// FIFO kolejka w RAM (prosta, wystarczająca dla projektu studenckiego)
struct DataPoint {
  unsigned long ts; // millis() - timestamp od startu urządzenia
  float t;
  float h;
  float p;
};
const int QUEUE_SZ = 60; // dopasuj, ale pamiętaj o pamięci
DataPoint queueBuf[QUEUE_SZ];
int qHead = 0, qTail = 0, qCount = 0;
unsigned long lastQueueFlushAttempt = 0;

// WiFi backoff
unsigned long wifiLastAttempt = 0;
unsigned long wifiRetryDelay = WIFI_BASE_RETRY_MS;
bool wifiConnectedOnce = false;

// ---------- POMOCNICZE FUNKCJE KOLEJKI ----------
bool queuePush(const DataPoint &d) {
  if (qCount >= QUEUE_SZ) return false;
  queueBuf[qTail] = d;
  qTail = (qTail + 1) % QUEUE_SZ;
  qCount++;
  return true;
}
bool queuePeek(DataPoint &out) {
  if (qCount == 0) return false;
  out = queueBuf[qHead];
  return true;
}
bool queuePop(DataPoint &out) {
  if (qCount == 0) return false;
  out = queueBuf[qHead];
  qHead = (qHead + 1) % QUEUE_SZ;
  qCount--;
  return true;
}
int queueAvailable() { return qCount; }

// ---------- ŚREDNIA KROCZĄCA i WALIDACJA ----------
void pushValue(float *buf, float value) {
  buf[bufIdx] = value;
}
float avgBuffer(const float *buf, int count) {
  if (count <= 0) return NAN;
  float s = 0.0;
  for (int i=0;i<count;i++) s += buf[i];
  return s / count;
}
bool isPhysicallyValid(float t, float h, float p) {
  if (isnan(t) || isnan(h) || isnan(p)) return false;
  if (t < TEMP_MIN || t > TEMP_MAX) return false;
  if (h < HUM_MIN || h > HUM_MAX) return false;
  if (p < PRES_MIN || p > PRES_MAX) return false;
  return true;
}

// ---------- WYŚWIETLANIE ----------
void showSplashScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  for (int x = -128; x <= 5; x += 4) {
    display.clearDisplay();

    // --- tytuł (animowany) ---
    display.setTextSize(2);
    display.setCursor(x, 4);
    display.println("STACJA");
    display.setCursor(x, 20);
    display.println("POGODOWA");

    // --- autorzy ---
    display.setTextSize(1);
    display.setCursor(x, 40);
    display.println("by Hubert Zdanowicz");

    display.setCursor(x, 48);
    display.println("& Michal Tomys");

    display.display();
    delay(12);
  }

  delay(800);
}



// decyduje jaki ikon rysować na podstawie T,H,P (prosty heurystyczny algorytm)
WeatherIcon determineWeatherIcon(float t, float h, float p) {
  if (isnan(t) || isnan(h) || isnan(p)) return ICON_UNKNOWN;
  // priorytet: śnieg jeśli temp <= 0 i wilgotność umiarkowana/wysoka
  if (t <= 0.0 && h > 40.0) return ICON_SNOW;
  // deszcz jeśli wilgotność bardzo wysoka (proste)
  if (h >= 80.0) return ICON_RAIN;
  // chmura jeśli wilgotność średnia-wysoka lub niskie ciśnienie
  if (h >= 60.0 || p < 1000.0) return ICON_CLOUD;
  // słońce dla cieplejszych i suchszych warunków
  if (t >= 20.0 && h < 60.0) return ICON_SUN;
  // inaczej chmurka jako domyślna
  return ICON_CLOUD;
}

// Rysowanie ikon wektorowo
void drawSun(int x, int y, int size) {
  int cx = x + size/2;
  int cy = y + size/2;
  int r = size/3;
  display.fillCircle(cx, cy, r, SSD1306_WHITE);
  // promienie
  for (int a = 0; a < 360; a += 45) {
    float rad = a * 0.01745329;
    int x1 = cx + (r+2) * cos(rad);
    int y1 = cy + (r+2) * sin(rad);
    int x2 = cx + (r+6) * cos(rad);
    int y2 = cy + (r+6) * sin(rad);
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }
}
void drawCloud(int x, int y) {
  // prosta chmurka: trzy wypełnione kółka + prostokąt
  display.fillCircle(x+6, y+8, 7, SSD1306_WHITE);
  display.fillCircle(x+16, y+6, 8, SSD1306_WHITE);
  display.fillCircle(x+28, y+8, 7, SSD1306_WHITE);
  display.fillRect(x+6, y+8, 26, 10, SSD1306_WHITE);
}
void drawRain(int x, int y) {
  drawCloud(x, y);
  // krople
  display.drawLine(x+10, y+20, x+8, y+26, SSD1306_WHITE);
  display.drawLine(x+18, y+20, x+16, y+26, SSD1306_WHITE);
  display.drawLine(x+26, y+20, x+26, y+26, SSD1306_WHITE);
  // małe krople jako kropki
  display.drawPixel(x+12, y+24, SSD1306_WHITE);
  display.drawPixel(x+20, y+26, SSD1306_WHITE);
}
void drawSnow(int x, int y) {
  drawCloud(x, y);
  // proste płatki śniegu jako krzyżyki
  int sx = x+10, sy = y+26;
  display.drawLine(sx-2, sy-2, sx+2, sy+2, SSD1306_WHITE);
  display.drawLine(sx-2, sy+2, sx+2, sy-2, SSD1306_WHITE);
  display.drawLine(sx, sy-4, sx, sy+4, SSD1306_WHITE);
  display.drawLine(sx-4, sy, sx+4, sy, SSD1306_WHITE);

  sx = x+22; sy = y+30;
  display.drawLine(sx-2, sy-2, sx+2, sy+2, SSD1306_WHITE);
  display.drawLine(sx-2, sy+2, sx+2, sy-2, SSD1306_WHITE);
}

void drawWeatherIconAt(float t, float h, float p) {
  WeatherIcon ic = determineWeatherIcon(t, h, p);

  // PRAWY GÓRNY RÓG — 32x32 px
  const int ICON_SIZE = 32;
  const int x = 128 - ICON_SIZE; // 96
  const int y = 0;

  switch(ic) {
    case ICON_SUN:
      drawSun(x, y, 24);
      break;

    case ICON_CLOUD:
      drawCloud(x + 2, y + 8);
      break;

    case ICON_RAIN:
      drawRain(x + 2, y + 8);
      break;

    case ICON_SNOW:
      drawSnow(x + 2, y + 8);
      break;

    default:
      display.drawRect(x + 8, y + 8, 16, 16, SSD1306_WHITE);
      break;
  }
}


void showScreen(int mode, float t, float h, float p) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // rysujemy ikonę pogodową niezależnie od trybu, jeśli mamy dane
  drawWeatherIconAt(t, h, p);
  
  if (mode == 0) {
    display.setTextSize(2); display.setCursor(0,0); display.println("TEMP");
    display.setTextSize(3); display.setCursor(0,32);
    if (!isnan(t)) { display.print(t,1); display.print(" C"); } else display.print("---.-");
  } else if (mode == 1) {
    display.setTextSize(2); display.setCursor(0,0); display.println("WILG");
    display.setTextSize(3); display.setCursor(0,32);
    if (!isnan(h)) { display.print(h,1); display.print(" %"); } else display.print("--.-");
  } else {
    display.setTextSize(2); display.setCursor(0,0); display.println("CISN");
    display.setTextSize(2); display.setCursor(0,36);
    if (!isnan(p)) { display.print(p,1); display.print(" hPa"); } else display.print("----.-");
  }
  display.display();
}

// ---------- INICJALIZACJA BME ----------
bool tryInitBME(int maxTries) {
  int tries = 0;
  while (tries < maxTries) {
    if (bme.begin(BME_ADDR)) {
      Serial.println("BME280 OK");
      return true;
    }
    tries++;
    delay(500);
  }
  return false;
}

// ---------- WIFI  ----------
void wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiRetryDelay = WIFI_BASE_RETRY_MS;
    if (!wifiConnectedOnce) {
      Serial.print("Wi-Fi connected, IP=");
      Serial.println(WiFi.localIP());
      wifiConnectedOnce = true;
    }
    return;
  }
  unsigned long now = millis();
  if (now - wifiLastAttempt < wifiRetryDelay) return;
  wifiLastAttempt = now;
  Serial.print("Próba Wi-Fi: "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiRetryDelay = min(wifiRetryDelay * 2, WIFI_MAX_RETRY_MS);
  wifiConnectedOnce = false;
}

// ---------- WYSYŁKA DO THINGSPEAK (GET) ----------
bool sendToThingSpeak(const DataPoint &d) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(THINGSPEAK_SERVER) + "?api_key=" + THINGSPEAK_APIKEY;
  url += "&field1=" + String(d.t, 2);
  url += "&field2=" + String(d.h, 2);
  url += "&field3=" + String(d.p, 2);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  bool ok = false;
  if (code > 0) {
    if (code >= 200 && code < 300) ok = true;
    else Serial.print("ThingSpeak HTTP code: "), Serial.println(code);
  } else {
    Serial.print("HTTP error: "); Serial.println(http.errorToString(code));
  }
  http.end();
  return ok;
}

// ---------- WYSYŁKA DO SUPABASE ----------

bool sendToSupabase(const DataPoint &d) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(SUPABASE_URL);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_APIKEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_APIKEY);
  http.addHeader("Prefer", "return=minimal");

  String json =
    "{"
    "\"temperatura\":" + String(d.t, 2) + ","
    "\"wilgotnosc\":"  + String(d.h, 2) + ","
    "\"cisnienie\":"  + String(d.p, 2) +
    "}";

  int code = http.POST(json);
  http.end();

  if (code == 201 || code == 204) {
    Serial.println("Supabase: OK");
    return true;
  } else {
    Serial.print("Supabase error: ");
    Serial.println(code);
    return false;
  }
}

// jedna funkcja, dwa backendy

bool sendToAllBackends(const DataPoint &d) {
  bool okTS = sendToThingSpeak(d);
  bool okSB = sendToSupabase(d);

  // punkt uznajemy za wysłany TYLKO jeśli oba się powiodły
  return okTS && okSB;
}


// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);

  Serial.println("\n=== START STACJI ===");

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) showSplashScreen();
  else Serial.println("OLED: brak");

  bmeDetected = tryInitBME(MAX_INIT_TRIES);
  if (!bmeDetected) {
    Serial.println("BME nie wykryty - sprawdz polaczenia");
    if (oledOk) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0,20);
      display.println("BME280 NIE WYKRYTY");
      display.display();
    }
  }

  // zerowanie buforów
  for (int i=0;i<MOVING_AVG_N;i++){ tempBuf[i]=NAN; humBuf[i]=NAN; presBuf[i]=NAN; }
  bufIdx = 0; bufCount = 0;

  // init queue
  qHead = qTail = qCount = 0;
  lastReadMillis = millis();

  // start WiFi w trybie stacji; pierwsza próba będzie w loop()
  WiFi.mode(WIFI_STA);
  wifiLastAttempt = millis() - WIFI_BASE_RETRY_MS;
  wifiRetryDelay = WIFI_BASE_RETRY_MS;
}

// ---------- LOOP ----------
void loop() {
  unsigned long now = millis();

  // 1) Wi-Fi: próbuj łączyć się (non-blocking)
  wifiEnsureConnected();

  // 2) Próba flush kolejki co QUEUE_FLUSH_INTERVAL_MS (jeśli połączony)
  if (WiFi.status() == WL_CONNECTED && (now - lastQueueFlushAttempt >= QUEUE_FLUSH_INTERVAL_MS)) {
    lastQueueFlushAttempt = now;
    if (queueAvailable() > 0) {
      DataPoint front;
      if (queuePeek(front)) {
        if (sendToAllBackends(front)) {
          DataPoint popped;
          queuePop(popped);
          Serial.println("Wysłano do Supabase + ThingSpeak");
        } else {
          Serial.println("Błąd wysyłki (jeden z backendów)");
        }
      }
    }
  }

  // 3) Odczyt czujnika co READ_INTERVAL_MS
  if (bmeDetected && (now - lastReadMillis >= READ_INTERVAL_MS)) {
    lastReadMillis = now;

    float t_raw = bme.readTemperature();
    float h_raw = bme.readHumidity();
    float p_raw = bme.readPressure() / 100.0F; // hPa

    Serial.print("Raw T="); Serial.print(t_raw);
    Serial.print(" H="); Serial.print(h_raw);
    Serial.print(" P="); Serial.println(p_raw);

    // walidacja prostymi regułami
    if (!isPhysicallyValid(t_raw, h_raw, p_raw)) {
      Serial.println("Odrzucono: poza zakresem fizycznym");
      if (oledOk) showScreen(screenMode, NAN, NAN, NAN);
      // nie enqueue, czekamy na następny odczyt
      return;
    }

    // push into moving window
    pushValue(tempBuf, t_raw);
    pushValue(humBuf, h_raw);
    pushValue(presBuf, p_raw);
    bufIdx = (bufIdx + 1) % MOVING_AVG_N;
    if (bufCount < MOVING_AVG_N) bufCount++;

    float t_avg = avgBuffer(tempBuf, bufCount);
    float h_avg = avgBuffer(humBuf, bufCount);
    float p_avg = avgBuffer(presBuf, bufCount);

    lastGoodTemp = t_avg; lastGoodHum = h_avg; lastGoodPres = p_avg;

    Serial.print("AVG T="); Serial.print(t_avg);
    Serial.print(" H="); Serial.print(h_avg);
    Serial.print(" P="); Serial.println(p_avg);

    // przygotuj datapoint i enqueue
    DataPoint dp;
    dp.ts = now;
    dp.t = t_avg;
    dp.h = h_avg;
    dp.p = p_avg;

    if (!queuePush(dp)) {
      Serial.println("Kolejka pelna — punkt odrzucony");
    } else {
      Serial.print("Enqueued, size="); Serial.println(queueAvailable());
    }

    // jeśli online, spróbuj od razu wysłać (ale pamiętaj o limitach: flush używa 15s)
    // tutaj i tak robimy jedynie próbę wysłania najstarszego elementu (może nic nie poszło)
    if (WiFi.status() == WL_CONNECTED) {
      DataPoint front;
      if (queuePeek(front)) {
        if (now - lastQueueFlushAttempt >= QUEUE_FLUSH_INTERVAL_MS) {
          lastQueueFlushAttempt = now;
          if (sendToAllBackends(front)) {
            DataPoint popped;
            queuePop(popped);
            Serial.println("Wysłano natychmiast: TS + Supabase");
          } else {
            Serial.println("Natychmiastowa wysyłka nieudana");
          }
        }
      }
    }


    // rotate screen co SCREEN_INTERVAL
    if (oledOk && (now - lastScreenSwitch >= SCREEN_INTERVAL)) {
      lastScreenSwitch = now;
      screenMode = (screenMode + 1) % 3;
    }
    if (oledOk) showScreen(screenMode, t_avg, h_avg, p_avg);
  }

  // 4) jeśli BME nie wykryty, co READ_INTERVAL_MS próbuj ponownie
  if (!bmeDetected && (now - lastReadMillis >= READ_INTERVAL_MS)) {
    lastReadMillis = now;
    bmeDetected = tryInitBME(1);
    if (!bmeDetected && oledOk) showScreen(screenMode, NAN, NAN, NAN);
  }

  // krótka pauza
  delay(10);
}
