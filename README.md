# DrawEye — PlatformIO (XIAO ESP32-C6)

Olhos animados em display OLED SSD1306, com cabeça motorizada (servo), Wi-Fi,
painel de controle web, relógio/clima, modo foco (pomodoro) e modo sono automático —
tudo controlado por um botão físico e um touch capacitivo.

---

## Estrutura do projeto

```
DrawEye/
├── platformio.ini       ← configuração da placa e bibliotecas
├── include/
│   └── config.h         ← ✏️  EDITE AQUI para ajustar tudo manualmente
└── src/
    └── main.cpp         ← código principal
```

---

## Funcionalidades

- **Olhos animados**: piscadas automáticas, movimento aleatório do olhar e expressões
  (`neutro`, `feliz`, `triste`, `malvado`, `dormido`, `cansado`, `furia`, `rascal`, `blink`).
- **Cabeça motorizada**: um servo acompanha suavemente o olhar no modo neutro e se
  centraliza durante expressões e telas especiais.
- **Relógio e clima**: sincroniza hora via NTP e clima via Open-Meteo (cidade fixa em
  `config.h`), com cache local para não precisar de Wi-Fi o tempo todo.
- **Modo foco (pomodoro)**: configura um tempo (5 a 60 min) e conta regressivo na tela,
  com aviso sonoro ao final.
- **Modo sono automático**: baseado no horário sincronizado por NTP, os olhos dormem
  sozinhos no horário configurado e "acordam" depois.
- **Painel web**: controla as expressões remotamente, ajusta o centro/amplitude do
  servo, reinicia o dispositivo ou esquece a rede salva.
- **Portal captive de Wi-Fi**: configura a rede pelo celular/PC na primeira vez (ou
  quando a rede salva falhar).

---

## Como ajustar manualmente

Abra **`include/config.h`** — cada constante tem comentário explicando o que faz.
Principais grupos de ajuste:

| Constante | Efeito |
|-----------|--------|
| `EYE_SIZE_X / Y`, `EYE_THICKNESS` | tamanho e espessura dos olhos |
| `EYE_MOVE_SPEED`, `EYE_MOVE_INTERVAL`, `EYE_RANGE_*` | suavidade e alcance do olhar |
| `BLINK_INTERVAL_MIN/MAX`, `BLINK_DURATION` | frequência e duração das piscadas |
| `NEUTRAL_DURATION_*`, `EXPR_DURATION_*` | quanto tempo fica neutro / em expressão |
| `SERVO_PIN`, `SERVO_SMOOTH` | pino e suavidade do movimento da cabeça |
| `SERVO_CENTER`, `SERVO_RANGE` | centro e amplitude **padrão** de fábrica — ajustáveis depois pelo painel web (veja abaixo), que sobrescreve e persiste em NVS |
| `SLEEP_HOUR_START` / `SLEEP_HOUR_END` | horário em que entra/sai do modo sono |
| `FOCUS_MIN_TIME` / `FOCUS_MAX_TIME` / `FOCUS_STEP` / `FOCUS_HOLD_MS` | limites do modo foco e tempo de clique longo |
| `WIFI_AP_NAME` / `WIFI_AP_PASSWORD` | nome/senha da rede do portal de configuração |
| `SPECIAL_MODE_DURATION` | quanto tempo cada tela especial (hora/clima/info web) fica visível |
| `BUZZER_FREQ_*`, `BUZZER_DURATION` | tom e duração do beep dos botões |
| `LOC_LAT` / `LOC_LON` / `LOC_CITY` (em `main.cpp`) | localização usada para o clima |

---

## Pinos (XIAO ESP32-C6)

| Pino | Função |
|------|--------|
| `GPIO 00` | Botão físico (relógio / modo foco / info web) |
| `GPIO 01` | Touch / botão capacitivo (expressão aleatória / navegação) |
| `GPIO 02` | Buzzer piezo |
| `GPIO 17` | Servo da cabeça |
| `D4 / D5` | SDA/SCL do OLED (padrão I²C do XIAO) |

---

## Uso: botão físico e touch

O comportamento muda de acordo com o "modo" atual da tela. Ponto de partida —
**modo normal** (olhos animados, sem tela especial):

| Ação | Efeito |
|------|--------|
| **Botão** (1 clique) | Mostra a **hora** (sincroniza NTP/clima em segundo plano se o cache expirou) |
| **Touch** (1 clique) | Dispara uma **expressão aleatória** (feliz, fúria, malvado, blink ou rascal) |

A partir da tela de **hora**:

| Ação | Efeito |
|------|--------|
| **Touch** | Vai para a tela de **clima** |
| **Botão** | Vai para a tela de **configuração do foco** |

Na tela de **clima**, touch volta para a hora e botão avança para o foco (mesmo padrão).

Na tela de **configuração do foco** (mostra `FOCO` e os minutos selecionados):

| Ação | Efeito |
|------|--------|
| **Touch** | Soma 5 minutos (até 60, depois volta pra 5) |
| **Botão — clique curto** | Mostra a tela de **como acessar o modo web** (SSID/IP) |
| **Botão — clique longo** (≥ `FOCUS_HOLD_MS`, padrão 800ms) | **Inicia** a contagem do foco |

Ou seja, a sequência **botão, botão, botão** (curto) passa por: hora → configurar foco
→ informações de acesso ao modo web. Qualquer toque na tela de info web volta ao neutro.

Durante o **foco em andamento**, o botão cancela o timer a qualquer momento.

Todas as telas especiais (hora, clima, info web) somem sozinhas depois de
`SPECIAL_MODE_DURATION` e voltam para os olhos animados.

### Modo sono

Quando o horário sincronizado cai entre `SLEEP_HOUR_START` e `SLEEP_HOUR_END`
(em `config.h`), os olhos entram sozinhos no modo `dormido` e a cabeça fica
centralizada. Nesse modo só o **touch** funciona, mostrando a hora brevemente
sem sair do sono. Ao passar do horário de acordar, os olhos voltam ao neutro
automaticamente.

---

## Configuração do Wi-Fi (portal captive)

1. Na primeira vez (ou se a rede salva falhar ao reconectar), o ESP abre uma rede
   com o nome definido em `WIFI_AP_NAME` (`config.h`, atualmente `"DrawEye"`).
2. Conecte no celular/PC e acesse **192.168.4.1**. A tela do dispositivo mostra o
   SSID e o IP até a configuração ser concluída.
3. Escolha sua rede Wi-Fi (a página faz um scan automático) e salve a senha.
4. O ESP conecta, salva as credenciais na memória (NVS) e passa a expor o
   **painel web** direto pelo IP da sua rede local — sem precisar do AP de novo.

---

## Interface Web

Com o ESP conectado à sua rede, acesse o IP mostrado no display (ou pela tela de
"info web", acionada com 3 cliques no botão físico):

| Rota | Descrição |
|------|-----------|
| `/` | Página de configuração de Wi-Fi (scan de redes, SSID/senha) |
| `/panel` | Painel de controle: expressões, servo, reiniciar e esquecer rede |
| `/eye?name=feliz` | Força uma expressão via URL |
| `/status` | JSON com estado atual (`eye`, `wifi`, `uptime`) |
| `/scan` | JSON com redes Wi-Fi visíveis (usado pela página `/`) |
| `/connect?ssid=...&pass=...` | Conecta a uma rede e salva as credenciais |
| `/forget` | Apaga a rede salva (o portal reabre no próximo boot) |
| `/restart` | Reinicia o dispositivo remotamente |
| `/servo` | Sem parâmetros: consulta `{center, range}` atuais |
| `/servo?center=90&range=30` | Aplica na hora (teste), sem salvar |
| `/servo?center=90&range=30&save=1` | Aplica e **salva** em NVS (persiste após reiniciar) |

Expressões aceitas em `/eye`: `neutro`, `feliz`, `triste`, `malvado`, `dormido`,
`cansado`, `furia`, `rascal`, `blink`, `random`.

No `/panel` também tem:
- **Ajuste do servo**: dois sliders — **Centro** (0–180°, posição de repouso da
  cabeça) e **Amplitude** (0–90°, quanto ela gira para cada lado a partir do
  centro). Mostra ao vivo o intervalo resultante (`min°` a `max°`, sempre
  travado dentro de 0–180°). Botão **Testar** move a cabeça na hora sem salvar;
  **Salvar** grava os valores em NVS para valerem após reiniciar.
- Botões **Trocar Wi-Fi**, **Reiniciar DrawEye** e **Esquecer rede salva** (os
  dois últimos pedem confirmação antes de agir).

> A conexão Wi-Fi fica ativa continuamente enquanto o painel web estiver
> disponível — sincronizar hora/clima (pelo botão físico) não derruba o Wi-Fi
> nem tira o painel do ar.

---

## Upload

```bash
pio run --target upload
pio device monitor          # serial 115200
```

O monitor serial mostra logs úteis de diagnóstico, como o resultado da conexão
Wi-Fi/AP (`[WiFi] ...`), sincronização de hora/clima (`[NTP] ...` / `[Weather] ...`)
e transições de modo sono (`[Sleep] ...`).

---

## Bibliotecas usadas

- **U8g2** — driver do display OLED
- **Pushbutton** (Pololu) — leitura de botões com debounce
- **ESP32Servo** — controle do servo da cabeça
- **ArduinoJson** — parse da resposta de clima (Open-Meteo)
- **WiFi / WebServer / DNSServer / Preferences / WiFiClientSecure / HTTPClient** —
  bibliotecas nativas do framework Arduino-ESP32 (Wi-Fi, servidor HTTP, portal
  captive, persistência de credenciais e requisições HTTPS)
