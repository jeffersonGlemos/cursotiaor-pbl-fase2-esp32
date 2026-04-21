/*
 * FarmTech Solutions — Cap-3, Tarefa 1 (Fase 1, Capítulo 3)
 * Versão Wokwi.com online — mesmo conteúdo do src/main.cpp.
 *
 * Libraries Manager: DHT sensor library, Adafruit Unified Sensor
 */

#include <DHT.h>

// ── Pinagem ─────────────────────────────────────────────────────────
const int PIN_POT_NPK = 34;
const int PIN_LDR     = 35;
const int PIN_DHT     = 18;
const int PIN_RELE    = 23;
const int PIN_BTN_SEL = 15;
const int PIN_LED     = 19;
const int PIN_BUZZER  = 33;

const int SEG_PINS[8] = { 25, 26, 27, 13, 14, 16, 17, 5 };
const int COM_VAL[3]  = { 21, 22, 4 };
const int COM_SEL     = 32;
const int COM_PH[3]   = { 2, 0, 12 };

const uint8_t DIGIT_ON[10] = {
  0b11111100, 0b01100000, 0b11011010, 0b11110010, 0b01100110,
  0b10110110, 0b10111110, 0b11100000, 0b11111110, 0b11110110
};
const uint8_t DP_BIT = 0b00000001;

// ── Faixas ideais para SOJA ─────────────────────────────────────────
const float PH_MIN         = 5.5f;
const float PH_MAX         = 6.8f;
const float UMID_CRITICA   = 40.0f;
const float UMID_ALVO      = 60.0f;
const float UMID_SATURACAO = 70.0f;
const int   NIVEL_N_MAX    = 30;
const int   NIVEL_PK_MIN   = 30;

const unsigned long INTERVALO_LOG_MS = 2000;
const unsigned int  MUX_DIGITO_US    = 1500;
const unsigned int  BUZZER_FREQ_HZ   = 1800;
const unsigned int  BUZZER_MS        = 150;

// ── Estado global ───────────────────────────────────────────────────
int   nutrientes[3]  = { 20, 45, 50 };
int   idxSelecionado = 0;
int   potBaseline    = -1;
float pHAtual        = 7.0f;
bool  bombaAnterior  = false;
const char NOMES[3]  = { 'N', 'P', 'K' };

DHT dht(PIN_DHT, DHT22);

// ── Displays ────────────────────────────────────────────────────────
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
    { (uint8_t)(DIGIT_ON[(phInt / 10) % 10] | DP_BIT), COM_PH[1] },
    { DIGIT_ON[ phInt        % 10],                  COM_PH[2]  },
  };

  for (int i = 0; i < 7; i++) {
    desligarCommons();
    escreverSegmentos(slots[i].pat);
    digitalWrite(slots[i].comPin, HIGH);
    delayMicroseconds(MUX_DIGITO_US);
  }
  desligarCommons();
}

// ── Botão ───────────────────────────────────────────────────────────
void tratarBotaoSelecionador() {
  static bool ultimoEstado = HIGH;
  static unsigned long ultimaBorda = 0;
  const bool estado = digitalRead(PIN_BTN_SEL);
  if (estado != ultimoEstado && (millis() - ultimaBorda) > 30) {
    ultimaBorda  = millis();
    ultimoEstado = estado;
    if (estado == LOW) {
      idxSelecionado = (idxSelecionado + 1) % 3;
      potBaseline = -1;
      Serial.printf("▶ Editando matriz[%d] = %c (valor atual %d)\n",
                    idxSelecionado, NOMES[idxSelecionado],
                    nutrientes[idxSelecionado]);
    }
  }
}

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

float lerPhBase() {
  return analogRead(PIN_LDR) * 14.0f / 4095.0f;
}

float calcularPH(float phBase, int n, int p, int k) {
  const float ajuste = -(n * 0.04f + p * 0.01f + k * 0.005f);
  return constrain(phBase + ajuste, 0.0f, 14.0f);
}

bool decidirBomba(int n, int p, int k, float ph, float umidade) {
  if (isnan(umidade))              return false;
  if (umidade < UMID_CRITICA)      return true;
  if (umidade >= UMID_SATURACAO)   return false;

  const bool ph_ok    = (ph >= PH_MIN && ph <= PH_MAX);
  const bool n_excess = (n > NIVEL_N_MAX);
  const bool pk_ok    = (p >= NIVEL_PK_MIN && k >= NIVEL_PK_MIN);

  if (!ph_ok)    return false;
  if (n_excess)  return false;
  if (!pk_ok)    return false;
  return umidade < UMID_ALVO;
}

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
  if (ph < PH_MIN)                                  Serial.println(F("  ⚠ pH baixo"));
  if (ph > PH_MAX)                                  Serial.println(F("  ⚠ pH alto"));
  if (n > NIVEL_N_MAX)                              Serial.println(F("  ⚠ N em excesso"));
  if (p < NIVEL_PK_MIN)                             Serial.println(F("  ⚠ falta P"));
  if (k < NIVEL_PK_MIN)                             Serial.println(F("  ⚠ falta K"));
}

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
  atualizarDisplays();
  tratarBotaoSelecionador();
  amostrarPotenciometro();
  pHAtual = calcularPH(lerPhBase(),
                       nutrientes[0], nutrientes[1], nutrientes[2]);

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
