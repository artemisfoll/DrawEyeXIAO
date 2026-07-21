# DrawEye — PlatformIO (XIAO ESP32-C6)

Olhos animados em display OLED SSD1306, com cabeça motorizada (servo), Wi-Fi,
painel de controle web (com menu lateral), relógio/clima com previsão de 3 dias,
modo foco (pomodoro) e contraste automático entre **modo dia** e **modo sono** —
tudo controlado por um botão físico e um touch capacitivo.

---

## Estrutura do projeto

```
DrawEye/
├── platformio.ini       ← configuração da placa e bibliotecas
├── include/
│   └── config.h         ← ✏️  EDITE AQUI para ajustar os valores padrão de fábrica
└── src/
    └── main.cpp         ← código principal
```

---

## Funcionalidades

- **Olhos animados**: piscadas automáticas, movimento aleatório do olhar e 12
  expressões — `neutro`, `feliz`, `triste`, `malvado`, `dormido`, `cansado`,
  `furia`, `rascal`, `surpreso`, `apaixonado`, `piscadinha`, `confuso` (+ `blink`,
  usado só pela piscada automática).
- **Cabeça motorizada**: um servo acompanha suavemente o olhar no modo neutro e se
  centraliza durante expressões e telas especiais.
- **Modo dia × modo sono**: fora do horário de sono, o "modo dia" fica ativo — a
  cada 30s os olhos fazem uma expressão automática (3 a 5s) com um aviso sonoro
  baixo, voltando ao neutro em seguida. No horário de sono, os olhos dormem
  sozinhos e só acordam depois do horário configurado.
- **Relógio, clima atual e previsão de 3 dias**: sincroniza hora via NTP e clima
  via Open-Meteo (cidade fixa em `config.h`) numa única requisição, com cache
  local para não precisar de Wi-Fi o tempo todo. O clima atual mostra também a
  mínima/máxima do dia.
- **Modo foco (pomodoro)**: configura um tempo (5 a 60 min) e conta regressivo na
  tela, com aviso sonoro ao final.
- **Status do dispositivo**: tela com rede/IP de acesso ao painel, uptime e
  memória livre.
- **Painel web** (app com menu lateral): aba **Emoções** (grade com ícone+texto
  para cada expressão) e aba **Ajustes** (servo, tamanho dos olhos, horário de
  sono, utilidades do dispositivo) — tudo ajustável ao vivo e persistido em NVS.
- **Portal captive de Wi-Fi**: configura a rede pelo celular/PC na primeira vez (ou
  quando a rede salva falhar), com tela de login antes do painel.

---

## Como ajustar manualmente

Abra **`include/config.h`** — cada constante tem comentário explicando o que faz.
Esses valores são o **padrão de fábrica**; servo, tamanho dos olhos e horário do
modo sono podem ser ajustados depois pelo painel web (aba **Ajustes**), que
sobrescreve e persiste os valores em NVS — sobrevivendo a reinícios.

| Constante | Efeito |
|-----------|--------|
| `EYE_SIZE_X / Y`, `EYE_THICKNESS` | tamanho e espessura **padrão** dos olhos — ajustável depois pelo painel web (aba Ajustes) |
| `EYE_MOVE_SPEED`, `EYE_MOVE_INTERVAL`, `EYE_RANGE_*` | suavidade e alcance do olhar |
| `BLINK_INTERVAL_MIN/MAX`, `BLINK_DURATION` | frequência e duração das piscadas |
| `NEUTRAL_DURATION_*`, `EXPR_DURATION_*` | quanto tempo fica neutro / em expressão (fora do modo dia) |
| `DAY_MODE_INTERVAL` | intervalo entre expressões automáticas no modo dia (padrão 30s) |
| `DAY_EXPR_DURATION_MIN/MAX` | duração da expressão automática no modo dia (padrão 3–5s) |
| `DAY_EXPR_BEEP_FREQ/DUR` | tom e duração do aviso sonoro baixo ao trocar de expressão no modo dia |
| `SERVO_PIN` | pino do servo (não ajustável pelo painel) |
| `SERVO_CENTER`, `SERVO_RANGE`, `SERVO_SMOOTH` | centro, amplitude e suavidade **padrão** de fábrica — ajustáveis depois pelo painel web, que sobrescreve e persiste em NVS |
| `SLEEP_HOUR_START` / `SLEEP_HOUR_END` | horário **padrão** de entrar/sair do modo sono — ajustável depois pelo painel web |
| `FOCUS_MIN_TIME` / `FOCUS_MAX_TIME` / `FOCUS_STEP` / `FOCUS_HOLD_MS` | limites do modo foco e tempo de clique longo |
| `WIFI_AP_NAME` / `WIFI_AP_PASSWORD` | nome/senha da rede do portal de configuração |
| `SPECIAL_MODE_DURATION` | quanto tempo cada tela especial (hora/clima/previsão/status) fica visível |
| `BUZZER_FREQ_*`, `BUZZER_DURATION` | tom e duração do beep dos botões |
| `LOC_LAT` / `LOC_LON` / `LOC_CITY` (em `main.cpp`) | localização usada para o clima |

---

## Pinos (XIAO ESP32-C6)

| Pino | Função |
|------|--------|
| `GPIO 00` | Botão físico (relógio / modo foco / status) |
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
| **Touch** (1 clique) | Dispara uma **expressão aleatória** |

A partir da tela de **hora**:

| Ação | Efeito |
|------|--------|
| **Touch** | Vai para a tela de **clima atual** (com mínima/máxima do dia) |
| **Botão** | Vai para a tela de **configuração do foco** |

Na tela de **clima atual**, touch avança para a **previsão dos próximos 3 dias**
(dia da semana, ícone e máx/mín de cada um); botão avança para o foco. Na
**previsão**, touch volta para a hora; botão avança para o foco.

Na tela de **configuração do foco** (mostra `FOCO` e os minutos selecionados):

| Ação | Efeito |
|------|--------|
| **Touch** | Soma 5 minutos (até 60, depois volta pra 5) |
| **Botão — clique curto** | Mostra a tela de **status do dispositivo** (rede/IP do painel, uptime, memória livre) |
| **Botão — clique longo** (≥ `FOCUS_HOLD_MS`, padrão 800ms) | **Inicia** a contagem do foco |

Ou seja, a sequência **botão, botão, botão** (curto) passa por: hora → configurar
foco → status do dispositivo. Qualquer toque na tela de status volta ao neutro.

Durante o **foco em andamento**, o botão cancela o timer a qualquer momento.

Todas as telas especiais (hora, clima, previsão, status) somem sozinhas depois de
`SPECIAL_MODE_DURATION` e voltam para os olhos animados.

### Modo dia × modo sono

Fora do horário de sono (`SLEEP_HOUR_START`/`SLEEP_HOUR_END`, ajustáveis também
pelo painel web), o **modo dia** fica ativo: a cada `DAY_MODE_INTERVAL` (padrão
30s) os olhos fazem uma expressão automática (`DAY_EXPR_DURATION_MIN/MAX`,
padrão 3–5s) com um aviso sonoro baixo, e voltam ao neutro em seguida.

Quando o horário sincronizado cai no intervalo de sono, os olhos entram
sozinhos no modo `dormido` e a cabeça fica centralizada. Nesse modo só o
**touch** funciona, mostrando a hora brevemente sem sair do sono. Ao passar do
horário de acordar, os olhos voltam ao neutro (modo dia) automaticamente.

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
**status**, acionada com 3 cliques no botão físico). Sem Wi-Fi conectado, `/` e
`/panel` mostram a tela de login (scan de redes); uma vez conectado, `/panel`
abre direto o app.

O painel (`/panel`) é dividido em menu lateral:

- **Emoções** — grade com ícone e nome de cada expressão; clique para aplicar na hora.
- **Ajustes** — quatro cartões:
  - **Servo (cabeça)**: centro, amplitude e suavidade do movimento. **Testar**
    aplica na hora sem salvar; **Salvar** grava em NVS (persiste após reiniciar).
  - **Tamanho dos olhos**: raio horizontal, raio vertical e espessura do traço.
  - **Horário do modo sono**: hora de dormir e de acordar.
  - **Dispositivo**: status ao vivo (IP, modo dia/sono, uptime, memória livre) e
    os botões Trocar Wi-Fi / Reiniciar / Esquecer rede salva.

A barra superior do painel mostra IP, modo atual (dia/sono) e uptime, atualizando
sozinha a cada poucos segundos.

### Rotas HTTP

| Rota | Descrição |
|------|-----------|
| `/` | Página de configuração de Wi-Fi (scan de redes, SSID/senha) |
| `/panel` | Painel de controle (Emoções + Ajustes) |
| `/eye?name=feliz` | Força uma expressão via URL |
| `/status` | JSON: `{eye, wifi, uptime, sleep, day, heap}` |
| `/scan` | JSON com redes Wi-Fi visíveis (usado pela página `/`) |
| `/connect?ssid=...&pass=...` | Conecta a uma rede e salva as credenciais |
| `/forget` | Apaga a rede salva (o portal reabre no próximo boot) |
| `/restart` | Reinicia o dispositivo remotamente |
| `/servo` | Sem parâmetros: consulta `{center, range, smooth}` atuais |
| `/servo?center=90&range=30&smooth=0.09` | Aplica na hora (teste), sem salvar |
| `/servo?...&save=1` | Aplica e **salva** em NVS |
| `/eyesize` | Sem parâmetros: consulta `{sizex, sizey, thick}` atuais |
| `/eyesize?sizex=14&sizey=15&thick=6` | Aplica na hora, sem salvar |
| `/eyesize?...&save=1` | Aplica e **salva** em NVS |
| `/sleep` | Sem parâmetros: consulta `{start, end}` atuais |
| `/sleep?start=23&end=9` | Aplica na hora, sem salvar |
| `/sleep?...&save=1` | Aplica e **salva** em NVS |

Expressões aceitas em `/eye`: `neutro`, `feliz`, `triste`, `malvado`, `dormido`,
`cansado`, `furia`, `rascal`, `surpreso`, `apaixonado`, `piscadinha`, `confuso`,
`blink`, `random`.

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
Wi-Fi/AP (`[WiFi] ...`), sincronização de hora/clima e previsão (`[NTP] ...` /
`[Weather] ...`) e transições de modo sono (`[Sleep] ...`).

> `platformio.ini` usa a tabela de partição `huge_app.csv` (app ~3MB, sem OTA)
> em vez do esquema padrão do ESP32 (~1.3MB com OTA dupla) — o projeto não usa
> OTA nem filesystem, então essa troca dá bastante margem para o painel web.

---

## Bibliotecas usadas

- **U8g2** — driver do display OLED
- **Pushbutton** (Pololu) — leitura de botões com debounce
- **ESP32Servo** — controle do servo da cabeça
- **ArduinoJson** — parse da resposta de clima/previsão (Open-Meteo)
- **WiFi / WebServer / DNSServer / Preferences / WiFiClientSecure / HTTPClient** —
  bibliotecas nativas do framework Arduino-ESP32 (Wi-Fi, servidor HTTP, portal
  captive, persistência de credenciais e requisições HTTPS)
