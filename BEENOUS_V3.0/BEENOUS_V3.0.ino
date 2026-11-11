
#include <Arduino.h>
#include <math.h>
#include <stdint.h>

#include "esp_adc/adc_continuous.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

// ==================================================
// BEENOUS V3.0 - CONFIGURAÇÃO
// ==================================================

#define ADC_PIN          34
#define ADC_CHANNEL      ADC_CHANNEL_6

#define SAMPLE_RATE      20000
#define WINDOW_SAMPLES   4000

#define DMA_FRAME_SIZE   256
#define DMA_BUFFER_SIZE  4096

#define MIN_FREQ_HZ      40.0f
#define MAX_FREQ_HZ      70.0f

#define HYSTERESIS       15.0f

// ==================================================
// ESTRUTURA DE MEDIÇÃO
// ==================================================

struct MedicaoAC {
  float offsetDC;
  float rmsAC;
  float frequenciaHz;

  uint16_t minimo;
  uint16_t maximo;
  uint16_t picoPico;
  uint16_t saturacoes;
  uint16_t ciclosDetectados;

  bool frequenciaValida;
};

// ==================================================
// VARIÁVEIS GLOBAIS
// ==================================================

adc_continuous_handle_t adcHandle = NULL;

alignas(4) uint8_t dmaBuffer[512];

uint16_t samples[WINDOW_SAMPLES];

uint32_t sampleCount = 0;
uint32_t totalSamples = 0;

// ==================================================
// TRATAMENTO DE ERROS
// ==================================================

void stopWithError(const char *message, esp_err_t error) {

  Serial.printf(
    "\nERRO: %s | Código: 0x%x\n",
    message,
    (unsigned int)error
  );

  if (adcHandle != NULL) {
    adc_continuous_stop(adcHandle);
  }

  while (true) {
    delay(1000);
  }
}

// ==================================================
// INICIALIZAÇÃO ADC CONTINUOUS + DMA
// ==================================================

void initADC_DMA() {

  adc_continuous_handle_cfg_t handleConfig = {};

  handleConfig.max_store_buf_size = DMA_BUFFER_SIZE;
  handleConfig.conv_frame_size = DMA_FRAME_SIZE;

  esp_err_t error = adc_continuous_new_handle(
    &handleConfig,
    &adcHandle
  );

  if (error != ESP_OK) {
    stopWithError("Falha ao criar ADC DMA", error);
  }

  adc_digi_pattern_config_t adcPattern = {};

  adcPattern.atten = ADC_ATTEN_DB_12;
  adcPattern.channel = ADC_CHANNEL;
  adcPattern.unit = ADC_UNIT_1;
  adcPattern.bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t adcConfig = {};

  adcConfig.pattern_num = 1;
  adcConfig.adc_pattern = &adcPattern;
  adcConfig.sample_freq_hz = SAMPLE_RATE;
  adcConfig.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  adcConfig.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;

  error = adc_continuous_config(
    adcHandle,
    &adcConfig
  );

  if (error != ESP_OK) {
    stopWithError("Falha na configuração ADC", error);
  }

  error = adc_continuous_start(adcHandle);

  if (error != ESP_OK) {
    stopWithError("Falha ao iniciar ADC DMA", error);
  }

  Serial.println("ADC Continuous + DMA iniciado.");
}

// ==================================================
// PROCESSAMENTO DA JANELA
// ==================================================

MedicaoAC processarJanela(
  const uint16_t *samples,
  float sampleRateHz
) {

  MedicaoAC resultado = {};

  uint64_t soma = 0;

  uint16_t minimo = 4095;
  uint16_t maximo = 0;
  uint16_t saturacoes = 0;

  // 1. ESTATÍSTICAS BÁSICAS

  for (int i = 0; i < WINDOW_SAMPLES; i++) {

    uint16_t valor = samples[i];

    soma += valor;

    if (valor < minimo) minimo = valor;
    if (valor > maximo) maximo = valor;

    if (valor == 0 || valor >= 4095) {
      saturacoes++;
    }
  }

  float media = (float)soma / WINDOW_SAMPLES;

  // 2. CÁLCULO RMS AC

  double somaQuadrados = 0.0;

  for (int i = 0; i < WINDOW_SAMPLES; i++) {

    float componenteAC = samples[i] - media;

    somaQuadrados +=
      (double)componenteAC * componenteAC;
  }

  float rms = sqrt(
    somaQuadrados / WINDOW_SAMPLES
  );

  // 3. ESTIMATIVA DA FREQUÊNCIA

  bool armado = false;
  bool primeiroCruzamento = true;

  float ultimoCruzamento = 0.0f;

  double somaPeriodos = 0.0;

  uint16_t periodosValidos = 0;

  for (int i = 1; i < WINDOW_SAMPLES; i++) {

    float anterior = samples[i - 1];
    float atual = samples[i];

    if (atual < media - HYSTERESIS) {
      armado = true;
    }

    if (
      armado &&
      anterior < media &&
      atual >= media
    ) {

      float denominador = atual - anterior;

      if (denominador > 0) {

        float fracao =
          (media - anterior) / denominador;

        float cruzamento =
          (i - 1) + fracao;

        if (!primeiroCruzamento) {

          float periodo =
            cruzamento - ultimoCruzamento;

          if (periodo > 0 && sampleRateHz > 0) {

            float frequenciaInstantanea =
              sampleRateHz / periodo;

            if (
              frequenciaInstantanea >= MIN_FREQ_HZ &&
              frequenciaInstantanea <= MAX_FREQ_HZ
            ) {

              somaPeriodos += periodo;
              periodosValidos++;
            }
          }

        } else {

          primeiroCruzamento = false;
        }

        ultimoCruzamento = cruzamento;
        armado = false;
      }
    }
  }

  // 4. FREQUÊNCIA MÉDIA

  float frequencia = 0.0f;
  bool frequenciaValida = false;

  if (periodosValidos >= 2 && sampleRateHz > 0) {

    float periodoMedio =
      somaPeriodos / periodosValidos;

    frequencia = sampleRateHz / periodoMedio;

    frequenciaValida = true;
  }

  // 5. ORGANIZAÇÃO DOS RESULTADOS

  resultado.offsetDC = media;
  resultado.rmsAC = rms;
  resultado.frequenciaHz = frequencia;

  resultado.minimo = minimo;
  resultado.maximo = maximo;
  resultado.picoPico = maximo - minimo;

  resultado.saturacoes = saturacoes;
  resultado.ciclosDetectados = periodosValidos;

  resultado.frequenciaValida = frequenciaValida;

  return resultado;
}

// ==================================================
// APRESENTAÇÃO DOS RESULTADOS
// ==================================================

void apresentarResultados(const MedicaoAC &m) {

  Serial.println();
  Serial.println("========== BEENOUS V3.0 ==========");

  Serial.printf("Amostras: %u\n", WINDOW_SAMPLES);

  Serial.printf("Mínimo ADC: %u\n", m.minimo);
  Serial.printf("Máximo ADC: %u\n", m.maximo);

  Serial.printf("Offset DC: %.2f\n", m.offsetDC);
  Serial.printf("RMS AC: %.2f\n", m.rmsAC);

  Serial.printf("Pico a pico: %u\n", m.picoPico);

  Serial.printf("Saturações: %u\n", m.saturacoes);

  Serial.printf(
    "Taxa configurada: %d amostras/s\n",
    SAMPLE_RATE
  );

  if (m.frequenciaValida) {

    Serial.printf(
      "Frequência estimada: %.2f Hz\n",
      m.frequenciaHz
    );

    Serial.printf(
      "Períodos válidos: %u\n",
      m.ciclosDetectados
    );

  } else {

    Serial.println("Frequência: não validada");
  }

  if (m.saturacoes > 0) {
    Serial.println("Estado: SATURAÇÃO DETECTADA");
  } else {
    Serial.println("Estado: SEM SATURAÇÃO");
  }

  Serial.printf(
    "Total acumulado: %lu\n",
    (unsigned long)totalSamples
  );

  Serial.println("==================================");
}

// ==================================================
// RECEPÇÃO DE AMOSTRAS
// ==================================================

void processSample(uint16_t value) {

  if (sampleCount < WINDOW_SAMPLES) {

    samples[sampleCount] = value;

    sampleCount++;
    totalSamples++;
  }

  if (sampleCount >= WINDOW_SAMPLES) {

    MedicaoAC medicao = processarJanela(
      samples,
      SAMPLE_RATE
    );

    apresentarResultados(medicao);

    // Preparar próxima janela

    sampleCount = 0;
  }
}

// ==================================================
// SETUP
// ==================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("BEENOUS V3.0");
  Serial.println("Sistema de medição AC");
  Serial.println("------------------------------");

  initADC_DMA();
}

// ==================================================
// LOOP
// ==================================================

void loop() {

  uint32_t bytesRead = 0;

  esp_err_t error = adc_continuous_read(
    adcHandle,
    dmaBuffer,
    sizeof(dmaBuffer),
    &bytesRead,
    1000
  );

  if (error == ESP_ERR_TIMEOUT) {
    return;
  }

  if (error != ESP_OK) {
    stopWithError("Erro na leitura ADC DMA", error);
  }

  if (
    bytesRead % SOC_ADC_DIGI_RESULT_BYTES != 0
  ) {

    Serial.println("Bloco ADC com tamanho inválido.");
    return;
  }

  for (
    uint32_t i = 0;
    i < bytesRead;
    i += SOC_ADC_DIGI_RESULT_BYTES
  ) {

    adc_digi_output_data_t *data =
      (adc_digi_output_data_t *)&dmaBuffer[i];

    uint16_t value = data->type1.data;
    uint8_t channel = data->type1.channel;

    if (channel == ADC_CHANNEL) {
      processSample(value);
    }
  }
}

