#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DHT.h>
#include <DNSServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LiquidCrystal_I2C.h>
#include <LittleFS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <time.h>

// -------------------- Аппаратные параметры --------------------
constexpr uint8_t LCD_ADDR_PRIMARY = 0x27;
constexpr uint8_t LCD_ADDR_SECONDARY = 0x3F;
constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;
constexpr uint8_t DHT_PIN = D5;      // GPIO14
constexpr uint8_t DHT_TYPE = DHT11;
constexpr uint8_t RESET_BUTTON_PIN = D3;  // GPIO0 (кнопка FLASH на NodeMCU)

// -------------------- Тайминги --------------------
constexpr unsigned long DISPLAY_SWITCH_MS = 4000;
constexpr unsigned long DISPLAY_REFRESH_MS = 300;
constexpr unsigned long DHT_READ_MS = 3000;
constexpr unsigned long WEATHER_UPDATE_MS = 15UL * 60UL * 1000UL;
constexpr unsigned long WIFI_RECONNECT_MIN_MS = 15000;
constexpr unsigned long WIFI_RECONNECT_MAX_MS = 300000;
constexpr unsigned long RESET_BUTTON_ARM_MS = 10000;
constexpr unsigned long RESET_BUTTON_HOLD_MS = 6000;

// -------------------- Wi-Fi / AP --------------------
const char* AP_SSID = "ClockSetup";
const char* AP_PASS = "";  // открытая сеть для удобства первичной настройки
const byte DNS_PORT = 53;

// -------------------- Хранилище --------------------
const char* CONFIG_PATH = "/config.json";

struct AppConfig {
  String ssid;
  String password;
  String city = "Novosibirsk";
  String weatherApiKey;
  int utcOffsetHours = 7;
};

struct WeatherData {
  bool valid = false;
  float tempC = NAN;
  int conditionId = 0;
  String description;
  unsigned long updatedAtMs = 0;
  int lastHttpCode = 0;
  String lastError;
};

AppConfig config;
WeatherData weather;

LiquidCrystal_I2C* lcd = nullptr;
uint8_t detectedLcdAddress = 0;
DHT dht(DHT_PIN, DHT_TYPE);

DNSServer dnsServer;
ESP8266WebServer server(80);
WiFiUDP ntpUdp;
NTPClient timeClient(ntpUdp, "pool.ntp.org", 7 * 3600, 60 * 1000);

bool portalMode = false;
bool serverStarted = false;
bool pendingRestart = false;
unsigned long restartAtMs = 0;
bool otaEnabled = false;
String otaHostname;

float localTempC = NAN;
float localHumidity = NAN;

unsigned long lastDisplaySwitchMs = 0;
unsigned long lastDisplayRefreshMs = 0;
unsigned long lastDhtReadMs = 0;
unsigned long lastWeatherUpdateMs = 0;

uint8_t currentScreen = 0;
unsigned long wifiLastReconnectTryMs = 0;
unsigned long wifiReconnectIntervalMs = WIFI_RECONNECT_MIN_MS;
unsigned long bootMs = 0;
unsigned long resetButtonPressedAtMs = 0;
bool resetTriggered = false;

String htmlEscape(const String& src) {
  String out;
  out.reserve(src.length() + 8);
  for (size_t i = 0; i < src.length(); i++) {
    const char c = src[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

String urlEncode(const String& value) {
  String encoded;
  char hex[4];
  for (size_t i = 0; i < value.length(); i++) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    const bool safe = (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      encoded += static_cast<char>(c);
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", c);
      encoded += hex;
    }
  }
  return encoded;
}

void lcdPrint2(const String& line1, const String& line2) {
  if (!lcd) return;
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(line1.substring(0, LCD_COLS));
  lcd->setCursor(0, 1);
  lcd->print(line2.substring(0, LCD_COLS));
}

bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

uint8_t detectLcdAddress() {
  if (i2cDevicePresent(LCD_ADDR_PRIMARY)) return LCD_ADDR_PRIMARY;
  if (i2cDevicePresent(LCD_ADDR_SECONDARY)) return LCD_ADDR_SECONDARY;

  for (uint8_t addr = 8; addr < 120; addr++) {
    if (i2cDevicePresent(addr)) return addr;
  }
  return 0;
}

bool initLcd() {
  detectedLcdAddress = detectLcdAddress();
  if (detectedLcdAddress == 0) {
    Serial.println("[LCD] I2C устройство не найдено");
    return false;
  }

  lcd = new LiquidCrystal_I2C(detectedLcdAddress, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  Serial.printf("[LCD] Адрес: 0x%02X\n", detectedLcdAddress);
  return true;
}

void scheduleRestart(unsigned long delayMs = 1500) {
  pendingRestart = true;
  restartAtMs = millis() + delayMs;
}

void resetConfigAndRestart() {
  Serial.println("[CFG] Сброс настроек по кнопке");
  LittleFS.remove(CONFIG_PATH);

  config = AppConfig();
  weather = WeatherData();
  otaEnabled = false;

  lcdPrint2("Reset config", "Reboot...");
  scheduleRestart(1800);
}

void checkResetButton() {
  if (millis() - bootMs < RESET_BUTTON_ARM_MS) return;
  if (resetTriggered) return;

  const bool pressed = (digitalRead(RESET_BUTTON_PIN) == LOW);

  if (pressed) {
    if (resetButtonPressedAtMs == 0) {
      resetButtonPressedAtMs = millis();
      Serial.println("[BTN] Кнопка сброса нажата");
    } else if (millis() - resetButtonPressedAtMs >= RESET_BUTTON_HOLD_MS) {
      resetTriggered = true;
      resetConfigAndRestart();
    }
  } else {
    resetButtonPressedAtMs = 0;
  }
}

void setupOta() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (otaEnabled) return;

  otaHostname = String("clock-nsk-") + String(ESP.getChipId(), HEX);
  ArduinoOTA.setHostname(otaHostname.c_str());

  ArduinoOTA.onStart([]() {
    lcdPrint2("OTA update", "Starting...");
    Serial.println("[OTA] Начало обновления");
  });

  ArduinoOTA.onEnd([]() {
    lcdPrint2("OTA update", "Done");
    Serial.println("[OTA] Обновление завершено");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Ошибка: %u\n", error);
    lcdPrint2("OTA error", String((int)error));
  });

  ArduinoOTA.begin();
  otaEnabled = true;
  Serial.printf("[OTA] Готово, hostname: %s\n", otaHostname.c_str());
}

void saveConfig() {
  DynamicJsonDocument doc(512);
  doc["ssid"] = config.ssid;
  doc["password"] = config.password;
  doc["city"] = config.city;
  doc["weatherApiKey"] = config.weatherApiKey;
  doc["utcOffsetHours"] = config.utcOffsetHours;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println("[CFG] Ошибка открытия файла на запись");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("[CFG] Конфигурация сохранена");
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("[CFG] Файл конфигурации не найден, используем значения по умолчанию");
    return;
  }

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    Serial.println("[CFG] Ошибка открытия файла");
    return;
  }

  DynamicJsonDocument doc(512);
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[CFG] Ошибка JSON: %s\n", err.c_str());
    return;
  }

  config.ssid = doc["ssid"] | "";
  config.password = doc["password"] | "";
  config.city = doc["city"] | "Novosibirsk";
  config.weatherApiKey = doc["weatherApiKey"] | "";
  config.utcOffsetHours = doc["utcOffsetHours"] | 7;

  if (config.city.isEmpty()) config.city = "Novosibirsk";
  if (config.utcOffsetHours < -12 || config.utcOffsetHours > 14) config.utcOffsetHours = 7;

  Serial.println("[CFG] Конфигурация загружена");
}

bool connectToWifi(unsigned long timeoutMs = 20000) {
  if (config.ssid.isEmpty()) {
    Serial.println("[WiFi] SSID пустой, подключение пропущено");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());

  lcdPrint2("WiFi connect...", config.ssid);
  Serial.printf("[WiFi] Подключение к %s\n", config.ssid.c_str());

  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] OK, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("[WiFi] Не удалось подключиться");
  return false;
}

String buildSetupPage() {
  String page;
  page.reserve(2800);
  page += F("<!doctype html><html lang='ru'><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Настройка часов</title>");
  page += F("<style>body{font-family:Verdana,Arial,sans-serif;margin:0;background:#f0f4f8;color:#1f2937}"
            "main{max-width:560px;margin:24px auto;padding:20px;background:#fff;border-radius:14px;box-shadow:0 10px 28px rgba(0,0,0,.08)}"
            "h1{font-size:22px;margin:0 0 10px}p{line-height:1.4}label{display:block;font-weight:700;margin-top:12px}"
            "input{width:100%;box-sizing:border-box;padding:11px;margin-top:6px;border:1px solid #cbd5e1;border-radius:10px}"
            "button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:10px;background:#0ea5e9;color:#fff;font-weight:700;font-size:16px}"
            ".card{background:#f8fafc;border:1px solid #e2e8f0;border-radius:10px;padding:10px;margin-top:12px}.mono{font-family:monospace}</style></head><body><main>");
  page += F("<h1>Настройка Wi‑Fi и погоды</h1>");
  page += F("<p>Заполните параметры и нажмите <b>Сохранить</b>. Контроллер перезагрузится и подключится к вашему роутеру.</p>");
  page += F("<form method='post' action='/save'>");

  page += F("<label>SSID Wi‑Fi</label><input name='ssid' required value='");
  page += htmlEscape(config.ssid);
  page += F("'>");

  page += F("<label>Пароль Wi‑Fi</label><input name='password' type='password' value='");
  page += htmlEscape(config.password);
  page += F("'>");

  page += F("<label>Город (OpenWeather)</label><input name='city' required value='");
  page += htmlEscape(config.city);
  page += F("'>");

  page += F("<label>API key OpenWeather</label><input name='apiKey' required value='");
  page += htmlEscape(config.weatherApiKey);
  page += F("'>");

  page += F("<label>Часовой пояс UTC (например, 7)</label><input name='utcOffsetHours' type='number' min='-12' max='14' required value='");
  page += String(config.utcOffsetHours);
  page += F("'>");

  page += F("<button type='submit'>Сохранить</button></form>");
  page += F("<div class='card'><b>Подсказка:</b> API key берется на сайте OpenWeather. После сохранения страница станет недоступна до повторного входа в AP режим.</div>");
  page += F("<div class='card'><b>OTA:</b> после подключения к домашнему Wi-Fi прошивка доступна через Arduino OTA в той же сети.</div>");
  page += F("<div class='card mono'>Статус: <a href='/status'>/status</a></div>");
  page += F("</main></body></html>");
  return page;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", buildSetupPage());
}

void handleStatus() {
  DynamicJsonDocument doc(768);
  doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
  doc["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["portalMode"] = portalMode;
  doc["lcdI2cAddress"] = detectedLcdAddress;
  doc["otaEnabled"] = otaEnabled;
  doc["otaHostname"] = otaHostname;
  doc["city"] = config.city;
  doc["utcOffsetHours"] = config.utcOffsetHours;

  if (!isnan(localTempC)) doc["dhtTempC"] = localTempC;
  if (!isnan(localHumidity)) doc["dhtHumidity"] = localHumidity;

  if (weather.valid) {
    doc["weatherTempC"] = weather.tempC;
    doc["weatherConditionId"] = weather.conditionId;
    doc["weatherDescription"] = weather.description;
  }
  doc["weatherLastHttpCode"] = weather.lastHttpCode;
  doc["weatherLastError"] = weather.lastError;

  String json;
  serializeJsonPretty(doc, json);
  server.send(200, "application/json; charset=utf-8", json);
}

void handleSave() {
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  const String city = server.arg("city");
  const String apiKey = server.arg("apiKey");
  const String utcRaw = server.arg("utcOffsetHours");

  if (ssid.isEmpty() || city.isEmpty() || apiKey.isEmpty() || utcRaw.isEmpty()) {
    server.send(400, "text/plain; charset=utf-8", "Не заполнены обязательные поля");
    return;
  }

  const int utc = utcRaw.toInt();
  if (utc < -12 || utc > 14) {
    server.send(400, "text/plain; charset=utf-8", "UTC должен быть в диапазоне -12..14");
    return;
  }

  config.ssid = ssid;
  config.password = password;
  config.city = city;
  config.weatherApiKey = apiKey;
  config.utcOffsetHours = utc;

  saveConfig();

  server.send(200,
              "text/html; charset=utf-8",
              "<html><body><h2>Сохранено</h2><p>Устройство перезагружается...</p></body></html>");

  scheduleRestart(1500);
}

void beginHttpServer() {
  if (serverStarted) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  serverStarted = true;
}

void startPortalMode() {
  portalMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  beginHttpServer();

  Serial.printf("[AP] Точка доступа: %s\n", AP_SSID);
  Serial.printf("[AP] IP: %s\n", WiFi.softAPIP().toString().c_str());
  lcdPrint2("Setup mode", "192.168.4.1");
}

void stopPortalMode() {
  if (!portalMode) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalMode = false;
}

void setupTimeClient() {
  timeClient.setTimeOffset(config.utcOffsetHours * 3600);
  timeClient.begin();
  timeClient.forceUpdate();
}

void readDhtSensor() {
  const float t = dht.readTemperature();
  const float h = dht.readHumidity();
  if (!isnan(t)) localTempC = t;
  if (!isnan(h)) localHumidity = h;
}

void updateWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (config.weatherApiKey.isEmpty()) return;

  weather.lastHttpCode = 0;
  weather.lastError = "";

  const String url = "http://api.openweathermap.org/data/2.5/weather?q=" +
                     urlEncode(config.city) +
                     "&appid=" + urlEncode(config.weatherApiKey) +
                     "&units=metric&lang=ru";

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[WEATHER] Не удалось начать HTTP-запрос");
    weather.lastError = "http.begin fail";
    return;
  }

  const int code = http.GET();
  weather.lastHttpCode = code;
  if (code != HTTP_CODE_OK) {
    const String payload = http.getString();
    Serial.printf("[WEATHER] Ошибка HTTP: %d\n", code);
    weather.lastError = "HTTP " + String(code);
    if (!payload.isEmpty()) {
      DynamicJsonDocument errDoc(512);
      if (!deserializeJson(errDoc, payload)) {
        const String apiMsg = String((const char*)(errDoc["message"] | ""));
        if (!apiMsg.isEmpty()) {
          weather.lastError = "HTTP " + String(code) + ": " + apiMsg;
        }
      }
    }
    http.end();
    return;
  }

  DynamicJsonDocument doc(2048);
  const DeserializationError err = deserializeJson(doc, http.getString());
  http.end();

  if (err) {
    Serial.printf("[WEATHER] Ошибка JSON: %s\n", err.c_str());
    weather.lastError = "JSON parse fail";
    return;
  }

  weather.tempC = doc["main"]["temp"] | NAN;
  weather.conditionId = doc["weather"][0]["id"] | 0;
  weather.description = String((const char*)(doc["weather"][0]["description"] | ""));
  weather.valid = !isnan(weather.tempC);
  weather.updatedAtMs = millis();
  if (!weather.valid) {
    weather.lastError = "temp is NaN";
    return;
  }

  Serial.printf("[WEATHER] %s: %.1f C, id=%d\n", config.city.c_str(), weather.tempC, weather.conditionId);
}

void renderClockScreen() {
  const time_t epoch = timeClient.getEpochTime();
  if (epoch < 100000) {
    lcdPrint2("Time sync...", "NTP");
    return;
  }

  const tm* t = gmtime(&epoch);
  if (!t) {
    lcdPrint2("Time error", "tm null");
    return;
  }

  char line1[17];
  char line2[17];
  snprintf(line1, sizeof(line1), "%02d.%02d.%04d", t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);
  snprintf(line2, sizeof(line2), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  lcdPrint2(String(line1), String(line2));
}

void renderDhtScreen() {
  if (isnan(localTempC) || isnan(localHumidity)) {
    lcdPrint2("DHT11", "No data");
    return;
  }

  char line1[17];
  char line2[17];
  snprintf(line1, sizeof(line1), "Temp: %4.1f C", localTempC);
  snprintf(line2, sizeof(line2), "Hum : %4.1f %%", localHumidity);
  lcdPrint2(String(line1), String(line2));
}

void renderWeatherScreen() {
  if (config.weatherApiKey.isEmpty()) {
    lcdPrint2("Weather: no key", "Open / for cfg");
    return;
  }

  if (!weather.valid) {
    if (!weather.lastError.isEmpty()) {
      lcdPrint2("Weather error", weather.lastError.substring(0, LCD_COLS));
    } else {
      lcdPrint2("Weather wait...", config.city.substring(0, LCD_COLS));
    }
    return;
  }

  char line1[17];
  char line2[17];
  snprintf(line1, sizeof(line1), "Out: %4.1f C", weather.tempC);
  snprintf(line2, sizeof(line2), "Cond ID: %4d", weather.conditionId);
  lcdPrint2(String(line1), String(line2));
}

void renderCurrentScreen() {
  if (currentScreen == 0) {
    renderClockScreen();
  } else if (currentScreen == 1) {
    renderDhtScreen();
  } else {
    renderWeatherScreen();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  bootMs = millis();

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(D2, D1);
  initLcd();
  lcdPrint2("Clock boot", "ESP8266");

  dht.begin();

  if (!LittleFS.begin()) {
    Serial.println("[FS] Ошибка LittleFS");
  }

  loadConfig();
  beginHttpServer();

  const bool wifiOk = connectToWifi();
  if (!wifiOk) {
    startPortalMode();
  } else {
    stopPortalMode();
    setupTimeClient();
    setupOta();
    updateWeather();
  }

  lastDisplaySwitchMs = millis();
  lastDisplayRefreshMs = 0;
  lastDhtReadMs = 0;
  lastWeatherUpdateMs = millis();
}

void loop() {
  server.handleClient();
  checkResetButton();

  if (portalMode) {
    dnsServer.processNextRequest();
  }

  if (pendingRestart && millis() >= restartAtMs) {
    ESP.restart();
  }

  if (!portalMode && WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    if (!otaEnabled) setupOta();
    ArduinoOTA.handle();
  }

  const unsigned long now = millis();

  if (now - lastDhtReadMs >= DHT_READ_MS) {
    lastDhtReadMs = now;
    readDhtSensor();
  }

  if (!portalMode && WiFi.status() == WL_CONNECTED && now - lastWeatherUpdateMs >= WEATHER_UPDATE_MS) {
    lastWeatherUpdateMs = now;
    updateWeather();
  }

  if (now - lastDisplaySwitchMs >= DISPLAY_SWITCH_MS) {
    lastDisplaySwitchMs = now;
    currentScreen = (currentScreen + 1) % 3;
  }

  if (now - lastDisplayRefreshMs >= DISPLAY_REFRESH_MS) {
    lastDisplayRefreshMs = now;
    renderCurrentScreen();
  }

  if (!portalMode && WiFi.status() != WL_CONNECTED && !config.ssid.isEmpty()) {
    if (now - wifiLastReconnectTryMs >= wifiReconnectIntervalMs) {
      wifiLastReconnectTryMs = now;
      connectToWifi(8000);
      if (WiFi.status() == WL_CONNECTED) {
        wifiReconnectIntervalMs = WIFI_RECONNECT_MIN_MS;
        setupTimeClient();
        setupOta();
        updateWeather();
      } else {
        wifiReconnectIntervalMs = min(wifiReconnectIntervalMs * 2, WIFI_RECONNECT_MAX_MS);
      }
    }
  }
}
