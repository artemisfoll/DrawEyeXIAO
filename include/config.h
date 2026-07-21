#pragma once

// ══════════════════════════════════════════════════════════════════════
//  DrawEye — Configurações Manuais
//  Edite este arquivo para ajustar o comportamento sem mexer no código.
// ══════════════════════════════════════════════════════════════════════

// ── Pinos (XIAO ESP32-C6) ────────────────────────────────────────────
#define BUZZER_PIN   2    // GPIO 02 → buzzer piezo
#define BUTTON_PIN   0    // GPIO 00 → botão físico (muda expressão)
#define TOUCH_PIN    1    // GPIO 01 → botão toque capacitivo

// ── Display OLED ─────────────────────────────────────────────────────
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

// ── Posições dos olhos no display ────────────────────────────────────
#define EYE_RIGHT    96   // centro X do olho direito
#define EYE_LEFT     32   // centro X do olho esquerdo
#define EYE_CENTER_Y 40   // centro Y base dos olhos

// ── Tamanho padrão dos olhos ─────────────────────────────────────────
#define EYE_SIZE_X   14.5   // raio horizontal
#define EYE_SIZE_Y   15   // raio vertical
#define EYE_THICKNESS 6   // espessura (número de elipses sobrepostas)

// ── Movimentos oculares ──────────────────────────────────────────────
#define EYE_MOVE_SPEED     0.5f   // suavidade do movimento (0.1 lento … 1.0 instantâneo)
#define EYE_MOVE_INTERVAL  4000   // ms entre mudanças de direção do olhar
#define EYE_RANGE_X        15     // pixels de deslocamento horizontal
#define EYE_RANGE_Y_UP     20     // pixels máximos para cima
#define EYE_RANGE_Y_DOWN   6      // pixels máximos para baixo

// ── Temporização das expressões ──────────────────────────────────────
#define NEUTRAL_DURATION_MIN  15000   // ms
#define NEUTRAL_DURATION_MAX  20000
#define EXPR_DURATION_MIN     2000
#define EXPR_DURATION_MAX     5000
#define BLINK_DURATION        150    // ms

// ── Intervalo entre piscadas ─────────────────────────────────────────
#define BLINK_INTERVAL_MIN   5000   // ms
#define BLINK_INTERVAL_MAX  9000

// ── Buzzer ───────────────────────────────────────────────────────────
#define BUZZER_FREQ_LOW   800    // Hz — som neutro / toque
#define BUZZER_FREQ_HIGH  1200   // Hz — confirmação
#define BUZZER_DURATION   80     // ms

// ── Wi-Fi / Web server ───────────────────────────────────────────────
#define WIFI_AP_NAME     "DrawEye"   // nome da rede do portal captive
#define WIFI_AP_PASSWORD ""          // deixe vazio para rede aberta
#define WEB_SERVER_PORT  80

// ── Ciclo principal ──────────────────────────────────────────────────
#define LOOP_DELAY_MS    20   // ms entre frames (~50 fps)

// ── Servo ─────────────────────────────────────────────────
#define SERVO_CENTER  90   // posição central em graus
#define SERVO_RANGE   30   // amplitude para cada lado
#define SERVO_SMOOTH  0.09f // 0.02 = muito lento, 0.15 = mais rápido

// ── Modo foco ────────────────────────────────────────────────────────
#define FOCUS_MIN_TIME    5    // minutos mínimos
#define FOCUS_MAX_TIME    60   // minutos máximos
#define FOCUS_STEP        5    // incremento por toque (minutos)
#define FOCUS_HOLD_MS     800  // ms para considerar clique longo

// ── Modo sono ────────────────────────────────────────────────────────
#define SLEEP_HOUR_START  23   // hora de entrar em modo sono
#define SLEEP_HOUR_END    9    // hora de acordar

// ── Modo dia (contraste com o modo sono) ──────────────────────────────
// Enquanto acordado, o ciclo é: NEUTRO (centrado, só piscando) por
// NEUTRAL_HOLD_DURATION → expressão aleatória por EXPR_HOLD_DURATION → NEUTRO
// de novo. A cada CURIOUS_INTERVAL de tempo nesse ciclo, uma pausa de
// CURIOUS_DURATION no modo CURIOSO (olhando em volta) antes de continuar.
#define NEUTRAL_HOLD_DURATION  20000  // ms — modo neutro (centrado, só piscando)
#define EXPR_HOLD_DURATION      5000  // ms — expressão aleatória entre os neutros
#define CURIOUS_INTERVAL       50000  // ms — a cada quanto tempo entra no modo curioso
#define CURIOUS_DURATION       20000  // ms — duração do modo curioso (olhando em volta)
#define DAY_EXPR_BEEP_FREQ        500  // Hz — aviso sonoro baixo ao trocar de expressão
#define DAY_EXPR_BEEP_DUR          40  // ms

#define SPECIAL_MODE_DURATION 10000
#define SERVO_PIN 17