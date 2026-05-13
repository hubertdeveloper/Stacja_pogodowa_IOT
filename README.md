# Stacja Pogodowa IoT

Projekt stacji pogodowej oparty na **ESP32**, który mierzy temperaturę, wilgotność i ciśnienie z czujnika **BME280**, wyświetla wyniki lokalnie na **OLED SSD1306** oraz wysyła dane do chmury przez **Wi‑Fi**.  
Na podstawie dokumentacji projekt wykorzystuje także **ThingSpeak** do wykresów oraz **Supabase** do zapisu danych i obsługi dodatkowej logiki w chmurze.

## Najważniejsze funkcje

- pomiar temperatury, wilgotności i ciśnienia,
- lokalne wyświetlanie danych na ekranie OLED,
- automatyczne przełączanie ekranów z różnymi pomiarami,
- prosta ikona pogody wyliczana heurystycznie na podstawie T/H/P,
- filtracja danych przez **średnią kroczącą**,
- walidacja odczytów w fizycznych zakresach,
- buforowanie pomiarów w kolejce FIFO, gdy Wi‑Fi jest niedostępne,
- wysyłka danych do **ThingSpeak** i **Supabase**,
- automatyczne ponawianie połączenia Wi‑Fi z rosnącym opóźnieniem.

## Zastosowane komponenty

- **ESP32**
- **BME280** — czujnik temperatury, wilgotności i ciśnienia
- **OLED SSD1306 128x64**
- magistrala **I²C**
- łączność **Wi‑Fi**

## Połączenia sprzętowe

- **SDA** → GPIO **21**
- **SCL** → GPIO **22**
- adres **BME280**: `0x76`
- adres **OLED**: `0x3C`
- zasilanie czujnika i wyświetlacza: **3.3 V**
- wspólna masa wszystkich modułów.

## Jak działa program

Po uruchomieniu system inicjalizuje magistralę I²C, wyświetlacz OLED oraz czujnik BME280. Następnie próbuje połączyć się z siecią Wi‑Fi, a w razie problemów ponawia próbę z backoffem. Odczyty są wykonywane cyklicznie co 10 sekund, sprawdzane pod kątem poprawności, wygładzane średnią kroczącą i zapisywane do kolejki. Dane są wysyłane okresowo do backendów, a punkt pomiarowy jest usuwany z bufora dopiero po skutecznym zapisie po obu stronach.

## Parametry programu

- odczyt czujnika: **co 10 s**
- wysyłka do chmury: **co 10 min**
- średnia krocząca: **5 ostatnich pomiarów**
- rotacja ekranów OLED: **co 3 s**
- zakresy walidacji:
  - temperatura: `-40°C ... 85°C`
  - wilgotność: `0% ... 100%`
  - ciśnienie: `300 hPa ... 1100 hPa`

## Wymagania

- Arduino IDE lub PlatformIO,
- płytka **ESP32**,
- biblioteki:
  - `Wire`
  - `Adafruit_Sensor`
  - `Adafruit_BME280`
  - `Adafruit_GFX`
  - `Adafruit_SSD1306`
  - `WiFi`
  - `HTTPClient`
- konto i konfiguracja w **ThingSpeak**,
- konto i tabela w **Supabase**.

## Konfiguracja

W kodzie należy uzupełnić własne dane:

```cpp
const char* WIFI_SSID = "TWOJA_SIEC_WIFI";
const char* WIFI_PASS = "TWOJE_HASLO_WIFI";

#define THINGSPEAK_APIKEY "TWOJ_API_KEY_THINGSPEAK"
#define SUPABASE_URL "https://twoj-projekt.supabase.co/rest/v1/pogoda"
#define SUPABASE_APIKEY "TWOJ_SUPABASE_SERVICE_KEY"
```

## Uruchomienie

1. Podłącz ESP32, BME280 i OLED zgodnie z opisem wyprowadzeń.
2. Zainstaluj wymagane biblioteki w Arduino IDE.
3. Wklej własne dane Wi‑Fi i klucze API.
4. Wgraj szkic na ESP32.
5. Otwórz monitor portu szeregowego z prędkością **115200**.
6. Sprawdź, czy odczyty pojawiają się na OLED oraz w backendach chmurowych.

## Integracja z chmurą

Projekt zapisuje dane do dwóch serwisów:

- **ThingSpeak** — do wizualizacji wykresów,
- **Supabase** — do przechowywania rekordów i dodatkowej logiki po stronie chmury.

## Uwagi

W projekcie **nie ma pomiaru pyłów PM2.5**, ponieważ używany jest wyłącznie czujnik BME280.

## Licencja
This project is licensed under the MIT License.
