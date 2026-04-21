/*
 * FarmTech Solutions — Cap-3, Tarefa 1 (Fase 1, Capítulo 3)
 * Sistema de irrigação inteligente para cultivo de SOJA.
 *
 * Hardware (ver diagram.json):
 *   - 1 potenciômetro slide   → valor 0–99 do nutriente selecionado
 *   - 1 botão "SEL"           → cicla N → P → K
 *   - Display 7-seg 3 dígitos → mostra matriz[selecionado]  (ex: "045")
 *   - Display 7-seg 1 dígito  → mostra índice 1/2/3 (N/P/K)
 *   - Display 7-seg 3 dígitos → mostra pH lido pelo LDR (ex: "06.8")
 *   - LDR                     → simula pH 0.0–14.0
 *   - DHT22                   → umidade do solo
 *   - Relé azul               → bomba d'água
 *   - LED vermelho            → espelha o estado do relé (bomba)
 *   - Buzzer                  → beep curto na borda off→on da bomba
 *
 * Os três displays são common-anode e compartilham os 8 pinos de segmentos.
 * Só os commons são independentes — multiplexing manual cicla pelos 7
 * dígitos lógicos (3 valor + 1 seletor + 3 pH) a ~95 Hz por dígito.
 *
 * Matriz NPK:
 *   nutrientes[0] = N   (nitrogênio)
 *   nutrientes[1] = P   (fósforo)
 *   nutrientes[2] = K   (potássio)
 *
 * Lógica para SOJA:
 *   - pH ideal 5.5–6.8
 *   - Umidade alvo 60 % (crítica <40 %, saturação ≥70 %)
 *   - N > 30 → excesso inibe fixação biológica → não regar
 *   - P ≥ 30 e K ≥ 30 → nutrição adequada
 *
 * Observação didática: ao mexer nos nutrientes no potenciômetro slide,
 * mova também o slider do LDR — fisicamente o pH do solo muda com a
 * aplicação de adubos (ureia acidifica, calcário alcaliniza).
 */

#include <Arduino.h>
#include <DHT.h>

// ── Pinagem ─────────────────────────────────────────────────────────
const int PIN_POT_NPK = 34;   // potenciômetro slide (valor do nutriente)
const int PIN_LDR     = 35;   // LDR → pH
const int PIN_DHT     = 18;
const int PIN_RELE    = 23;
const int PIN_BTN_SEL = 15;
const int PIN_LED     = 19;   // espelha o relé
const int PIN_BUZZER  = 33;   // beep curto na ativação

// Displays (common-anode) — segmentos compartilhados, commons independentes
const int SEG_PINS[8] = { 25, 26, 27, 13, 14, 16, 17, 5 };  // A B C D E F G DP
const int COM_VAL[3]  = { 21, 22, 4 };   // DIG1, DIG2, DIG3 do display de valor
const int COM_SEL     = 32;              // COM do display do seletor
const int COM_PH[3]   = { 2, 0, 12 };    // DIG1, DIG2, DIG3 do display de pH

// Padrões 7-seg — bit = 1 significa segmento ACESO
// Ordem dos bits MSB→LSB: A B C D E F G DP
const uint8_t DIGIT_ON[10] = {
  0b11111100,  // 0
  0b01100000,  // 1
  0b11011010,  // 2
  0b11110010,  // 3
  0b01100110,  // 4
  0b10110110,  // 5
  0b10111110,  // 6
  0b11100000,  // 7
  0b11111110,  // 8
  0b11110110   // 9
};
const uint8_t DP_BIT = 0b00000001;  // ponto decimal

// ── Faixas ideais para SOJA ─────────────────────────────────────────
const float PH_MIN         = 5.5f;
const float PH_MAX         = 6.8f;
const float UMID_CRITICA   = 40.0f;
const float UMID_ALVO      = 60.0f;
const float UMID_SATURACAO = 70.0f;
const int   NIVEL_N_MAX    = 30;
const int   NIVEL_PK_MIN   = 30;

const unsigned long INTERVALO_LOG_MS = 2000;
const unsigned int  MUX_DIGITO_US    = 1500;  // 1.5 ms × 7 dígitos → ~95 Hz
const unsigned int  BUZZER_FREQ_HZ   = 1800;
const unsigned int  BUZZER_MS        = 150;

// ── Estado global ───────────────────────────────────────────────────
int   nutrientes[3]  = { 20, 45, 50 };  // N, P, K — solo arável típico
int   idxSelecionado = 0;               // 0=N, 1=P, 2=K
int   potBaseline    = -1;              // -1 = ainda não registrado
float pHAtual        = 7.0f;            // última leitura do LDR
bool  bombaAnterior  = false;           // detecção de borda off→on

const char NOMES[3] = { 'N', 'P', 'K' };

DHT dht(PIN_DHT, DHT22);

// ── Displays (multiplexing manual, common-anode) ───────────────────
// Common-anode: common em HIGH + segmento em LOW → segmento aceso.
void escreverSegmentos(uint8_t pattern) {
  for (int s = 0; s < 8; s++) {
    const bool aceso = (pattern >> (7 - s)) & 1;
    digitalWrite(SEG_PINS[s], aceso ? LOW : HIGH);
  }
}

void desligarCommons() {
  for (int c = 0; c < 3; c++) digitalWrite(COM_VAL[c], LOW);
  digitalWrite(COM_SEL, LOW);
  for (int c = 0; c < 3; c++) digitalWrite(COM_PH[c],  LOW);
}

// Cicla pelos 7 dígitos lógicos — 3 do valor, 1 do seletor, 3 do pH.
// pH é mostrado como "XX.X" com ponto decimal no dígito do meio.
void atualizarDisplays() {
  const int valor = nutrientes[idxSelecionado];
  const int phInt = constrain((int)(pHAtual * 10.0f + 0.5f), 0, 199);

  struct Slot { uint8_t pat; int comPin; };
  const Slot slots[7] = {
    { DIGIT_ON[(valor / 100) % 10],                  COM_VAL[0] },
    { DIGIT_ON[(valor / 10)  % 10],                  COM_VAL[1] },
    { DIGIT_ON[ valor        % 10],                  COM_VAL[2] },
    { DIGIT_ON[idxSelecionado + 1],                  COM_SEL    },
    { DIGIT_ON[(phInt / 100) % 10],                  COM_PH[0]  },
    { (uint8_t)(DIGIT_ON[(phInt / 10) % 10] | DP_BIT), COM_PH[1] },  // "X."
    { DIGIT_ON[ phInt        % 10],                  COM_PH[2]  },
  };

  for (int i = 0; i < 7; i++) {
    desligarCommons();
    escreverSegmentos(slots[i].pat);
    digitalWrite(slots[i].comPin, HIGH);
    delayMicroseconds(MUX_DIGITO_US);
  }
  desligarCommons();  // evita "ghost" no próximo pattern
}

// ── Botão SEL — cicla N → P → K, com debounce ──────────────────────
void tratarBotaoSelecionador() {
  static bool ultimoEstado = HIGH;
  static unsigned long ultimaBorda = 0;
  const bool estado = digitalRead(PIN_BTN_SEL);
  if (estado != ultimoEstado && (millis() - ultimaBorda) > 30) {
    ultimaBorda  = millis();
    ultimoEstado = estado;
    if (estado == LOW) {
      idxSelecionado = (idxSelecionado + 1) % 3;
      potBaseline = -1;   // pot precisa "se achar" no novo nutriente
      Serial.printf("▶ Editando matriz[%d] = %c (valor atual %d)\n",
                    idxSelecionado, NOMES[idxSelecionado],
                    nutrientes[idxSelecionado]);
    }
  }
}

// ── Potenciômetro ──────────────────────────────────────────────────
// Preserva valores iniciais: só sobrescreve nutrientes[idxSelecionado]
// quando o slider for realmente movido (>~2 % da escala). Ao trocar de
// nutriente o baseline é resetado, para o valor salvo não "saltar" para
// a posição atual do pot no instante da troca.
void amostrarPotenciometro() {
  const int bruto = analogRead(PIN_POT_NPK);
  if (potBaseline < 0) {
    potBaseline = bruto;
    return;
  }
  if (abs(bruto - potBaseline) > 80) {
    nutrientes[idxSelecionado] = (bruto * 100) / 4096;
    potBaseline = bruto;
  }
}

// ── LDR → pH base + correlação NPK ─────────────────────────────────
// pH do solo é modulado pelos adubos aplicados:
//   - N (ureia) → nitrificação gera H⁺ → acidifica (-0.04 por unidade)
//   - P (superfosfato) → leve acidificação (-0.01 por unidade)
//   - K (KCl) → impacto mínimo (-0.005 por unidade)
// LDR simula o pH base do solo antes da adubação.
float lerPhBase() {
  return analogRead(PIN_LDR) * 14.0f / 4095.0f;
}

float calcularPH(float phBase, int n, int p, int k) {
  const float ajuste = -(n * 0.04f + p * 0.01f + k * 0.005f);
  return constrain(phBase + ajuste, 0.0f, 14.0f);
}

// ── Decisão de irrigação (cascata de 7 regras) ─────────────────────
bool decidirBomba(int n, int p, int k, float ph, float umidade) {
  if (isnan(umidade))              return false;  // 1) leitura inválida
  if (umidade < UMID_CRITICA)      return true;   // 2) emergência
  if (umidade >= UMID_SATURACAO)   return false;  // 3) solo saturado

  const bool ph_ok    = (ph >= PH_MIN && ph <= PH_MAX);
  const bool n_excess = (n > NIVEL_N_MAX);
  const bool pk_ok    = (p >= NIVEL_PK_MIN && k >= NIVEL_PK_MIN);

  if (!ph_ok)    return false;   // 4) pH fora da faixa
  if (n_excess)  return false;   // 5) N inibe fixação biológica
  if (!pk_ok)    return false;   // 6) P ou K deficitários
  return umidade < UMID_ALVO;    // 7) regar se abaixo do alvo
}

// ── Atuadores: relé + LED + buzzer ─────────────────────────────────
// LED espelha o relé. Buzzer bipa apenas na borda off→on para não ficar
// estridente enquanto a bomba permanece ligada.
void acionarAtuadores(bool bomba) {
  digitalWrite(PIN_RELE, bomba ? HIGH : LOW);
  digitalWrite(PIN_LED,  bomba ? HIGH : LOW);
  if (bomba && !bombaAnterior) {
    tone(PIN_BUZZER, BUZZER_FREQ_HZ, BUZZER_MS);
  }
  bombaAnterior = bomba;
}

void imprimirStatus(int n, int p, int k, float ph, float umidade, float temp, bool bomba) {
  Serial.println(F("─────────────────────────────────────────"));
  Serial.printf("Matriz NPK: [N=%d  P=%d  K=%d]  editando: %c\n",
    n, p, k, NOMES[idxSelecionado]);
  Serial.printf("pH: %5.2f  |  Umidade: %5.1f%%  |  Temp: %5.1f C\n",
    ph,
    isnan(umidade) ? -1.0f : umidade,
    isnan(temp)    ? -1.0f : temp);
  Serial.printf("→ BOMBA %s\n", bomba ? "LIGADA ▶" : "DESLIGADA ▪");

  if (isnan(umidade))                               Serial.println(F("  ⚠ DHT22 inválido"));
  if (!isnan(umidade) && umidade < UMID_CRITICA)    Serial.println(F("  ⚠ umidade crítica"));
  if (!isnan(umidade) && umidade >= UMID_SATURACAO) Serial.println(F("  ⚠ solo saturado"));
  if (ph < PH_MIN)                                  Serial.println(F("  ⚠ pH baixo — calagem"));
  if (ph > PH_MAX)                                  Serial.println(F("  ⚠ pH alto — corrigir"));
  if (n > NIVEL_N_MAX)                              Serial.println(F("  ⚠ N em excesso inibe fixação biológica"));
  if (p < NIVEL_PK_MIN)                             Serial.println(F("  ⚠ deficiência de P"));
  if (k < NIVEL_PK_MIN)                             Serial.println(F("  ⚠ deficiência de K"));
}

// ── Arduino ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_SEL, INPUT_PULLUP);
  pinMode(PIN_RELE,    OUTPUT);
  pinMode(PIN_LED,     OUTPUT);
  pinMode(PIN_BUZZER,  OUTPUT);
  digitalWrite(PIN_RELE, LOW);
  digitalWrite(PIN_LED,  LOW);

  for (int s = 0; s < 8; s++) pinMode(SEG_PINS[s], OUTPUT);
  for (int c = 0; c < 3; c++) pinMode(COM_VAL[c],  OUTPUT);
  for (int c = 0; c < 3; c++) pinMode(COM_PH[c],   OUTPUT);
  pinMode(COM_SEL, OUTPUT);
  desligarCommons();

  dht.begin();

  // beep duplo de inicialização — confirma que o buzzer e o LED estão ok
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED, HIGH);
    tone(PIN_BUZZER, 1800, 100);
    delay(200);
    digitalWrite(PIN_LED, LOW);
    delay(100);
  }

  Serial.println();
  Serial.println(F("═══════════════════════════════════════"));
  Serial.println(F("  FarmTech Solutions — Irrigação Soja  "));
  Serial.println(F("  Tarefa 1 · Fase 1 · Capítulo 3       "));
  Serial.println(F("═══════════════════════════════════════"));
  Serial.printf("Matriz NPK inicial: [N=%d  P=%d  K=%d]\n",
                nutrientes[0], nutrientes[1], nutrientes[2]);
  Serial.println(F("Slider NPK → valor  |  Botão SEL → próximo nutriente"));
  Serial.println(F("Slider LDR → pH base do solo (sem adubação)"));
  Serial.println(F("pH exibido = pH_base - (N×0.04 + P×0.01 + K×0.005)"));
}

void loop() {
  // Fast path — sem delays bloqueantes; multiplexing roda na velocidade do loop.
  atualizarDisplays();
  tratarBotaoSelecionador();
  amostrarPotenciometro();
  pHAtual = calcularPH(lerPhBase(),
                       nutrientes[0], nutrientes[1], nutrientes[2]);

  // Slow path — a cada 2 s: DHT, decisão, atuadores, log.
  static unsigned long ultimoLog = 0;
  if (millis() - ultimoLog >= INTERVALO_LOG_MS) {
    ultimoLog = millis();

    const float umidade = dht.readHumidity();
    const float temp    = dht.readTemperature();
    const bool  bomba   = decidirBomba(
      nutrientes[0], nutrientes[1], nutrientes[2], pHAtual, umidade);

    acionarAtuadores(bomba);
    imprimirStatus(nutrientes[0], nutrientes[1], nutrientes[2],
                   pHAtual, umidade, temp, bomba);
  }
}
