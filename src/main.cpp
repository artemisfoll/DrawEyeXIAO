#include <Arduino.h>
#include <U8g2lib.h>
#include <Pushbutton.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

#include "config.h"

// ══════════════════════════════════════════════════════════════════════
//  Tipos
// ══════════════════════════════════════════════════════════════════════
enum EyeType {
  EYE_NEUTRO = 0,
  EYE_FELIZ,
  EYE_TRISTE,
  EYE_MALVADO,
  EYE_DORMIDO,
  EYE_CANSADO,
  EYE_FURIA,
  EYE_BLINK,
  EYE_RASCAL,
  EYE_COUNT
};

const char* eyeNames[] = {
  "neutro", "feliz", "triste", "malvado",
  "dormido", "cansado", "furia", "blink", "rascal"
};

// Modos especiais do botão físico
enum SpecialMode {
  MODE_NONE         = 0,
  MODE_CLOCK        = 1,   // 1º click botão → hora
  MODE_FOCUS_SETUP  = 2,   // 2º click botão → configura timer foco
  MODE_FOCUS_RUN    = 3,   // botão longo no modo setup → inicia foco
  MODE_WEB_INFO     = 4,   // 3º click botão → como acessar o modo web
  MODE_WEATHER      = 9,   // touch no modo clock → clima
};

// ══════════════════════════════════════════════════════════════════════
//  Objetos globais
// ══════════════════════════════════════════════════════════════════════
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

Pushbutton button(BUTTON_PIN);
Pushbutton touch(TOUCH_PIN);

WebServer  server(WEB_SERVER_PORT);
DNSServer  dnsServer;
const IPAddress AP_IP(192, 168, 4, 1);

// ── Servo ─────────────────────────────────────────────────────────────

Servo  headServo;
float  servoCurrentAngle = SERVO_CENTER;
int    servoCenterCfg    = SERVO_CENTER;  // ajustável via /servo (painel web)
int    servoRangeCfg     = SERVO_RANGE;   // ajustável via /servo (painel web)

void servoWrite(int angle) {
  angle = constrain(angle, 0, 180);
  headServo.write(angle);
}

Preferences prefs;  // NVS — persiste SSID/senha entre boots

// ══════════════════════════════════════════════════════════════════════
//  Estado global — animação
// ══════════════════════════════════════════════════════════════════════
float currentX = 0, currentY = 0;
float targetX  = 0, targetY  = 0;
unsigned long lastMoveChange = 0;

EyeType currentEye  = EYE_NEUTRO;
EyeType previousEye = EYE_NEUTRO;
EyeType forcedEye   = EYE_COUNT;

unsigned long stateTimer    = 0;
unsigned long stateDuration = 0;
unsigned long blinkTimer    = 0;
unsigned long nextBlinkIn   = 0;
bool blinking = false;

const EyeType RANDOM_POOL[] = {
  EYE_FELIZ, EYE_FURIA, EYE_MALVADO, EYE_BLINK, EYE_RASCAL
};
const byte RANDOM_POOL_SIZE = sizeof(RANDOM_POOL) / sizeof(RANDOM_POOL[0]);

SpecialMode   specialMode     = MODE_NONE;
unsigned long specialModeEnd  = 0;
bool          sleepMode       = false;  // modo sono ativo

// ── Modo foco ─────────────────────────────────────────────────────────
int           focusMinutes    = 5;          // tempo selecionado (minutos)
unsigned long focusEndMs      = 0;          // millis() quando o foco termina
unsigned long btnPressStart   = 0;          // para detectar clique longo
bool          btnHeld         = false;      // botão está pressionado

// ── Estado Wi-Fi ──────────────────────────────────────────────────────
bool wifiConnected    = false;
bool webWifiConnected = false;
String savedSSID      = "";
String savedPass      = "";

// ── Credenciais NVS (declaradas aqui, após savedSSID/savedPass) ─────────
void saveCredentials(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

bool loadCredentials() {
  prefs.begin("wifi", true);
  savedSSID = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
  return savedSSID.length() > 0;
}

void clearCredentials() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  savedSSID = "";
  savedPass = "";
}

// ── Ajuste do servo (centro/amplitude) — persistido em NVS ──────────────
void loadServoConfig() {
  prefs.begin("servo", true);
  servoCenterCfg = prefs.getInt("center", SERVO_CENTER);
  servoRangeCfg  = prefs.getInt("range",  SERVO_RANGE);
  prefs.end();
}

void saveServoConfig() {
  prefs.begin("servo", false);
  prefs.putInt("center", servoCenterCfg);
  prefs.putInt("range",  servoRangeCfg);
  prefs.end();
}

// ── Cache de hora / clima ─────────────────────────────────────────────
bool          ntpSynced    = false;
int           cachedHour   = 0;
int           cachedMin    = 0;
int           cachedDay    = 0;
int           cachedMonth  = 0;
int           cachedWDay   = 0;   // 0=Dom … 6=Sab
unsigned long lastNtpSync  = 0;
#define NTP_RESYNC_INTERVAL  3600000UL  // 1h

float  cachedTemp    = 0;
int    cachedWCode   = 0;   // WMO weather code
bool   weatherReady  = false;
unsigned long lastWeatherFetch = 0;
#define WEATHER_RESYNC_INTERVAL 1800000UL  // 30min

// Barueri, SP
#define LOC_LAT   "-23.5114"
#define LOC_LON   "-46.8756"
#define LOC_CITY  "Barueri"
#define NTP_SERVER "pool.ntp.org"

// ══════════════════════════════════════════════════════════════════════
//  Helpers de tempo (animação)
// ══════════════════════════════════════════════════════════════════════
unsigned long neutralDuration()    { return random(NEUTRAL_DURATION_MIN, NEUTRAL_DURATION_MAX); }
unsigned long expressionDuration() { return random(EXPR_DURATION_MIN,    EXPR_DURATION_MAX);    }
unsigned long nextBlinkDelay()     { return random(BLINK_INTERVAL_MIN,   BLINK_INTERVAL_MAX);   }

// ══════════════════════════════════════════════════════════════════════
//  Buzzer
// ══════════════════════════════════════════════════════════════════════
void beep(int freq = BUZZER_FREQ_LOW, int dur = BUZZER_DURATION) {
  tone(BUZZER_PIN, freq, dur);
}

// ══════════════════════════════════════════════════════════════════════
//  Tela "pensando" — olhos rascal + balão de pensamento animado
//  (usada no lugar de textos de "buscando..." durante sincronizações)
// ══════════════════════════════════════════════════════════════════════
void drawThinking() {
  u8g2.clearBuffer();

  int lPosX = EYE_LEFT, rPosX = EYE_RIGHT, posY = EYE_CENTER_Y;

  // Olhos rascal (mesmo desenho da expressão EYE_RASCAL)
  for (int i = 0; i < EYE_THICKNESS; i++) {
    u8g2.drawArc(lPosX, posY, EYE_SIZE_X + i, 130, 0);
    u8g2.drawArc(rPosX, posY, EYE_SIZE_X + i, 130, 0);
  }

  // Balão de pensamento (bolhas crescentes) com reticências animadas
  int bx = rPosX + 14, by = posY - 20;
  u8g2.drawCircle(bx, by + 6, 2);
  u8g2.drawCircle(bx + 4, by, 3);
  u8g2.drawCircle(bx + 11, by - 9, 7);

  u8g2.setFont(u8g2_font_5x7_tr);
  int dots = (millis() / 400) % 4;
  char dotsBuf[4] = "";
  for (int i = 0; i < dots; i++) dotsBuf[i] = '.';
  dotsBuf[dots] = '\0';
  u8g2.drawStr(bx + 7, by - 7, dotsBuf);

  u8g2.sendBuffer();
}

// ══════════════════════════════════════════════════════════════════════
//  Wi-Fi: liga / desliga sob demanda
// ══════════════════════════════════════════════════════════════════════
bool wifiConnect(int timeoutMs = 12000) {
  if (savedSSID.length() == 0) return false;
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.length() ? savedPass.c_str() : nullptr);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < (unsigned long)timeoutMs) {
    drawThinking();
    delay(200);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  return wifiConnected;
}

void wifiOff() {
  if (webWifiConnected) return;  // mantém a conexão viva para o painel web
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
}

// ══════════════════════════════════════════════════════════════════════
//  Telas especiais
// ══════════════════════════════════════════════════════════════════════
const char* wdayNames[] = { "Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sab" };

// Avanca o cache de hora usando millis() — sem reconectar WiFi
// syncedEpochSec guarda os segundos absolutos no momento do sync
static unsigned long syncedEpochSec = 0;  // segundos desde meia-noite no momento do sync
static unsigned long syncedMillis   = 0;  // millis() no momento do sync

void tickClock() {
  if (!ntpSynced) return;
  unsigned long nowSec = syncedEpochSec + (millis() - syncedMillis) / 1000UL;
  cachedHour = (nowSec / 3600) % 24;
  cachedMin  = (nowSec % 3600) / 60;
}

// WMO code → glifo Open Iconic Weather 2x (16x16px)
uint16_t wmoGlyph(int code) {
  if (code == 0)   return 64;   // sol
  if (code <= 2)   return 65;   // sol + nuvem
  if (code <= 3)   return 69;   // nublado
  if (code <= 49)  return 69;   // neblina
  if (code <= 67)  return 66;   // chuva / garoa
  if (code <= 77)  return 68;   // neve
  if (code <= 82)  return 66;   // chuva forte
  if (code <= 99)  return 67;   // trovoada
  return 69;
}

const char* wmoDesc(int code) {
  if (code == 0)   return "Sol";
  if (code <= 2)   return "P.Nuvens";
  if (code <= 3)   return "Nublado";
  if (code <= 49)  return "Neblina";
  if (code <= 59)  return "Garoa";
  if (code <= 69)  return "Chuva";
  if (code <= 79)  return "Neve";
  if (code <= 84)  return "C.Forte";
  if (code <= 99)  return "Trovoada";
  return "?";
}

void drawClock() {
  tickClock();
  u8g2.clearBuffer();

  // Linha superior: dia da semana + data
  char dateBuf[16];
  snprintf(dateBuf, sizeof(dateBuf), "%s %02d/%02d",
           wdayNames[cachedWDay], cachedDay, cachedMonth);
  u8g2.setFont(u8g2_font_6x12_tf);
  int16_t wTop = u8g2.getStrWidth(dateBuf);
  int16_t xTop = (OLED_WIDTH - wTop) / 2;
  u8g2.drawStr(xTop, 14, dateBuf);

  // Hora grande
  char timeBuf[6];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", cachedHour, cachedMin);
  u8g2.setFont(u8g2_font_fub30_tf);
  int16_t wTime = u8g2.getStrWidth(timeBuf);
  int16_t xTime = (OLED_WIDTH - wTime) / 2;
  u8g2.drawStr(xTime, 54, timeBuf);

  u8g2.sendBuffer();
}

void drawWeather() {
  u8g2.clearBuffer();

  // ── Linha 1: cidade ───────────────────────────────────────────────
  u8g2.setFont(u8g2_font_6x12_tf);
  const char* city = LOC_CITY;
  int16_t wc = u8g2.getStrWidth(city);
  u8g2.drawStr((OLED_WIDTH - wc) / 2, 12, city);

  // ── Linha 2: temperatura grande (esquerda) ────────────────────────
  char tempBuf[8];
  snprintf(tempBuf, sizeof(tempBuf), "%.0f", cachedTemp);
  u8g2.setFont(u8g2_font_fub30_tf);
  int16_t wt = u8g2.getStrWidth(tempBuf);
  u8g2.drawStr(4, 52, tempBuf);

  // Simbolo de grau + C em fonte menor ao lado
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(wt + 6, 34, "o");   // grau
  u8g2.drawStr(wt + 6, 48, "C");

  // ── Icone de clima (Open Iconic Weather 2x = 16x16) ──────────────
  // Posiciona no lado direito
  int iconX = 88;
  int iconY = 36;   // baseline do glifo
  u8g2.setFont(u8g2_font_open_iconic_weather_2x_t);
  u8g2.drawGlyph(iconX, iconY, wmoGlyph(cachedWCode));

  // Descricao abaixo do icone
  u8g2.setFont(u8g2_font_5x7_tr);
  const char* desc = wmoDesc(cachedWCode);
  int16_t wd = u8g2.getStrWidth(desc);
  u8g2.drawStr(iconX + (16 - wd) / 2, 52, desc);

  u8g2.sendBuffer();
}

// ── Tela: como acessar o modo web (3º click do botão) ─────────────────
void drawWebInfo() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  if (webWifiConnected) {
    const char* l1 = "Painel Web em:";
    int16_t w1 = u8g2.getStrWidth(l1);
    u8g2.drawStr((OLED_WIDTH - w1) / 2, 26, l1);

    String ip = WiFi.localIP().toString();
    int16_t w2 = u8g2.getStrWidth(ip.c_str());
    u8g2.drawStr((OLED_WIDTH - w2) / 2, 42, ip.c_str());
  } else {
    const char* l1 = "Conecte ao Wi-Fi:";
    int16_t w1 = u8g2.getStrWidth(l1);
    u8g2.drawStr((OLED_WIDTH - w1) / 2, 16, l1);

    int16_t w2 = u8g2.getStrWidth(WIFI_AP_NAME);
    u8g2.drawStr((OLED_WIDTH - w2) / 2, 30, WIFI_AP_NAME);

    char l3[24];
    snprintf(l3, sizeof(l3), "Acesse: %s", AP_IP.toString().c_str());
    int16_t w3 = u8g2.getStrWidth(l3);
    u8g2.drawStr((OLED_WIDTH - w3) / 2, 46, l3);
  }

  u8g2.sendBuffer();
}

void showLoadingScreen(const char* line1, const char* line2 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  int16_t w1 = u8g2.getStrWidth(line1);
  u8g2.drawStr((OLED_WIDTH - w1) / 2, 28, line1);
  if (strlen(line2) > 0) {
    int16_t w2 = u8g2.getStrWidth(line2);
    u8g2.drawStr((OLED_WIDTH - w2) / 2, 42, line2);
  }
  u8g2.sendBuffer();
}

// ══════════════════════════════════════════════════════════════════════
//  Máquina de estados
// ══════════════════════════════════════════════════════════════════════
void enterState(EyeType eye) {
  currentEye  = eye;
  stateTimer  = millis();
  if (eye == EYE_NEUTRO) {
    stateDuration = neutralDuration();
    blinkTimer    = millis();
    nextBlinkIn   = nextBlinkDelay();
    blinking      = false;
  } else if (eye == EYE_BLINK) {
    stateDuration = BLINK_DURATION;
  } else {
    stateDuration = expressionDuration();
  }
}

EyeType randomExpression() {
  // Pool completo de expressões automáticas (exceto BLINK que é gerenciado separado)
  EyeType options[] = {
    EYE_FELIZ, EYE_FELIZ,       // peso maior para feliz
    EYE_TRISTE,
    EYE_MALVADO,
    EYE_CANSADO,
    EYE_FURIA,
    EYE_RASCAL,
  };
  int count = sizeof(options) / sizeof(options[0]);
  EyeType picked;
  do { picked = options[random(0, count)]; } while (picked == previousEye);
  return picked;
}

void updateMovement() {
  unsigned long now = millis();
  if (now - lastMoveChange > EYE_MOVE_INTERVAL) {
    targetX        = random(-EYE_RANGE_X, EYE_RANGE_X);
    targetY        = random(-EYE_RANGE_Y_UP, EYE_RANGE_Y_DOWN);
    lastMoveChange = now;
  }
  currentX += (targetX - currentX) * EYE_MOVE_SPEED;
  currentY += (targetY - currentY) * EYE_MOVE_SPEED;
}

void updateServo() {
  float targetAngle;

  if (specialMode != MODE_NONE) {
    // Telas especiais: centraliza o servo suavemente
    targetAngle = servoCenterCfg;
  } else if (currentEye == EYE_NEUTRO) {
    // Modo neutro: segue currentX dos olhos
    // currentX varia entre -EYE_RANGE_X e +EYE_RANGE_X
    // mapeia para servoCenterCfg ± servoRangeCfg
    float ratio = currentX / (float)EYE_RANGE_X;  // -1.0 a +1.0
    targetAngle = servoCenterCfg + ratio * servoRangeCfg;
  } else {
    // Em expressões: centraliza
    targetAngle = servoCenterCfg;
  }

  // Suaviza o movimento
  servoCurrentAngle += (targetAngle - servoCurrentAngle) * SERVO_SMOOTH;

  // Limita range e escreve
  int angle = constrain((int)round(servoCurrentAngle),
                        servoCenterCfg - servoRangeCfg,
                        servoCenterCfg + servoRangeCfg);
  servoWrite(angle);
}

void updateStateMachine() {
  unsigned long now = millis();
  if (forcedEye != EYE_COUNT) {
    enterState(forcedEye);
    forcedEye = EYE_COUNT;
    return;
  }
  if (sleepMode) {
    // Mantém o olho dormindo fixo por todo o período do sono — sem isso,
    // EYE_DORMIDO expirava como uma expressão comum (poucos segundos) e
    // voltava sozinho ao neutro bem antes de SLEEP_HOUR_END.
    if (currentEye != EYE_DORMIDO) enterState(EYE_DORMIDO);
    currentX = 0; currentY = 0;
    return;
  }
  if (currentEye == EYE_NEUTRO) {
    updateMovement();
    if (now - stateTimer > stateDuration) { previousEye = EYE_NEUTRO; enterState(randomExpression()); return; }
    if (!blinking && now - blinkTimer > nextBlinkIn) { previousEye = EYE_NEUTRO; enterState(EYE_BLINK); }
  } else if (currentEye == EYE_BLINK) {
    if (now - stateTimer > stateDuration) { enterState(EYE_NEUTRO); blinkTimer = millis(); nextBlinkIn = nextBlinkDelay(); }
  } else {
    currentX = 0; currentY = 0;
    if (now - stateTimer > stateDuration) { previousEye = currentEye; enterState(EYE_NEUTRO); }
  }
}

// ── Modo sono por horário ─────────────────────────────────────────────
void checkSleepMode() {
  if (!ntpSynced) return;
  tickClock();

  int h = cachedHour;
  bool shouldSleep = (h >= SLEEP_HOUR_START || h < SLEEP_HOUR_END);

  if (shouldSleep && !sleepMode) {
    // Entra no modo sono
    sleepMode = true;
    forcedEye = EYE_DORMIDO;
    Serial.printf("[Sleep] Entrando em modo sono (%02d:xx)\n", h);

  } else if (!shouldSleep && sleepMode) {
    // Acorda
    sleepMode = false;
    forcedEye = EYE_NEUTRO;
    Serial.printf("[Sleep] Acordando (%02d:xx)\n", h);
  }
}

// ══════════════════════════════════════════════════════════════════════
//  Botões
// ══════════════════════════════════════════════════════════════════════
// ── Sincroniza NTP ────────────────────────────────────────────────────
void doSyncNTP() {
  drawThinking();
  if (!wifiConnect()) { showLoadingScreen("Sem Wi-Fi", "verifique a rede"); delay(2000); return; }

  configTzTime("BRT3", NTP_SERVER, "time.google.com", "time.cloudflare.com");
  unsigned long t0 = millis();
  struct tm ti;
  while (millis() - t0 < 10000) {
    if (getLocalTime(&ti) && ti.tm_year > 100) break;
    drawThinking();
    delay(300);
  }
  if (getLocalTime(&ti) && ti.tm_year > 100) {
    unsigned long capturedMillis = millis();
    cachedHour  = ti.tm_hour;
    cachedMin   = ti.tm_min;
    cachedDay   = ti.tm_mday;
    cachedMonth = ti.tm_mon + 1;
    cachedWDay  = ti.tm_wday;
    ntpSynced   = true;
    lastNtpSync = capturedMillis;
    syncedEpochSec = (unsigned long)ti.tm_hour * 3600
                   + (unsigned long)ti.tm_min  * 60
                   + (unsigned long)ti.tm_sec;
    syncedMillis   = capturedMillis;
    Serial.printf("[NTP] %02d:%02d:%02d\n", ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    Serial.println("[NTP] Falha na sincronizacao");
  }
  wifiOff();
}

// ── Sincroniza clima ───────────────────────────────────────────────────
void doSyncWeather() {
  drawThinking();
  if (!wifiConnect()) { showLoadingScreen("Sem Wi-Fi", "verifique a rede"); delay(2000); return; }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast"
               "?latitude=" LOC_LAT
               "&longitude=" LOC_LON
               "&current_weather=true"
               "&temperature_unit=celsius";
  http.begin(client, url);
  http.setTimeout(10000);
  drawThinking();
  int code = http.GET();
  Serial.printf("[Weather] HTTP %d\n", code);
  if (code == HTTP_CODE_OK) {
    String body = http.getString();
    Serial.println("[Weather] " + body.substring(0, 150));
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      cachedTemp  = doc["current_weather"]["temperature"] | 0.0f;
      cachedWCode = doc["current_weather"]["weathercode"] | 0;
      weatherReady     = true;
      lastWeatherFetch = millis();
      Serial.printf("[Weather] Temp=%.1f Code=%d\n", cachedTemp, cachedWCode);
    } else {
      Serial.println("[Weather] JSON erro: " + String(err.c_str()));
    }
  }
  http.end();
  wifiOff();
}

// ── Tela de configuração do foco ─────────────────────────────────────
void drawFocusSetup() {
  u8g2.clearBuffer();

  // "FOCO" grande no topo
  u8g2.setFont(u8g2_font_fub20_tf);
  const char* title = "FOCO";
  int16_t wTitle = u8g2.getStrWidth(title);
  u8g2.drawStr((OLED_WIDTH - wTitle) / 2, 26, title);

  // Tempo selecionado grande abaixo
  char buf[12];
  snprintf(buf, sizeof(buf), "%d min", focusMinutes);
  u8g2.setFont(u8g2_font_fub30_tf);
  int16_t w = u8g2.getStrWidth(buf);
  u8g2.drawStr((OLED_WIDTH - w) / 2, 60, buf);

  u8g2.sendBuffer();
}

// ── Tela de foco em andamento ─────────────────────────────────────────
void drawFocusRun() {
  unsigned long remaining = 0;
  if (millis() < focusEndMs) remaining = (focusEndMs - millis()) / 1000UL;

  int mins = remaining / 60;
  int secs = remaining % 60;

  u8g2.clearBuffer();

  // Olhos dormindo (linhas)
  int lx = EYE_LEFT, rx = EYE_RIGHT, py = 28;
  for (int i = 0; i < 4; i++) {
    u8g2.drawHLine(lx - 13, py + i, 26);
    u8g2.drawHLine(rx - 13, py + i, 26);
  }

  // Ícone de lâmpada piscando (centro, entre os olhos)
  // Pisca a cada 800ms
  if ((millis() / 800) % 2 == 0) {
    int bx = 58, by = 18;
    // Corpo da lâmpada
    u8g2.drawCircle(bx + 5, by, 6);
    u8g2.drawVLine(bx + 2, by + 5, 4);
    u8g2.drawVLine(bx + 5, by + 5, 4);
    u8g2.drawVLine(bx + 8, by + 5, 4);
    u8g2.drawHLine(bx + 2, by + 9, 7);
    // Raios
    u8g2.drawPixel(bx + 5, by - 8);
    u8g2.drawPixel(bx - 4, by - 4);
    u8g2.drawPixel(bx + 14, by - 4);
  }

  // Timer regressivo abaixo dos olhos
  char timeBuf[8];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", mins, secs);
  u8g2.setFont(u8g2_font_6x12_tf);
  int16_t w = u8g2.getStrWidth(timeBuf);
  u8g2.drawStr((OLED_WIDTH - w) / 2, 62, timeBuf);

  u8g2.sendBuffer();
}

// ── Aviso sonoro ao finalizar foco ───────────────────────────────────
void focusFinishBeep() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 1400, 200);
    delay(300);
    tone(BUZZER_PIN, 1000, 200);
    delay(300);
  }
}

void activateClock() {
  specialMode    = MODE_CLOCK;
  specialModeEnd = millis() + SPECIAL_MODE_DURATION;
  beep(BUZZER_FREQ_HIGH);
  // Sincroniza NTP sempre que cache expirou
  if (!ntpSynced || millis() - lastNtpSync > NTP_RESYNC_INTERVAL) doSyncNTP();
  // Aproveita a conexao para buscar clima tambem (so se necessario)
  if (!weatherReady || millis() - lastWeatherFetch > WEATHER_RESYNC_INTERVAL) doSyncWeather();
}

void activateWeather() {
  specialMode    = MODE_WEATHER;
  specialModeEnd = millis() + SPECIAL_MODE_DURATION;
  beep(BUZZER_FREQ_HIGH);
  if (!weatherReady || millis() - lastWeatherFetch > WEATHER_RESYNC_INTERVAL) doSyncWeather();
}

void activateFocusSetup() {
  specialMode  = MODE_FOCUS_SETUP;
  focusMinutes = FOCUS_MIN_TIME;  // reseta para o mínimo
  beep(BUZZER_FREQ_HIGH);
}

void activateWebInfo() {
  specialMode    = MODE_WEB_INFO;
  specialModeEnd = millis() + SPECIAL_MODE_DURATION;
  beep(BUZZER_FREQ_HIGH);
}

void startFocusRun() {
  specialMode = MODE_FOCUS_RUN;
  focusEndMs  = millis() + (unsigned long)focusMinutes * 60000UL;
  beep(BUZZER_FREQ_HIGH, 200);
}

void handleButtons() {
  bool btnDown = (digitalRead(BUTTON_PIN) == LOW);  // botão pressionado agora

  // ═══════════════════════════════════════════════════════════════════
  //  MODO SONO: só touch mostra hora brevemente
  // ═══════════════════════════════════════════════════════════════════
  if (sleepMode) {
    if (touch.getSingleDebouncedPress()) {
      beep(BUZZER_FREQ_LOW);
      sleepMode = false;
      activateClock();
      sleepMode = true;
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO FOCO EM ANDAMENTO: só botão cancela
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_FOCUS_RUN) {
    if (button.getSingleDebouncedPress()) {
      // Cancela foco
      specialMode = MODE_NONE;
      forcedEye   = EYE_NEUTRO;
      beep(BUZZER_FREQ_LOW, 300);
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO SETUP FOCO: touch +5min / botão longo inicia
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_FOCUS_SETUP) {
    // Touch: adiciona FOCUS_STEP minutos (até FOCUS_MAX_TIME, depois volta ao mínimo)
    if (touch.getSingleDebouncedPress()) {
      focusMinutes += FOCUS_STEP;
      if (focusMinutes > FOCUS_MAX_TIME) focusMinutes = FOCUS_MIN_TIME;
      beep(BUZZER_FREQ_HIGH, 60);
    }

    // Detecta clique longo no botão (>= FOCUS_HOLD_MS)
    if (btnDown && !btnHeld) {
      btnPressStart = millis();
      btnHeld = true;
    }
    if (!btnDown && btnHeld) {
      unsigned long held = millis() - btnPressStart;
      btnHeld = false;
      if (held >= FOCUS_HOLD_MS) {
        startFocusRun();  // clique longo → inicia foco
      } else {
        // clique curto no setup (3º click) → mostra como acessar o modo web
        activateWebInfo();
      }
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO INFO WEB: qualquer clique volta ao neutro
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_WEB_INFO) {
    if (button.getSingleDebouncedPress() || touch.getSingleDebouncedPress()) {
      specialMode = MODE_NONE;
      forcedEye   = EYE_NEUTRO;
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO CLOCK: touch troca para clima
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_CLOCK) {
    if (touch.getSingleDebouncedPress()) {
      activateWeather();
    }
    // Botão físico avança para próximo modo (foco setup)
    if (button.getSingleDebouncedPress()) {
      activateFocusSetup();
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO CLIMA: touch volta para hora
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_WEATHER) {
    if (touch.getSingleDebouncedPress()) {
      activateClock();
    }
    if (button.getSingleDebouncedPress()) {
      activateFocusSetup();
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO NORMAL: botão → hora | touch → expressão aleatória
  // ═══════════════════════════════════════════════════════════════════
  if (button.getSingleDebouncedPress()) {
    specialMode = MODE_NONE;
    activateClock();
  }

  if (touch.getSingleDebouncedPress()) {
    specialMode = MODE_NONE;
    beep(BUZZER_FREQ_HIGH);
    forcedEye = RANDOM_POOL[random(RANDOM_POOL_SIZE)];
  }
}

// ══════════════════════════════════════════════════════════════════════
//  Renderização dos olhos
// ══════════════════════════════════════════════════════════════════════
void normalEye(int posX, int posY, int tamX, int tamY) { u8g2.drawEllipse(posX, posY, tamX, tamY); }
void halfEye(int posX, int posY, int tam, int start, int finish) { u8g2.drawArc(posX, posY, tam, start, finish); }
void lineEye(int posX, int posY, int tam) { u8g2.drawHLine(posX, posY, tam); }

void renderEyes(int tamX, int tamY, int esp) {
  int lPosX = round(currentX) + EYE_LEFT;
  int rPosX = round(currentX) + EYE_RIGHT;
  int posY  = round(currentY) + EYE_CENTER_Y;

  switch (currentEye) {
    case EYE_NEUTRO:
      for (int i = 0; i < esp; i++) { normalEye(lPosX, posY, tamX+i, tamY+i); normalEye(rPosX, posY, tamX+i, tamY+i); }
      break;
    case EYE_BLINK:
      for (int i = 0; i < esp; i++) { lineEye(lPosX-15, posY+i, tamX+20); lineEye(rPosX-15, posY+i, tamX+20); }
      break;
    case EYE_DORMIDO:
      for (int i = 0; i < esp; i++) { lineEye(lPosX-15, posY+i, tamX+20); lineEye(rPosX-15, posY+i, tamX+20); }
      u8g2.setFont(u8g2_font_6x10_tr);
      { int z = (millis()/400)%3;
        if(z>=0) u8g2.drawStr(rPosX-20, posY-10, "z");
        if(z>=1) u8g2.drawStr(rPosX-12, posY-15, "z"); }
      break;
    case EYE_FELIZ:
      for (int i = 0; i < esp; i++) { halfEye(lPosX, posY, tamX+i, 0, 130); halfEye(rPosX, posY, tamX+i, 0, 130); }
      break;
    case EYE_TRISTE:
      for (int i = 0; i < esp; i++) { halfEye(rPosX, posY-6, tamX+i, -160, -35); halfEye(lPosX, posY-6, tamX+i, 160, 35); }
      { int dy = posY+15+((millis()/100)%15);
        u8g2.drawDisc(rPosX+10, dy, 2); u8g2.drawLine(rPosX+10, dy-3, rPosX+10, dy);
        u8g2.drawDisc(lPosX-10, dy, 2); u8g2.drawLine(lPosX-10, dy-3, lPosX-10, dy); }
      break;
    case EYE_MALVADO:
      for (int i = 0; i < esp; i++) { halfEye(lPosX, posY, tamX+i, -160, -35); halfEye(rPosX, posY, tamX+i, 160, 35); }
      break;
    case EYE_CANSADO:
      for (int i = 0; i < esp; i++) { halfEye(rPosX, posY-6, tamX+i, -160, -35); halfEye(lPosX, posY-6, tamX+i, 160, 35); }
      break;
    case EYE_FURIA:
      for (int i = 0; i < esp; i++) { halfEye(lPosX, posY, tamX+i, -160, -35); halfEye(rPosX, posY, tamX+i, 160, 35); }
      if ((millis()/400)%2) { int ax=rPosX+18,ay=posY-22,s=5; u8g2.drawLine(ax,ay,ax+s,ay+s); u8g2.drawLine(ax+s,ay,ax,ay+s); }
      break;
    case EYE_RASCAL:
      for (int i = 0; i < esp; i++) { halfEye(lPosX, posY, tamX+i, 130, 0); halfEye(rPosX, posY, tamX+i, 130, 0); }
      break;
    default: break;
  }
}

// ══════════════════════════════════════════════════════════════════════
//  Servidor Web — rotas
// ══════════════════════════════════════════════════════════════════════
// ── Rota: scan de redes Wi-Fi → retorna JSON ─────────────────────────
void handleScan() {
  int n = WiFi.scanNetworks(false, false);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "'");
    int enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
    json += "{\"ssid\":\"" + ssid + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"enc\":" + String(enc) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

static const char* PAGE_STYLE = R"css(
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:24px;margin:0}
  h1{color:#4af;margin-bottom:4px}
  p{color:#aaa;margin:4px 0 16px}
  input{width:100%;max-width:320px;padding:10px 14px;border-radius:8px;
        border:2px solid #4af;background:#1a1a1a;color:#eee;font-size:1rem;margin:6px 0}
  input[type=range]{padding:0;border:none;background:transparent;height:24px}
  .servo-vals{font-size:.8rem;color:#aaa;margin:2px 0 8px}
  button{background:#222;color:#eee;border:2px solid #4af;border-radius:8px;
         padding:12px 20px;font-size:1rem;cursor:pointer;min-width:110px;margin:4px}
  button:hover{background:#4af;color:#111}
  .grid{display:flex;flex-wrap:wrap;justify-content:center;gap:10px;margin:20px 0}
  #status{margin-top:16px;font-size:.9rem;color:#aaa}
  .card{background:#1a1a1a;border:1px solid #333;border-radius:12px;padding:24px;max-width:360px;margin:0 auto}
  label{display:block;text-align:left;font-size:.85rem;color:#aaa;margin-top:10px}
  .net-list{list-style:none;padding:0;margin:10px 0;max-height:200px;overflow-y:auto}
  .net-item{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;margin:4px 0;
            background:#222;border:1px solid #333;border-radius:8px;cursor:pointer;transition:background .2s}
  .net-item:hover,.net-item.selected{background:#1a3a5c;border-color:#4af}
  .net-ssid{font-size:.95rem;font-weight:bold}
  .net-info{font-size:.75rem;color:#888}
  .scan-btn{font-size:.8rem;padding:6px 14px;margin-bottom:10px}
  .separator{border:none;border-top:1px solid #333;margin:14px 0}
</style>
)css";

void handleRoot() {
  if (webWifiConnected) { server.sendHeader("Location", "/panel"); server.send(302); return; }
  String html = "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>DrawEye - Wi-Fi</title>";
  html += PAGE_STYLE;
  html += R"rawhtml(
</head><body>
<h1>&#128065; DrawEye</h1>
<p>Selecione sua rede Wi-Fi</p>
<div class="card">
  <button class="scan-btn" onclick="scan()">&#128269; Buscar redes</button>
  <ul class="net-list" id="netList"><li style="color:#555">Clique em Buscar redes</li></ul>
  <hr class="separator">
  <label>Rede selecionada</label>
  <input id="ssid" type="text" placeholder="Ou digite o SSID manualmente" autocomplete="off">
  <label>Senha</label>
  <input id="pass" type="password" placeholder="••••••••">
  <br><br>
  <button onclick="conectar()">Conectar</button>
</div>
<p id="status"></p>
<script>
function rssiBar(r){return r>-55?'▂▄▆█':r>-70?'▂▄▆_':r>-80?'▂▄__':'▂___';}
async function scan(){
  document.getElementById('netList').innerHTML='<li style="color:#aaa">Buscando...</li>';
  try{
    const r=await fetch('/scan'); const nets=await r.json();
    if(!nets.length){document.getElementById('netList').innerHTML='<li style="color:#555">Nenhuma rede</li>';return;}
    nets.sort((a,b)=>b.rssi-a.rssi);
    document.getElementById('netList').innerHTML=nets.map(n=>
      `<li class="net-item" onclick="selectNet('${n.ssid.replace(/'/g,"\\'")}')">
        <span class="net-ssid">${n.ssid}${n.enc?' &#128274;':''}</span>
        <span class="net-info">${rssiBar(n.rssi)} ${n.rssi}dBm</span></li>`
    ).join('');
  }catch(e){document.getElementById('netList').innerHTML='<li style="color:#f66">Erro no scan</li>';}
}
function selectNet(s){
  document.getElementById('ssid').value=s;
  document.querySelectorAll('.net-item').forEach(el=>{
    el.classList.toggle('selected',el.querySelector('.net-ssid').textContent.trim().startsWith(s));
  });
  document.getElementById('pass').focus();
}
async function conectar(){
  const ssid=document.getElementById('ssid').value.trim();
  const pass=document.getElementById('pass').value;
  if(!ssid){document.getElementById('status').textContent='Selecione ou digite o SSID.';return;}
  document.getElementById('status').textContent='Conectando...';
  try{
    const r=await fetch('/connect?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass));
    const t=await r.text();
    document.getElementById('status').textContent=t;
    if(t.startsWith('OK')) setTimeout(()=>{window.location.href='/panel';},2000);
  }catch(e){document.getElementById('status').textContent='Erro: '+e;}
}
window.onload=scan;
</script></body></html>
)rawhtml";
  server.send(200, "text/html", html);
}

void handleConnect() {
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "SSID ausente"); return; }
  savedSSID = server.arg("ssid");
  savedPass = server.hasArg("pass") ? server.arg("pass") : "";

  showLoadingScreen("Conectando...", savedSSID.c_str());

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.length() ? savedPass.c_str() : nullptr);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);

  if (WiFi.status() == WL_CONNECTED) {
    webWifiConnected = true;
    wifiConnected    = true;
    saveCredentials(savedSSID, savedPass);  // persiste para próximo boot
    String ip = WiFi.localIP().toString();
    showLoadingScreen("Wi-Fi OK!", ip.c_str());
    server.send(200, "text/plain", "OK - IP: " + ip);
  } else {
    WiFi.disconnect();
    savedSSID = "";
    showLoadingScreen("Falha Wi-Fi", "Tente novamente");
    server.send(200, "text/plain", "Falha - verifique SSID e senha");
  }
}

void handlePanel() {
  if (!webWifiConnected) { server.sendHeader("Location", "/"); server.send(302); return; }
  String html = "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>DrawEye - Controle</title>";
  html += PAGE_STYLE;
  html += R"rawhtml(
</head><body>
<h1>&#128065; DrawEye</h1>
<p>Controle remoto das expressoes</p>
<div class="grid">
  <button onclick="setEye('neutro')">Neutro</button>
  <button onclick="setEye('feliz')">Feliz</button>
  <button onclick="setEye('triste')">Triste</button>
  <button onclick="setEye('malvado')">Malvado</button>
  <button onclick="setEye('dormido')">Dormido</button>
  <button onclick="setEye('cansado')">Cansado</button>
  <button onclick="setEye('furia')">Furia</button>
  <button onclick="setEye('rascal')">Rascal</button>
  <button onclick="setEye('random')">Aleatorio</button>
</div>
<p id="status">Pronto.</p>

<div class="card" style="margin-top:20px">
  <p style="margin-top:0">&#9881; Ajuste do servo (cabeca)</p>
  <label>Centro: <span id="centerVal">90</span>&deg;</label>
  <input type="range" id="servoCenter" min="0" max="180" value="90" oninput="atualizarLabelsServo()">
  <label>Amplitude para cada lado: <span id="rangeVal">30</span>&deg;</label>
  <input type="range" id="servoRange" min="0" max="90" value="30" oninput="atualizarLabelsServo()">
  <p class="servo-vals">Vai de <b id="minAngle">60</b>&deg; a <b id="maxAngle">120</b>&deg;</p>
  <button onclick="testarServo()" style="font-size:.8rem;padding:6px 12px">Testar</button>
  <button onclick="salvarServo()" style="font-size:.8rem;padding:6px 12px;border-color:#4a4;color:#4a4">Salvar</button>
</div>

<button onclick="window.location.href='/'" style="font-size:.8rem;padding:6px 12px;margin-top:20px">Trocar Wi-Fi</button>
<button onclick="reiniciar()" style="font-size:.8rem;padding:6px 12px;margin-top:6px;border-color:#fa4;color:#fa4">Reiniciar DrawEye</button>
<button onclick="esquecer()" style="font-size:.8rem;padding:6px 12px;margin-top:6px;border-color:#f66;color:#f66">Esquecer rede salva</button>
<script>
function atualizarLabelsServo(){
  const c = parseInt(document.getElementById('servoCenter').value);
  const r = parseInt(document.getElementById('servoRange').value);
  document.getElementById('centerVal').textContent = c;
  document.getElementById('rangeVal').textContent = r;
  document.getElementById('minAngle').textContent = Math.max(0, c - r);
  document.getElementById('maxAngle').textContent = Math.min(180, c + r);
}
async function carregarServo(){
  try{
    const r = await fetch('/servo'); const j = await r.json();
    document.getElementById('servoCenter').value = j.center;
    document.getElementById('servoRange').value  = j.range;
    atualizarLabelsServo();
  }catch(e){}
}
async function testarServo(){
  const c = document.getElementById('servoCenter').value;
  const r = document.getElementById('servoRange').value;
  try{ await fetch('/servo?center='+c+'&range='+r); document.getElementById('status').textContent='Testando servo...'; }
  catch(e){ document.getElementById('status').textContent='Erro: '+e; }
}
async function salvarServo(){
  const c = document.getElementById('servoCenter').value;
  const r = document.getElementById('servoRange').value;
  try{
    await fetch('/servo?center='+c+'&range='+r+'&save=1');
    document.getElementById('status').textContent='Servo salvo: centro '+c+'°, amplitude ±'+r+'°';
  }catch(e){ document.getElementById('status').textContent='Erro: '+e; }
}
window.addEventListener('load', carregarServo);
</script>
<script>
async function reiniciar(){
  if(!confirm('Reiniciar o DrawEye agora?')) return;
  document.getElementById('status').textContent='Reiniciando... aguarde alguns segundos.';
  try{ await fetch('/restart'); }catch(e){}
}
async function esquecer(){
  if(!confirm('Apagar rede salva? O ESP abrira o portal no proximo boot.')) return;
  await fetch('/forget');
  alert('Rede apagada.');
}
</script>
<script>
async function setEye(name){
  document.getElementById('status').textContent='Enviando: '+name+'...';
  try{const r=await fetch('/eye?name='+name);const t=await r.text();document.getElementById('status').textContent=t;}
  catch(e){document.getElementById('status').textContent='Erro: '+e;}
}
</script></body></html>
)rawhtml";
  server.send(200, "text/html", html);
}

void handleSetEye() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Parametro 'name' ausente"); return; }
  String name = server.arg("name");
  name.toLowerCase();
  if (name == "random") {
    forcedEye = randomExpression(); beep(BUZZER_FREQ_HIGH);
    server.send(200, "text/plain", "OK: expressao aleatoria"); return;
  }
  for (int i = 0; i < EYE_COUNT; i++) {
    if (name == eyeNames[i]) {
      forcedEye = (EyeType)i; beep(BUZZER_FREQ_HIGH);
      server.send(200, "text/plain", "OK: " + name); return;
    }
  }
  server.send(404, "text/plain", "Expressao desconhecida: " + name);
}

void handleStatus() {
  String json = "{";
  json += "\"eye\":\"" + String(eyeNames[currentEye]) + "\",";
  json += "\"wifi\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"uptime\":" + String(millis()/1000);
  json += "}";
  server.send(200, "application/json", json);
}

void handleRestart() {
  server.send(200, "text/plain", "Reiniciando...");
  server.handleClient();  // garante que a resposta HTTP saiu antes do reset
  showLoadingScreen("Reiniciando...", "aguarde");
  delay(500);
  ESP.restart();
}

// ── Ajuste do servo: centro e amplitude (graus para cada lado) ────────
void handleServo() {
  // Sem parâmetros → apenas consulta os valores atuais
  if (!server.hasArg("center") && !server.hasArg("range")) {
    String json = "{\"center\":" + String(servoCenterCfg) +
                  ",\"range\":"  + String(servoRangeCfg) + "}";
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("center")) servoCenterCfg = constrain(server.arg("center").toInt(), 0, 180);
  if (server.hasArg("range"))  servoRangeCfg  = constrain(server.arg("range").toInt(), 0, 90);

  // Garante que centro ± amplitude não estoure os limites físicos (0-180°)
  if (servoCenterCfg - servoRangeCfg < 0)   servoRangeCfg = servoCenterCfg;
  if (servoCenterCfg + servoRangeCfg > 180) servoRangeCfg = 180 - servoCenterCfg;

  // Move na hora para o usuário ver o resultado (teste ou salvo, tanto faz)
  servoCurrentAngle = servoCenterCfg;
  servoWrite(servoCenterCfg);

  if (server.hasArg("save")) saveServoConfig();

  String json = "{\"center\":" + String(servoCenterCfg) +
                ",\"range\":"  + String(servoRangeCfg) + "}";
  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/",        handleRoot);
  server.on("/scan",    handleScan);
  server.on("/connect", handleConnect);
  server.on("/forget",  [](){
    clearCredentials();
    server.sendHeader("Location", "/");
    server.send(302);
  });
  server.on("/panel",   handlePanel);
  server.on("/eye",     handleSetEye);
  server.on("/status",  handleStatus);
  server.on("/restart", handleRestart);
  server.on("/servo",   handleServo);
  server.begin();
  Serial.println("[Web] Servidor iniciado");
}

// ══════════════════════════════════════════════════════════════════════
//  Wi-Fi setup inicial (AP + portal de configuração)
// ══════════════════════════════════════════════════════════════════════
void setupWiFi() {
  // Evita estado de rádio "sujo" de um boot anterior (comum de causar
  // falha silenciosa do AP em transições rápidas STA → AP no ESP32-C6)
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(200);

  // ── Tenta reconectar com credenciais salvas ─────────────────────────
  if (loadCredentials()) {
    showLoadingScreen("Conectando...", savedSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.length() ? savedPass.c_str() : nullptr);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
      delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected    = true;
      webWifiConnected = true;
      String ip = WiFi.localIP().toString();
      Serial.print("[WiFi] Auto-conectado: "); Serial.println(ip);

      // Mostra IP por 3s e segue
      showLoadingScreen("Wi-Fi OK!", ip.c_str());

      // Servidor web em modo STA
      setupWebServer();
      delay(2000);
      return;  // sem portal, vai direto para o loop
    }

    // Falhou — limpa e abre portal
    Serial.println("[WiFi] Auto-connect falhou, abrindo portal...");
    WiFi.disconnect(true);
    clearCredentials();
    WiFi.mode(WIFI_OFF);
    delay(200);
  }

  // ── Nenhuma credencial ou falha: abre AP + portal ───────────────────
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);  // modem-sleep pode ocultar/derrubar o AP

  bool apOk = WiFi.softAP(WIFI_AP_NAME, strlen(WIFI_AP_PASSWORD) ? WIFI_AP_PASSWORD : nullptr);
  delay(300);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));

  IPAddress apIPAddr = WiFi.softAPIP();
  String apIP = apIPAddr.toString();
  Serial.printf("[WiFi] softAP %s | SSID=%s | IP=%s\n",
                apOk ? "OK" : "FALHOU", WIFI_AP_NAME, apIP.c_str());

  dnsServer.start(53, "*", apIPAddr);

  setupWebServer();

  // Mantém a tela de instruções enquanto o dispositivo não for configurado
  // via portal (antes só ficava 60s, depois voltava aos olhos "escondendo"
  // o SSID/IP mesmo com o AP ainda ativo).
  while (!webWifiConnected) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 12, "Conecte ao Wi-Fi:");
    u8g2.drawStr(0, 26, WIFI_AP_NAME);
    u8g2.drawStr(0, 42, "Acesse no browser:");
    u8g2.drawStr(0, 56, apIP.c_str());
    u8g2.sendBuffer();
    dnsServer.processNextRequest();
    server.handleClient();
    delay(50);
  }
}

// ══════════════════════════════════════════════════════════════════════
//  Setup & Loop
// ══════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  u8g2.begin();
  randomSeed(analogRead(0));

  // Servo
  loadServoConfig();
  ESP32PWM::allocateTimer(0);
  headServo.setPeriodHertz(50);
  headServo.attach(SERVO_PIN, 500, 2400);
  servoCurrentAngle = servoCenterCfg;
  headServo.write(servoCenterCfg);
  delay(500);
  setupWiFi();
  enterState(EYE_NEUTRO);
  beep(BUZZER_FREQ_HIGH, 150);
  Serial.println("[DrawEye] Pronto!");
}

void loop() {
  handleButtons();

  // Servidor web — mantém AP ativo para configuração
  dnsServer.processNextRequest();
  server.handleClient();

  // Tela especial ativa?
  if (specialMode != MODE_NONE) {

    if (specialMode == MODE_FOCUS_SETUP) {
      drawFocusSetup();
      delay(LOOP_DELAY_MS);
      return;
    }

    if (specialMode == MODE_FOCUS_RUN) {
      if (millis() < focusEndMs) {
        drawFocusRun();
      } else {
        // Foco terminou!
        focusFinishBeep();
        specialMode = MODE_NONE;
        enterState(EYE_NEUTRO);
      }
      delay(LOOP_DELAY_MS);
      return;
    }

    // Clock / Weather / Web Info: exibe por SPECIAL_MODE_DURATION
    if (millis() < specialModeEnd) {
      if (specialMode == MODE_CLOCK)    drawClock();
      if (specialMode == MODE_WEATHER)  drawWeather();
      if (specialMode == MODE_WEB_INFO) drawWebInfo();
    } else {
      specialMode = MODE_NONE;
      // Se o touch mostrou a hora durante o sono, volta a dormir (não ao neutro)
      enterState(sleepMode ? EYE_DORMIDO : EYE_NEUTRO);
    }
    delay(LOOP_DELAY_MS);
    return;
  }

  // Verifica modo sono por horário (a cada loop)
  checkSleepMode();

  // Servo
  updateServo();

  // Animação normal
  updateStateMachine();
  u8g2.clearBuffer();
  renderEyes(EYE_SIZE_X, EYE_SIZE_Y, EYE_THICKNESS);
  u8g2.sendBuffer();
  delay(LOOP_DELAY_MS);
}