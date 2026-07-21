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
  EYE_CURIOSO,   // olhos se movendo/olhando em volta (era o antigo "neutro")
  EYE_FELIZ,
  EYE_TRISTE,
  EYE_MALVADO,
  EYE_DORMIDO,
  EYE_CANSADO,
  EYE_FURIA,
  EYE_BLINK,
  EYE_RASCAL,
  EYE_SURPRESO,
  EYE_APAIXONADO,
  EYE_PISCADINHA,
  EYE_CONFUSO,
  EYE_COUNT
};

const char* eyeNames[] = {
  "neutro", "curioso", "feliz", "triste", "malvado",
  "dormido", "cansado", "furia", "blink", "rascal",
  "surpreso", "apaixonado", "piscadinha", "confuso"
};

// Modos especiais do botão físico
enum SpecialMode {
  MODE_NONE         = 0,
  MODE_CLOCK        = 1,   // 1º click botão → hora
  MODE_FOCUS_SETUP  = 2,   // 2º click botão → configura timer foco
  MODE_FOCUS_RUN    = 3,   // botão longo no modo setup → inicia foco
  MODE_STATUS       = 4,   // 3º click botão → status do dispositivo (rede/IP + uptime + heap)
  MODE_WEATHER      = 9,   // touch no modo clock → clima atual
  MODE_FORECAST     = 10,  // touch no modo clima → previsão dos próximos 3 dias
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
float  servoSmoothCfg    = SERVO_SMOOTH;  // ajustável via /servo (painel web)

// ── Tamanho dos olhos — ajustável via /eyesize (painel web) ────────────
int    eyeSizeXCfg      = (int)EYE_SIZE_X;
int    eyeSizeYCfg      = (int)EYE_SIZE_Y;
int    eyeThicknessCfg  = EYE_THICKNESS;

// ── Horário do modo sono — ajustável via /sleep (painel web) ───────────
int    sleepStartCfg    = SLEEP_HOUR_START;
int    sleepEndCfg      = SLEEP_HOUR_END;

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

// Guarda o progresso do neutro/curioso ao entrar numa piscada, para retomar
// exatamente de onde parou (sem reiniciar o cronômetro a cada piscada).
unsigned long savedStateTimer    = 0;
unsigned long savedStateDuration = 0;

// Marca o início da contagem do ciclo neutro/expressão — a cada
// CURIOUS_INTERVAL desde a última vez que saiu do modo curioso, entra
// nele de novo por CURIOUS_DURATION.
unsigned long curiousTimer = 0;

// ── Timing dos modos automáticos — ajustável via /timing (painel web) ──
unsigned long neutralHoldCfg     = NEUTRAL_HOLD_DURATION;
unsigned long exprHoldCfg        = EXPR_HOLD_DURATION;
unsigned long curiousIntervalCfg = CURIOUS_INTERVAL;
unsigned long curiousDurationCfg = CURIOUS_DURATION;

const EyeType RANDOM_POOL[] = {
  EYE_FELIZ, EYE_FURIA, EYE_MALVADO, EYE_BLINK, EYE_RASCAL,
  EYE_SURPRESO, EYE_APAIXONADO, EYE_PISCADINHA, EYE_CONFUSO
};
const byte RANDOM_POOL_SIZE = sizeof(RANDOM_POOL) / sizeof(RANDOM_POOL[0]);

SpecialMode   specialMode     = MODE_NONE;
unsigned long specialModeEnd  = 0;
bool          sleepMode       = false;  // modo sono ativo
bool          dayMode         = true;   // modo dia ativo (contrasta com sleepMode)

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

// ── Ajuste do servo (centro/amplitude/smooth) — persistido em NVS ──────
void loadServoConfig() {
  prefs.begin("servo", true);
  servoCenterCfg = prefs.getInt("center", SERVO_CENTER);
  servoRangeCfg  = prefs.getInt("range",  SERVO_RANGE);
  servoSmoothCfg = prefs.getFloat("smooth", SERVO_SMOOTH);
  prefs.end();
}

void saveServoConfig() {
  prefs.begin("servo", false);
  prefs.putInt("center", servoCenterCfg);
  prefs.putInt("range",  servoRangeCfg);
  prefs.putFloat("smooth", servoSmoothCfg);
  prefs.end();
}

// ── Tamanho dos olhos — persistido em NVS ──────────────────────────────
void loadEyeConfig() {
  prefs.begin("eyecfg", true);
  eyeSizeXCfg     = prefs.getInt("sizeX", (int)EYE_SIZE_X);
  eyeSizeYCfg     = prefs.getInt("sizeY", (int)EYE_SIZE_Y);
  eyeThicknessCfg = prefs.getInt("thick", EYE_THICKNESS);
  prefs.end();
}

void saveEyeConfig() {
  prefs.begin("eyecfg", false);
  prefs.putInt("sizeX", eyeSizeXCfg);
  prefs.putInt("sizeY", eyeSizeYCfg);
  prefs.putInt("thick", eyeThicknessCfg);
  prefs.end();
}

// ── Horário do modo sono — persistido em NVS ───────────────────────────
void loadSleepConfig() {
  prefs.begin("sleepcfg", true);
  sleepStartCfg = prefs.getInt("start", SLEEP_HOUR_START);
  sleepEndCfg   = prefs.getInt("end",   SLEEP_HOUR_END);
  prefs.end();
}

void saveSleepConfig() {
  prefs.begin("sleepcfg", false);
  prefs.putInt("start", sleepStartCfg);
  prefs.putInt("end",   sleepEndCfg);
  prefs.end();
}

// ── Timing dos modos automáticos (neutro/curioso/expressão) — NVS ──────
void loadTimingConfig() {
  prefs.begin("timing", true);
  neutralHoldCfg     = prefs.getULong("neutral", NEUTRAL_HOLD_DURATION);
  exprHoldCfg        = prefs.getULong("expr",    EXPR_HOLD_DURATION);
  curiousIntervalCfg = prefs.getULong("curInt",  CURIOUS_INTERVAL);
  curiousDurationCfg = prefs.getULong("curDur",  CURIOUS_DURATION);
  prefs.end();
}

void saveTimingConfig() {
  prefs.begin("timing", false);
  prefs.putULong("neutral", neutralHoldCfg);
  prefs.putULong("expr",    exprHoldCfg);
  prefs.putULong("curInt",  curiousIntervalCfg);
  prefs.putULong("curDur",  curiousDurationCfg);
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
float  todayMax      = 0;   // máxima do dia atual
float  todayMin      = 0;   // mínima do dia atual
bool   weatherReady  = false;
unsigned long lastWeatherFetch = 0;
#define WEATHER_RESYNC_INTERVAL 1800000UL  // 30min

// ── Previsão dos próximos 3 dias (buscada junto com o clima atual) ─────
float  forecastMax[3]   = {0, 0, 0};
float  forecastMin[3]   = {0, 0, 0};
int    forecastWCode[3] = {0, 0, 0};
bool   forecastReady    = false;

// Barueri, SP — valores padrão de fábrica; ajustáveis via /location (painel web)
#define LOC_LAT   "-23.5114"
#define LOC_LON   "-46.8756"
#define LOC_CITY  "Barueri"
#define NTP_SERVER "pool.ntp.org"

String locLat  = LOC_LAT;
String locLon  = LOC_LON;
String locCity = LOC_CITY;

// ── Localização (clima) — persistida em NVS ────────────────────────────
void loadLocationConfig() {
  prefs.begin("location", true);
  locLat  = prefs.getString("lat",  LOC_LAT);
  locLon  = prefs.getString("lon",  LOC_LON);
  locCity = prefs.getString("city", LOC_CITY);
  prefs.end();
}

void saveLocationConfig() {
  prefs.begin("location", false);
  prefs.putString("lat",  locLat);
  prefs.putString("lon",  locLon);
  prefs.putString("city", locCity);
  prefs.end();
}

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
  const char* city = locCity.c_str();
  int16_t wc = u8g2.getStrWidth(city);
  u8g2.drawStr((OLED_WIDTH - wc) / 2, 11, city);

  // ── Linha 2: temperatura grande (esquerda) ────────────────────────
  char tempBuf[8];
  snprintf(tempBuf, sizeof(tempBuf), "%.0f", cachedTemp);
  u8g2.setFont(u8g2_font_fub30_tf);
  int16_t wt = u8g2.getStrWidth(tempBuf);
  u8g2.drawStr(4, 46, tempBuf);

  // Simbolo de grau + C em fonte menor ao lado
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(wt + 6, 26, "o");   // grau
  u8g2.drawStr(wt + 6, 40, "C");

  // ── Icone de clima (Open Iconic Weather 2x = 16x16) ──────────────
  // Posiciona no lado direito
  int iconX = 88;
  int iconY = 28;   // baseline do glifo
  u8g2.setFont(u8g2_font_open_iconic_weather_2x_t);
  u8g2.drawGlyph(iconX, iconY, wmoGlyph(cachedWCode));

  // Descricao abaixo do icone
  u8g2.setFont(u8g2_font_5x7_tr);
  const char* desc = wmoDesc(cachedWCode);
  int16_t wd = u8g2.getStrWidth(desc);
  u8g2.drawStr(iconX + (16 - wd) / 2, 44, desc);

  // ── Linha inferior: mínima/máxima do dia atual ────────────────────
  u8g2.drawHLine(0, 50, OLED_WIDTH);
  char mmBuf[20];
  snprintf(mmBuf, sizeof(mmBuf), "Min %.0fo  Max %.0fo", todayMin, todayMax);
  int16_t wmm = u8g2.getStrWidth(mmBuf);
  u8g2.drawStr((OLED_WIDTH - wmm) / 2, 61, mmBuf);

  u8g2.sendBuffer();
}

// ── Tela: previsão dos próximos 3 dias (touch no modo clima) ──────────
void drawForecast() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_5x7_tr);
  const char* title = "Previsao (max/min)";
  int16_t wTitle = u8g2.getStrWidth(title);
  u8g2.drawStr((OLED_WIDTH - wTitle) / 2, 8, title);
  u8g2.drawHLine(0, 10, OLED_WIDTH);

  int colW = OLED_WIDTH / 3;
  for (int i = 0; i < 3; i++) {
    int colX = i * colW;
    int cx    = colX + colW / 2;

    // Dia da semana (amanhã, depois de amanhã, em 3 dias)
    int wday = (cachedWDay + i + 1) % 7;
    u8g2.setFont(u8g2_font_5x7_tr);
    int16_t ww = u8g2.getStrWidth(wdayNames[wday]);
    u8g2.drawStr(cx - ww / 2, 20, wdayNames[wday]);

    // Ícone de tempo (mesmo conjunto Open Iconic Weather 2x do clima atual)
    u8g2.setFont(u8g2_font_open_iconic_weather_2x_t);
    u8g2.drawGlyph(cx - 8, 38, wmoGlyph(forecastWCode[i]));

    // Temperatura máxima/mínima
    char buf[12];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "%.0f/%.0f", forecastMax[i], forecastMin[i]);
    int16_t wb = u8g2.getStrWidth(buf);
    u8g2.drawStr(cx - wb / 2, 50, buf);

    if (i > 0) u8g2.drawVLine(colX, 12, 40);
  }

  u8g2.sendBuffer();
}

// ── Tela: status do dispositivo (3º click do botão) ───────────────────
// Reúne rede/IP (como acessar o painel web) + uptime + memória livre.
void drawStatus() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  const char* title = "Status";
  int16_t wTitle = u8g2.getStrWidth(title);
  u8g2.drawStr((OLED_WIDTH - wTitle) / 2, 10, title);
  u8g2.drawHLine(0, 13, OLED_WIDTH);

  char buf[28];
  int y = 25;

  if (webWifiConnected) {
    snprintf(buf, sizeof(buf), "Painel: %s", WiFi.localIP().toString().c_str());
    u8g2.drawStr(2, y, buf); y += 11;
  } else {
    snprintf(buf, sizeof(buf), "Rede: %s", WIFI_AP_NAME);
    u8g2.drawStr(2, y, buf); y += 11;
    snprintf(buf, sizeof(buf), "Acesse: %s", AP_IP.toString().c_str());
    u8g2.drawStr(2, y, buf); y += 11;
  }

  unsigned long upSec = millis() / 1000UL;
  snprintf(buf, sizeof(buf), "Uptime: %luh %lum", upSec / 3600UL, (upSec % 3600UL) / 60UL);
  u8g2.drawStr(2, y, buf); y += 11;

  snprintf(buf, sizeof(buf), "Heap livre: %uKB", ESP.getFreeHeap() / 1024);
  u8g2.drawStr(2, y, buf);

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
  if (eye == EYE_NEUTRO || eye == EYE_CURIOSO) {
    // Neutro: centrado, só piscando. Curioso: olhando em volta (ambos piscam).
    stateDuration = (eye == EYE_NEUTRO)
                      ? (dayMode ? neutralHoldCfg : neutralDuration())
                      : curiousDurationCfg;
    blinkTimer    = millis();
    nextBlinkIn   = nextBlinkDelay();
    blinking      = false;
  } else if (eye == EYE_BLINK) {
    stateDuration = BLINK_DURATION;
  } else {
    stateDuration = dayMode ? exprHoldCfg : expressionDuration();
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
    EYE_SURPRESO,
    EYE_APAIXONADO,
    EYE_PISCADINHA,
    EYE_CONFUSO,
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
  } else if (currentEye == EYE_CURIOSO) {
    // Modo curioso: segue currentX dos olhos (só ele se move; o neutro fica centrado)
    // currentX varia entre -EYE_RANGE_X e +EYE_RANGE_X
    // mapeia para servoCenterCfg ± servoRangeCfg
    float ratio = currentX / (float)EYE_RANGE_X;  // -1.0 a +1.0
    targetAngle = servoCenterCfg + ratio * servoRangeCfg;
  } else {
    // Em expressões: centraliza
    targetAngle = servoCenterCfg;
  }

  // Suaviza o movimento
  servoCurrentAngle += (targetAngle - servoCurrentAngle) * servoSmoothCfg;

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
    // Modo neutro: olhos sempre centrados, só piscando.
    currentX = 0; currentY = 0;
    if (now - stateTimer > stateDuration) {
      previousEye = EYE_NEUTRO;
      if (dayMode && now - curiousTimer > curiousIntervalCfg) {
        enterState(EYE_CURIOSO);  // pausa curiosa a cada CURIOUS_INTERVAL
      } else {
        enterState(randomExpression());
        if (dayMode) beep(DAY_EXPR_BEEP_FREQ, DAY_EXPR_BEEP_DUR);  // aviso sonoro baixo (modo dia)
      }
      return;
    }
    if (!blinking && now - blinkTimer > nextBlinkIn) {
      previousEye        = EYE_NEUTRO;
      savedStateTimer    = stateTimer;
      savedStateDuration = stateDuration;
      enterState(EYE_BLINK);
    }
  } else if (currentEye == EYE_CURIOSO) {
    // Modo curioso: olhos se movendo/olhando em volta (antigo comportamento do neutro).
    updateMovement();
    if (now - stateTimer > stateDuration) {
      previousEye = EYE_CURIOSO;
      enterState(EYE_NEUTRO);
      curiousTimer = now;  // reinicia a contagem dos 50s a partir da volta ao ciclo aleatório
      return;
    }
    if (!blinking && now - blinkTimer > nextBlinkIn) {
      previousEye        = EYE_CURIOSO;
      savedStateTimer    = stateTimer;
      savedStateDuration = stateDuration;
      enterState(EYE_BLINK);
    }
  } else if (currentEye == EYE_BLINK) {
    if (now - stateTimer > stateDuration) {
      // Retoma o neutro/curioso exatamente de onde parou — NÃO usa enterState()
      // aqui, pois isso reiniciaria o cronômetro a cada piscada e o ciclo
      // nunca completaria os 20s (piscadas acontecem a cada 5-9s).
      currentEye    = previousEye == EYE_CURIOSO ? EYE_CURIOSO : EYE_NEUTRO;
      stateTimer    = savedStateTimer;
      stateDuration = savedStateDuration;
      blinkTimer    = millis();
      nextBlinkIn   = nextBlinkDelay();
    }
  } else {
    currentX = 0; currentY = 0;
    if (now - stateTimer > stateDuration) {
      previousEye = currentEye;
      if (dayMode && now - curiousTimer > curiousIntervalCfg) {
        enterState(EYE_CURIOSO);
      } else {
        enterState(EYE_NEUTRO);
      }
    }
  }
}

// ── Modo sono por horário ─────────────────────────────────────────────
void checkSleepMode() {
  if (!ntpSynced) return;
  tickClock();

  int h = cachedHour;
  bool shouldSleep = (h >= sleepStartCfg || h < sleepEndCfg);

  if (shouldSleep && !sleepMode) {
    // Entra no modo sono
    sleepMode = true;
    dayMode   = false;
    forcedEye = EYE_DORMIDO;
    Serial.printf("[Sleep] Entrando em modo sono (%02d:xx)\n", h);

  } else if (!shouldSleep && sleepMode) {
    // Acorda — entra no modo dia
    sleepMode    = false;
    dayMode      = true;
    forcedEye    = EYE_NEUTRO;
    curiousTimer = millis();  // reinicia a contagem dos 50s a partir do despertar
    Serial.printf("[Sleep] Acordando (%02d:xx) — modo dia ativo\n", h);
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
  // "daily" traz max/min/código de tempo dos próximos dias (index 0 = hoje);
  // forecast_days=4 garante os 3 dias seguintes a hoje em índices 1..3.
  String url = "https://api.open-meteo.com/v1/forecast"
               "?latitude=" + locLat +
               "&longitude=" + locLon +
               "&current_weather=true"
               "&daily=temperature_2m_max,temperature_2m_min,weathercode"
               "&forecast_days=4"
               "&timezone=auto"
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
      todayMax    = doc["daily"]["temperature_2m_max"][0] | 0.0f;
      todayMin    = doc["daily"]["temperature_2m_min"][0] | 0.0f;
      weatherReady     = true;
      lastWeatherFetch = millis();
      Serial.printf("[Weather] Temp=%.1f Code=%d Min/Max=%.0f/%.0f\n",
                     cachedTemp, cachedWCode, todayMin, todayMax);

      for (int i = 0; i < 3; i++) {
        forecastMax[i]   = doc["daily"]["temperature_2m_max"][i + 1] | 0.0f;
        forecastMin[i]   = doc["daily"]["temperature_2m_min"][i + 1] | 0.0f;
        forecastWCode[i] = doc["daily"]["weathercode"][i + 1]       | 0;
      }
      forecastReady = true;
      Serial.printf("[Weather] Previsao: %.0f/%.0f, %.0f/%.0f, %.0f/%.0f\n",
                     forecastMax[0], forecastMin[0], forecastMax[1], forecastMin[1],
                     forecastMax[2], forecastMin[2]);
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

// Previsão dos próximos 3 dias — usa o mesmo cache/fetch do clima atual
void activateForecast() {
  specialMode    = MODE_FORECAST;
  specialModeEnd = millis() + SPECIAL_MODE_DURATION;
  beep(BUZZER_FREQ_HIGH);
  if (!forecastReady || millis() - lastWeatherFetch > WEATHER_RESYNC_INTERVAL) doSyncWeather();
}

void activateFocusSetup() {
  specialMode  = MODE_FOCUS_SETUP;
  focusMinutes = FOCUS_MIN_TIME;  // reseta para o mínimo
  beep(BUZZER_FREQ_HIGH);
}

void activateStatus() {
  specialMode    = MODE_STATUS;
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
        // clique curto no setup (3º click) → mostra status do dispositivo
        activateStatus();
      }
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO STATUS: qualquer clique volta ao neutro (3º click do botão)
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_STATUS) {
    if (button.getSingleDebouncedPress() || touch.getSingleDebouncedPress()) {
      specialMode = MODE_NONE;
      forcedEye   = EYE_NEUTRO;
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO CLOCK: touch troca para clima atual
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
  //  MODO CLIMA: touch avança para a previsão dos próximos 3 dias
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_WEATHER) {
    if (touch.getSingleDebouncedPress()) {
      activateForecast();
    }
    if (button.getSingleDebouncedPress()) {
      activateFocusSetup();
    }
    return;
  }

  // ═══════════════════════════════════════════════════════════════════
  //  MODO PREVISÃO: touch volta para a hora
  // ═══════════════════════════════════════════════════════════════════
  if (specialMode == MODE_FORECAST) {
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
void heartEye(int posX, int posY, int size) {
  int r = size / 2;
  u8g2.drawDisc(posX - r/2, posY - r/3, r/2);
  u8g2.drawDisc(posX + r/2, posY - r/3, r/2);
  u8g2.drawTriangle(posX - r, posY - r/4, posX + r, posY - r/4, posX, posY + r);
}

void renderEyes(int tamX, int tamY, int esp) {
  int lPosX = round(currentX) + EYE_LEFT;
  int rPosX = round(currentX) + EYE_RIGHT;
  int posY  = round(currentY) + EYE_CENTER_Y;

  switch (currentEye) {
    case EYE_NEUTRO:
    case EYE_CURIOSO:  // mesmo desenho do neutro — a diferença é o olhar se mover
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
    case EYE_SURPRESO:
      for (int i = 0; i < esp; i++) { normalEye(lPosX, posY, tamX+i+6, tamY+i+6); normalEye(rPosX, posY, tamX+i+6, tamY+i+6); }
      break;
    case EYE_APAIXONADO:
      { int hs = tamX + tamY;  // coração maior que o padrão dos outros olhos
        heartEye(lPosX, posY, hs);
        heartEye(rPosX, posY, hs); }
      break;
    case EYE_PISCADINHA:
      for (int i = 0; i < esp; i++) { normalEye(lPosX, posY, tamX+i, tamY+i); }
      for (int i = 0; i < esp; i++) { lineEye(rPosX-15, posY+i, tamX+20); }
      break;
    case EYE_CONFUSO:
      for (int i = 0; i < esp; i++) { halfEye(lPosX, posY, tamX+i, 0, 130); halfEye(rPosX, posY-6, tamX+i, -160, -35); }
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(rPosX+10, posY-18, "?");
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
  :root{
    --bg:#0f1115; --surface:#181c23; --surface2:#20252e; --border:#2a2f3a;
    --accent:#4af; --accent-dim:#2c6fa3; --text:#eee; --muted:#9aa4b2;
    --danger:#f66; --warn:#fa4; --ok:#4d8;
  }
  *{box-sizing:border-box}
  body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);margin:0}
  h1{color:var(--accent);margin:0 0 4px;font-size:1.3rem}
  p{color:var(--muted);margin:4px 0 16px}
  input{width:100%;max-width:320px;padding:10px 14px;border-radius:8px;
        border:2px solid var(--accent);background:var(--surface2);color:var(--text);font-size:1rem;margin:6px 0}
  input[type=range]{padding:0;border:none;background:transparent;height:24px;accent-color:var(--accent)}
  select{padding:8px 10px;border-radius:8px;border:2px solid var(--accent);
         background:var(--surface2);color:var(--text);font-size:.95rem;width:100%}
  .servo-vals{font-size:.8rem;color:var(--muted);margin:2px 0 8px}
  button{background:var(--surface2);color:var(--text);border:2px solid var(--accent);border-radius:8px;
         padding:12px 20px;font-size:1rem;cursor:pointer;min-width:110px;margin:4px;
         transition:background .15s,transform .1s}
  button:hover{background:var(--accent);color:#111}
  button:active{transform:scale(.97)}
  .grid{display:flex;flex-wrap:wrap;justify-content:center;gap:10px;margin:20px 0}
  #status{margin-top:16px;font-size:.9rem;color:var(--muted)}
  .card{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:24px;max-width:360px;margin:0 auto}
  label{display:block;text-align:left;font-size:.85rem;color:var(--muted);margin-top:10px}
  .net-list{list-style:none;padding:0;margin:10px 0;max-height:200px;overflow-y:auto}
  .net-item{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;margin:4px 0;
            background:var(--surface2);border:1px solid var(--border);border-radius:8px;cursor:pointer;transition:background .2s}
  .net-item:hover,.net-item.selected{background:var(--accent-dim);border-color:var(--accent)}
  .net-ssid{font-size:.95rem;font-weight:bold}
  .net-info{font-size:.75rem;color:#888}
  .scan-btn{font-size:.8rem;padding:6px 14px;margin-bottom:10px}
  .separator{border:none;border-top:1px solid var(--border);margin:14px 0}
  .center-page{text-align:center;padding:24px}
</style>
)css";

// ── CSS específico do painel de controle (app com menu lateral) ────────
static const char* PANEL_STYLE = R"css(
<style>
  html,body{height:100%}
  body{overflow:hidden}
  .app{display:flex;flex-direction:column;height:100vh}
  .topbar{display:flex;align-items:center;justify-content:space-between;gap:10px;
          padding:10px 16px;background:var(--surface);border-bottom:1px solid var(--border);flex-shrink:0}
  .topbar h1{font-size:1.05rem}
  .statusline{font-size:.72rem;color:var(--muted);text-align:right;line-height:1.3;white-space:nowrap}
  .statusline b{color:var(--text)}
  .layout{display:flex;flex:1;min-height:0}
  .sidebar{display:flex;flex-direction:column;gap:4px;width:86px;flex-shrink:0;
           background:var(--surface);border-right:1px solid var(--border);padding:10px 6px;overflow-y:auto}
  .nav-item{display:flex;flex-direction:column;align-items:center;gap:2px;
            background:none;border:2px solid transparent;border-radius:10px;padding:10px 4px;
            color:var(--muted);cursor:pointer;min-width:0;margin:0;font-size:.66rem;width:100%}
  .nav-item .nav-icon{font-size:1.5rem}
  .nav-item:hover{background:var(--surface2);color:var(--text)}
  .nav-item.active{background:var(--accent-dim);border-color:var(--accent);color:#fff}
  .content{flex:1;overflow-y:auto;padding:18px}
  .tab{display:none}
  .tab.active{display:block}
  .emo-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(78px,1fr));gap:10px;max-width:640px;margin:0 auto}
  .emo-card{display:flex;flex-direction:column;align-items:center;gap:4px;
            background:var(--surface);border:1px solid var(--border);border-radius:12px;
            padding:14px 6px;cursor:pointer;transition:background .15s,border-color .15s}
  .emo-card:hover{background:var(--surface2);border-color:var(--accent)}
  .emo-card .emo-icon{font-size:1.8rem}
  .emo-card .emo-label{font-size:.7rem;color:var(--muted)}
  .settings-card{max-width:420px;margin:0 auto 18px}
  .settings-card h2{font-size:.95rem;color:var(--text);margin:0 0 4px}
  .settings-card .sub{font-size:.75rem;color:var(--muted);margin:0 0 12px}
  .btn-row{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin-top:8px}
  .btn-sm{font-size:.8rem;padding:8px 14px;min-width:auto}
  .btn-save{border-color:var(--ok);color:var(--ok)}
  .btn-warn{border-color:var(--warn);color:var(--warn)}
  .btn-danger{border-color:var(--danger);color:var(--danger)}
  .sleep-row{display:flex;gap:10px}
  .sleep-row>div{flex:1}
  #panelStatus{text-align:center;max-width:640px;margin:14px auto 0}
  @media (max-width:420px){
    .sidebar{width:68px} .nav-item{font-size:.6rem} .nav-item .nav-icon{font-size:1.3rem}
    .statusline{font-size:.65rem}
  }
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
<div class="center-page">
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
</div>
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
    "<title>DrawEye - Painel</title>";
  html += PAGE_STYLE;
  html += PANEL_STYLE;
  html += R"rawhtml(
</head><body>
<div class="app">
  <header class="topbar">
    <h1>&#128065; DrawEye</h1>
    <div class="statusline" id="statusline">Carregando&hellip;</div>
  </header>
  <div class="layout">
    <nav class="sidebar">
      <button class="nav-item active" id="nav-emocoes" onclick="showTab('emocoes')">
        <span class="nav-icon">&#128522;</span><span>Emoções</span>
      </button>
      <button class="nav-item" id="nav-ajustes" onclick="showTab('ajustes')">
        <span class="nav-icon">&#9881;&#65039;</span><span>Ajustes</span>
      </button>
      <button class="nav-item" id="nav-config" onclick="showTab('config')">
        <span class="nav-icon">&#127760;</span><span>Configurações</span>
      </button>
    </nav>
    <main class="content">
      <section id="tab-emocoes" class="tab active">
        <div class="emo-grid">
          <div class="emo-card" onclick="setEye('neutro')"><span class="emo-icon">&#128528;</span><span class="emo-label">Neutro</span></div>
          <div class="emo-card" onclick="setEye('feliz')"><span class="emo-icon">&#128522;</span><span class="emo-label">Feliz</span></div>
          <div class="emo-card" onclick="setEye('triste')"><span class="emo-icon">&#128546;</span><span class="emo-label">Triste</span></div>
          <div class="emo-card" onclick="setEye('malvado')"><span class="emo-icon">&#128520;</span><span class="emo-label">Malvado</span></div>
          <div class="emo-card" onclick="setEye('dormido')"><span class="emo-icon">&#128564;</span><span class="emo-label">Dormido</span></div>
          <div class="emo-card" onclick="setEye('cansado')"><span class="emo-icon">&#129396;</span><span class="emo-label">Cansado</span></div>
          <div class="emo-card" onclick="setEye('furia')"><span class="emo-icon">&#128545;</span><span class="emo-label">Fúria</span></div>
          <div class="emo-card" onclick="setEye('rascal')"><span class="emo-icon">&#128527;</span><span class="emo-label">Rascal</span></div>
          <div class="emo-card" onclick="setEye('surpreso')"><span class="emo-icon">&#128562;</span><span class="emo-label">Surpreso</span></div>
          <div class="emo-card" onclick="setEye('apaixonado')"><span class="emo-icon">&#128525;</span><span class="emo-label">Apaixonado</span></div>
          <div class="emo-card" onclick="setEye('piscadinha')"><span class="emo-icon">&#128521;</span><span class="emo-label">Piscadinha</span></div>
          <div class="emo-card" onclick="setEye('confuso')"><span class="emo-icon">&#128533;</span><span class="emo-label">Confuso</span></div>
          <div class="emo-card" onclick="setEye('random')"><span class="emo-icon">&#127922;</span><span class="emo-label">Aleatório</span></div>
        </div>
      </section>

      <section id="tab-ajustes" class="tab">
        <div class="card settings-card">
          <h2>&#9881;&#65039; Servo (cabeça)</h2>
          <p class="sub">Posição central, amplitude e suavidade do movimento</p>
          <label>Centro: <span id="centerVal">90</span>&deg;</label>
          <input type="range" id="servoCenter" min="0" max="180" value="90" oninput="atualizarLabelsServo()" onchange="aplicarServo()">
          <label>Amplitude para cada lado: <span id="rangeVal">30</span>&deg;</label>
          <input type="range" id="servoRange" min="0" max="90" value="30" oninput="atualizarLabelsServo()" onchange="aplicarServo()">
          <p class="servo-vals">Vai de <b id="minAngle">60</b>&deg; a <b id="maxAngle">120</b>&deg;</p>
          <label>Suavidade: <span id="smoothVal">0.09</span> (menor = mais lento)</label>
          <input type="range" id="servoSmooth" min="2" max="50" value="9" oninput="atualizarLabelsServo()" onchange="aplicarServo()">
          <div class="btn-row">
            <button class="btn-sm" onclick="testarServo()">Testar</button>
            <button class="btn-sm btn-save" onclick="salvarServo()">Salvar</button>
          </div>
        </div>

        <div class="card settings-card">
          <h2>&#128065; Tamanho dos olhos</h2>
          <p class="sub">Raio horizontal, raio vertical e espessura do traço</p>
          <label>Raio horizontal: <span id="sizeXVal">14</span>px</label>
          <input type="range" id="eyeSizeX" min="5" max="30" value="14" oninput="atualizarLabelsOlhos()" onchange="aplicarOlhos()">
          <label>Raio vertical: <span id="sizeYVal">15</span>px</label>
          <input type="range" id="eyeSizeY" min="5" max="30" value="15" oninput="atualizarLabelsOlhos()" onchange="aplicarOlhos()">
          <label>Espessura: <span id="thickVal">6</span></label>
          <input type="range" id="eyeThick" min="1" max="12" value="6" oninput="atualizarLabelsOlhos()" onchange="aplicarOlhos()">
          <div class="btn-row">
            <button class="btn-sm btn-save" onclick="salvarOlhos()">Salvar</button>
          </div>
        </div>

        <div class="card settings-card">
          <h2>&#127769; Horário do modo sono</h2>
          <p class="sub">Fora deste intervalo, o modo dia fica ativo</p>
          <div class="sleep-row">
            <div>
              <label>Dorme às</label>
              <select id="sleepStart" onchange="aplicarSono()"></select>
            </div>
            <div>
              <label>Acorda às</label>
              <select id="sleepEnd" onchange="aplicarSono()"></select>
            </div>
          </div>
          <div class="btn-row">
            <button class="btn-sm btn-save" onclick="salvarSono()">Salvar</button>
          </div>
        </div>

        <div class="card settings-card">
          <h2>&#9203; Tempo dos modos automáticos</h2>
          <p class="sub">Ciclo: neutro → expressão → neutro..., com pausas curiosas</p>
          <label>Neutro (centrado, piscando): <span id="neutralVal">20</span>s</label>
          <input type="range" id="timingNeutral" min="3" max="120" value="20" oninput="atualizarLabelsTiming()" onchange="aplicarTiming()">
          <label>Expressão aleatória: <span id="exprVal">5</span>s</label>
          <input type="range" id="timingExpr" min="1" max="60" value="5" oninput="atualizarLabelsTiming()" onchange="aplicarTiming()">
          <label>Intervalo até o modo curioso: <span id="curIntVal">50</span>s</label>
          <input type="range" id="timingCurInt" min="5" max="600" value="50" oninput="atualizarLabelsTiming()" onchange="aplicarTiming()">
          <label>Duração do modo curioso: <span id="curDurVal">20</span>s</label>
          <input type="range" id="timingCurDur" min="3" max="120" value="20" oninput="atualizarLabelsTiming()" onchange="aplicarTiming()">
          <div class="btn-row">
            <button class="btn-sm btn-save" onclick="salvarTiming()">Salvar</button>
          </div>
        </div>
      </section>

      <section id="tab-config" class="tab">
        <div class="card settings-card">
          <h2>&#128205; Localização (clima)</h2>
          <p class="sub">Usada para buscar clima atual e previsão via Open-Meteo</p>
          <label>Cidade (exibida na tela de clima)</label>
          <input type="text" id="locCity" placeholder="Ex: Barueri">
          <label>Latitude</label>
          <input type="text" id="locLat" placeholder="Ex: -23.5114">
          <label>Longitude</label>
          <input type="text" id="locLon" placeholder="Ex: -46.8756">
          <div class="btn-row">
            <button class="btn-sm btn-save" onclick="salvarLocalizacao()">Salvar</button>
          </div>
        </div>

        <div class="card settings-card">
          <h2>&#128225; Wi-Fi</h2>
          <p class="sub">Rede utilizada pelo DrawEye para o painel e sincronizações</p>
          <div class="btn-row">
            <button class="btn-sm" onclick="window.location.href='/'">Trocar Wi-Fi</button>
            <button class="btn-sm btn-danger" onclick="esquecer()">Esquecer rede</button>
          </div>
        </div>

        <div class="card settings-card">
          <h2>&#128295; Dispositivo</h2>
          <p class="sub" id="deviceInfo">&mdash;</p>
          <div class="btn-row">
            <button class="btn-sm btn-warn" onclick="reiniciar()">Reiniciar</button>
          </div>
        </div>
      </section>

      <p id="panelStatus">Pronto.</p>
    </main>
  </div>
</div>
<script>
function showTab(name){
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.querySelectorAll('.nav-item').forEach(n=>n.classList.remove('active'));
  document.getElementById('tab-'+name).classList.add('active');
  document.getElementById('nav-'+name).classList.add('active');
}

async function setEye(name){
  document.getElementById('panelStatus').textContent='Enviando: '+name+'...';
  try{const r=await fetch('/eye?name='+name);const t=await r.text();document.getElementById('panelStatus').textContent=t;}
  catch(e){document.getElementById('panelStatus').textContent='Erro: '+e;}
}

// ── Servo ──────────────────────────────────────────────────────────
function atualizarLabelsServo(){
  const c = parseInt(document.getElementById('servoCenter').value);
  const r = parseInt(document.getElementById('servoRange').value);
  const s = parseInt(document.getElementById('servoSmooth').value);
  document.getElementById('centerVal').textContent = c;
  document.getElementById('rangeVal').textContent = r;
  document.getElementById('minAngle').textContent = Math.max(0, c - r);
  document.getElementById('maxAngle').textContent = Math.min(180, c + r);
  document.getElementById('smoothVal').textContent = (s/100).toFixed(2);
}
async function carregarServo(){
  try{
    const r = await fetch('/servo'); const j = await r.json();
    document.getElementById('servoCenter').value = j.center;
    document.getElementById('servoRange').value  = j.range;
    document.getElementById('servoSmooth').value = Math.round(j.smooth*100);
    atualizarLabelsServo();
  }catch(e){}
}
async function aplicarServo(){
  const c = document.getElementById('servoCenter').value;
  const r = document.getElementById('servoRange').value;
  const s = (document.getElementById('servoSmooth').value/100).toFixed(2);
  try{ await fetch('/servo?center='+c+'&range='+r+'&smooth='+s); }catch(e){}
}
async function testarServo(){
  await aplicarServo();
  document.getElementById('panelStatus').textContent='Testando servo...';
}
async function salvarServo(){
  const c = document.getElementById('servoCenter').value;
  const r = document.getElementById('servoRange').value;
  const s = (document.getElementById('servoSmooth').value/100).toFixed(2);
  try{
    await fetch('/servo?center='+c+'&range='+r+'&smooth='+s+'&save=1');
    document.getElementById('panelStatus').textContent='Servo salvo: centro '+c+'°, amplitude ±'+r+'°, smooth '+s;
  }catch(e){ document.getElementById('panelStatus').textContent='Erro: '+e; }
}

// ── Tamanho dos olhos ─────────────────────────────────────────────
function atualizarLabelsOlhos(){
  document.getElementById('sizeXVal').textContent = document.getElementById('eyeSizeX').value;
  document.getElementById('sizeYVal').textContent = document.getElementById('eyeSizeY').value;
  document.getElementById('thickVal').textContent = document.getElementById('eyeThick').value;
}
async function carregarOlhos(){
  try{
    const r = await fetch('/eyesize'); const j = await r.json();
    document.getElementById('eyeSizeX').value = j.sizex;
    document.getElementById('eyeSizeY').value = j.sizey;
    document.getElementById('eyeThick').value = j.thick;
    atualizarLabelsOlhos();
  }catch(e){}
}
async function aplicarOlhos(){
  const x = document.getElementById('eyeSizeX').value;
  const y = document.getElementById('eyeSizeY').value;
  const t = document.getElementById('eyeThick').value;
  try{ await fetch('/eyesize?sizex='+x+'&sizey='+y+'&thick='+t); }catch(e){}
}
async function salvarOlhos(){
  await aplicarOlhos();
  try{
    const x = document.getElementById('eyeSizeX').value;
    const y = document.getElementById('eyeSizeY').value;
    const t = document.getElementById('eyeThick').value;
    await fetch('/eyesize?sizex='+x+'&sizey='+y+'&thick='+t+'&save=1');
    document.getElementById('panelStatus').textContent='Tamanho dos olhos salvo.';
  }catch(e){ document.getElementById('panelStatus').textContent='Erro: '+e; }
}

// ── Horário de sono ───────────────────────────────────────────────
function preencherHoras(sel){
  for(let h=0;h<24;h++){
    const opt=document.createElement('option');
    opt.value=h; opt.textContent=String(h).padStart(2,'0')+':00';
    sel.appendChild(opt);
  }
}
async function carregarSono(){
  const selS=document.getElementById('sleepStart'), selE=document.getElementById('sleepEnd');
  preencherHoras(selS); preencherHoras(selE);
  try{
    const r = await fetch('/sleep'); const j = await r.json();
    selS.value = j.start; selE.value = j.end;
  }catch(e){}
}
async function aplicarSono(){
  const s = document.getElementById('sleepStart').value;
  const e = document.getElementById('sleepEnd').value;
  try{ await fetch('/sleep?start='+s+'&end='+e); }catch(err){}
}
async function salvarSono(){
  try{
    const s = document.getElementById('sleepStart').value;
    const e = document.getElementById('sleepEnd').value;
    await fetch('/sleep?start='+s+'&end='+e+'&save=1');
    document.getElementById('panelStatus').textContent='Horário de sono salvo.';
  }catch(err){ document.getElementById('panelStatus').textContent='Erro: '+err; }
}

// ── Tempo dos modos automáticos ────────────────────────────────────
function atualizarLabelsTiming(){
  document.getElementById('neutralVal').textContent = document.getElementById('timingNeutral').value;
  document.getElementById('exprVal').textContent    = document.getElementById('timingExpr').value;
  document.getElementById('curIntVal').textContent  = document.getElementById('timingCurInt').value;
  document.getElementById('curDurVal').textContent  = document.getElementById('timingCurDur').value;
}
async function carregarTiming(){
  try{
    const r = await fetch('/timing'); const j = await r.json();
    document.getElementById('timingNeutral').value = j.neutral;
    document.getElementById('timingExpr').value    = j.expr;
    document.getElementById('timingCurInt').value  = j.curint;
    document.getElementById('timingCurDur').value  = j.curdur;
    atualizarLabelsTiming();
  }catch(e){}
}
async function aplicarTiming(){
  const n = document.getElementById('timingNeutral').value;
  const e = document.getElementById('timingExpr').value;
  const ci = document.getElementById('timingCurInt').value;
  const cd = document.getElementById('timingCurDur').value;
  try{ await fetch('/timing?neutral='+n+'&expr='+e+'&curint='+ci+'&curdur='+cd); }catch(err){}
}
async function salvarTiming(){
  await aplicarTiming();
  try{
    const n = document.getElementById('timingNeutral').value;
    const e = document.getElementById('timingExpr').value;
    const ci = document.getElementById('timingCurInt').value;
    const cd = document.getElementById('timingCurDur').value;
    await fetch('/timing?neutral='+n+'&expr='+e+'&curint='+ci+'&curdur='+cd+'&save=1');
    document.getElementById('panelStatus').textContent='Timing salvo.';
  }catch(err){ document.getElementById('panelStatus').textContent='Erro: '+err; }
}

// ── Localização (clima) ────────────────────────────────────────────
async function carregarLocalizacao(){
  try{
    const r = await fetch('/location'); const j = await r.json();
    document.getElementById('locCity').value = j.city;
    document.getElementById('locLat').value  = j.lat;
    document.getElementById('locLon').value  = j.lon;
  }catch(e){}
}
async function salvarLocalizacao(){
  const city = document.getElementById('locCity').value.trim();
  const lat  = document.getElementById('locLat').value.trim();
  const lon  = document.getElementById('locLon').value.trim();
  try{
    await fetch('/location?city='+encodeURIComponent(city)+'&lat='+encodeURIComponent(lat)+'&lon='+encodeURIComponent(lon)+'&save=1');
    document.getElementById('panelStatus').textContent='Localização salva: '+city;
  }catch(e){ document.getElementById('panelStatus').textContent='Erro: '+e; }
}

// ── Status / dispositivo ──────────────────────────────────────────
function fmtUptime(sec){
  const h=Math.floor(sec/3600), m=Math.floor((sec%3600)/60);
  return h+'h '+m+'m';
}
async function atualizarStatus(){
  try{
    const r = await fetch('/status'); const j = await r.json();
    const modo = j.sleep ? '&#127769; Sono' : '&#9728;&#65039; Dia';
    document.getElementById('statusline').innerHTML =
      '<b>'+j.wifi+'</b> &middot; '+modo+' &middot; '+fmtUptime(j.uptime);
    document.getElementById('deviceInfo').textContent =
      'IP: '+j.wifi+' · Uptime: '+fmtUptime(j.uptime)+' · Heap livre: '+Math.round(j.heap/1024)+'KB · Olho atual: '+j.eye;
  }catch(e){}
}
async function reiniciar(){
  if(!confirm('Reiniciar o DrawEye agora?')) return;
  document.getElementById('panelStatus').textContent='Reiniciando... aguarde alguns segundos.';
  try{ await fetch('/restart'); }catch(e){}
}
async function esquecer(){
  if(!confirm('Apagar rede salva? O ESP abrira o portal no proximo boot.')) return;
  await fetch('/forget');
  alert('Rede apagada.');
}

carregarServo();
carregarOlhos();
carregarSono();
carregarTiming();
carregarLocalizacao();
atualizarStatus();
setInterval(atualizarStatus, 5000);
</script>
</body></html>
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
  json += "\"uptime\":" + String(millis()/1000) + ",";
  json += "\"sleep\":" + String(sleepMode ? "true" : "false") + ",";
  json += "\"day\":"   + String(dayMode   ? "true" : "false") + ",";
  json += "\"heap\":"  + String(ESP.getFreeHeap());
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

// ── Ajuste do servo: centro, amplitude e smooth ───────────────────────
void handleServo() {
  // Sem parâmetros → apenas consulta os valores atuais
  if (!server.hasArg("center") && !server.hasArg("range") && !server.hasArg("smooth")) {
    String json = "{\"center\":" + String(servoCenterCfg) +
                  ",\"range\":"  + String(servoRangeCfg) +
                  ",\"smooth\":" + String(servoSmoothCfg, 2) + "}";
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("center")) servoCenterCfg = constrain(server.arg("center").toInt(), 0, 180);
  if (server.hasArg("range"))  servoRangeCfg  = constrain(server.arg("range").toInt(), 0, 90);
  if (server.hasArg("smooth")) servoSmoothCfg = constrain(server.arg("smooth").toFloat(), 0.02f, 0.50f);

  // Garante que centro ± amplitude não estoure os limites físicos (0-180°)
  if (servoCenterCfg - servoRangeCfg < 0)   servoRangeCfg = servoCenterCfg;
  if (servoCenterCfg + servoRangeCfg > 180) servoRangeCfg = 180 - servoCenterCfg;

  // Move na hora para o usuário ver o resultado (teste ou salvo, tanto faz)
  servoCurrentAngle = servoCenterCfg;
  servoWrite(servoCenterCfg);

  if (server.hasArg("save")) saveServoConfig();

  String json = "{\"center\":" + String(servoCenterCfg) +
                ",\"range\":"  + String(servoRangeCfg) +
                ",\"smooth\":" + String(servoSmoothCfg, 2) + "}";
  server.send(200, "application/json", json);
}

// ── Ajuste do tamanho dos olhos: raio X, raio Y e espessura ───────────
void handleEyeSize() {
  if (!server.hasArg("sizex") && !server.hasArg("sizey") && !server.hasArg("thick")) {
    String json = "{\"sizex\":" + String(eyeSizeXCfg) +
                  ",\"sizey\":" + String(eyeSizeYCfg) +
                  ",\"thick\":" + String(eyeThicknessCfg) + "}";
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("sizex")) eyeSizeXCfg     = constrain(server.arg("sizex").toInt(), 5, 30);
  if (server.hasArg("sizey")) eyeSizeYCfg     = constrain(server.arg("sizey").toInt(), 5, 30);
  if (server.hasArg("thick")) eyeThicknessCfg = constrain(server.arg("thick").toInt(), 1, 12);

  if (server.hasArg("save")) saveEyeConfig();

  String json = "{\"sizex\":" + String(eyeSizeXCfg) +
                ",\"sizey\":" + String(eyeSizeYCfg) +
                ",\"thick\":" + String(eyeThicknessCfg) + "}";
  server.send(200, "application/json", json);
}

// ── Ajuste do horário do modo sono: hora de início e de fim ───────────
void handleSleepCfg() {
  if (!server.hasArg("start") && !server.hasArg("end")) {
    String json = "{\"start\":" + String(sleepStartCfg) +
                  ",\"end\":"   + String(sleepEndCfg) + "}";
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("start")) sleepStartCfg = constrain(server.arg("start").toInt(), 0, 23);
  if (server.hasArg("end"))   sleepEndCfg   = constrain(server.arg("end").toInt(), 0, 23);

  if (server.hasArg("save")) saveSleepConfig();

  String json = "{\"start\":" + String(sleepStartCfg) +
                ",\"end\":"   + String(sleepEndCfg) + "}";
  server.send(200, "application/json", json);
}

// ── Localização usada para o clima (lat/lon/cidade) ───────────────────
void handleLocation() {
  if (!server.hasArg("lat") && !server.hasArg("lon") && !server.hasArg("city")) {
    String json = "{\"lat\":\"" + locLat + "\",\"lon\":\"" + locLon + "\",\"city\":\"" + locCity + "\"}";
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("lat"))  locLat  = server.arg("lat");
  if (server.hasArg("lon"))  locLon  = server.arg("lon");
  if (server.hasArg("city")) locCity = server.arg("city");

  // Localização mudou — invalida o cache para forçar nova busca na próxima vez
  weatherReady  = false;
  forecastReady = false;

  if (server.hasArg("save")) saveLocationConfig();

  String json = "{\"lat\":\"" + locLat + "\",\"lon\":\"" + locLon + "\",\"city\":\"" + locCity + "\"}";
  server.send(200, "application/json", json);
}

// ── Timing dos modos automáticos (neutro/curioso/expressão), em segundos ─
void handleTiming() {
  if (!server.hasArg("neutral") && !server.hasArg("expr") &&
      !server.hasArg("curint")  && !server.hasArg("curdur")) {
    String json = "{\"neutral\":" + String(neutralHoldCfg / 1000) +
                  ",\"expr\":"    + String(exprHoldCfg / 1000) +
                  ",\"curint\":"  + String(curiousIntervalCfg / 1000) +
                  ",\"curdur\":"  + String(curiousDurationCfg / 1000) + "}";
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("neutral")) neutralHoldCfg     = (unsigned long)constrain(server.arg("neutral").toInt(), 3, 120)  * 1000UL;
  if (server.hasArg("expr"))    exprHoldCfg        = (unsigned long)constrain(server.arg("expr").toInt(),    1, 60)   * 1000UL;
  if (server.hasArg("curint"))  curiousIntervalCfg = (unsigned long)constrain(server.arg("curint").toInt(),  5, 600)  * 1000UL;
  if (server.hasArg("curdur"))  curiousDurationCfg = (unsigned long)constrain(server.arg("curdur").toInt(),  3, 120)  * 1000UL;

  if (server.hasArg("save")) saveTimingConfig();

  String json = "{\"neutral\":" + String(neutralHoldCfg / 1000) +
                ",\"expr\":"    + String(exprHoldCfg / 1000) +
                ",\"curint\":"  + String(curiousIntervalCfg / 1000) +
                ",\"curdur\":"  + String(curiousDurationCfg / 1000) + "}";
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
  server.on("/eyesize", handleEyeSize);
  server.on("/sleep",   handleSleepCfg);
  server.on("/location", handleLocation);
  server.on("/timing",   handleTiming);
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

  // Servo / olhos / sono / localização / timing — configs persistidas em NVS
  loadServoConfig();
  loadEyeConfig();
  loadSleepConfig();
  loadLocationConfig();
  loadTimingConfig();
  ESP32PWM::allocateTimer(0);
  headServo.setPeriodHertz(50);
  headServo.attach(SERVO_PIN, 500, 2400);
  servoCurrentAngle = servoCenterCfg;
  headServo.write(servoCenterCfg);
  delay(500);
  setupWiFi();
  enterState(EYE_NEUTRO);
  curiousTimer = millis();
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

    // Clock / Weather / Forecast / Status: exibe por SPECIAL_MODE_DURATION
    if (millis() < specialModeEnd) {
      if (specialMode == MODE_CLOCK)    drawClock();
      if (specialMode == MODE_WEATHER)  drawWeather();
      if (specialMode == MODE_FORECAST) drawForecast();
      if (specialMode == MODE_STATUS)   drawStatus();
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
  renderEyes(eyeSizeXCfg, eyeSizeYCfg, eyeThicknessCfg);
  u8g2.sendBuffer();
  delay(LOOP_DELAY_MS);
}