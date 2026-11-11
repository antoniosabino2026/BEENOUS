
#include <Arduino.h>
#include <math.h>
#include <stdint.h>

#include "esp_adc/adc_continuous.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

// ==================================================
// BEENOUS V3.3 - VARIAVEIS DE CALIBRACAO
// ==================================================

const float FATOR_V_POR_COUNT = 0.46232f;
const float FATOR_MINIMO = 0.000001f;

float tensaoRMS = 0.0f;
bool tensaoCalibrada = false;

// ==================================================
// BEENOUS V3.2 - CONFIGURACAO
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

#define MIN_PERIODS      5
#define MAX_PERIODS      32

#define MIN_SIGNAL_P2P  200
#define MAX_PERIOD_CV   2.5f

// ==================================================
// ESTRUTURA DE MEDICAO
// ==================================================

struct MedicaoAC {

  float offsetDC;
  float rmsAC;
  float frequenciaHz;

  float periodoMedio;
  float desvioPadraoPeriodo;
  float variacaoPeriodoPct;

  uint16_t minimo;
  uint16_t maximo;
  uint16_t picoPico;
  uint16_t saturacoes;

  uint16_t cruzamentosDetectados;
  uint16_t periodosValidos;
  uint16_t periodosRejeitados;

  bool amplitudeValida;
  bool frequenciaCalculada;
  bool frequenciaValida;
};

// ==================================================
// RESULTADO DO DETECTOR DE FREQUENCIA
// ==================================================

struct ResultadoFrequencia {

  float frequenciaHz;

  float periodoMedio;
  float desvioPadraoPeriodo;
  float variacaoPeriodoPct;

  uint16_t cruzamentosDetectados;
  uint16_t periodosValidos;
  uint16_t periodosRejeitados;

  bool amplitudeValida;
  bool frequenciaCalculada;
  bool frequenciaValida;
};

// ==================================================
// VARIAVEIS GLOBAIS
// ==================================================

adc_continuous_handle_t adcHandle = NULL;

alignas(4) uint8_t dmaBuffer[512];

uint16_t samples[WINDOW_SAMPLES];

uint32_t sampleCount = 0;
uint32_t totalSamples = 0;

// ==================================================
// PROTOTIPOS DAS FUNCOES
// ==================================================

void calcularTensaoRMS(float rmsADC);

void apresentarResultados(const MedicaoAC &m);

void processSample(uint16_t value);

void initADC_DMA();

// ==================================================
// TRATAMENTO DE ERROS
// ==================================================

void stopWithError(
  const char *message,
  esp_err_t error
) {

  Serial.printf(
    "\nERRO: %s | Codigo: 0x%x\n",
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
// INICIALIZACAO ADC CONTINUOUS + DMA
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
    stopWithError(
      "Falha ao criar ADC DMA",
      error
    );
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
    stopWithError(
      "Falha na configuracao ADC",
      error
    );
  }

  error = adc_continuous_start(adcHandle);

  if (error != ESP_OK) {
    stopWithError(
      "Falha ao iniciar ADC DMA",
      error
    );
  }

  Serial.println(
    "ADC Continuous + DMA iniciado."
  );
}

// ==================================================
// DETECTOR DE FREQUENCIA V3.2
// ==================================================

ResultadoFrequencia detectarFrequencia(
  const uint16_t *dados,
  uint32_t quantidade,
  double offsetDC,
  float sampleRateHz,
  uint16_t picoPico
) {

  ResultadoFrequencia resultado = {};

  if (
    dados == nullptr ||
    quantidade < 2 ||
    sampleRateHz <= 0
  ) {
    return resultado;
  }

  // 1. VALIDACAO DA AMPLITUDE

  resultado.amplitudeValida =
    picoPico >= MIN_SIGNAL_P2P;

  if (!resultado.amplitudeValida) {
    return resultado;
  }

  // 2. LIMITES DO PERIODO

  const double limiarInferior =
    offsetDC - HYSTERESIS;

  const double periodoMinimo =
    sampleRateHz / MAX_FREQ_HZ;

  const double periodoMaximo =
    sampleRateHz / MIN_FREQ_HZ;

  // 3. VARIAVEIS DO DETECTOR

  bool armado = false;
  bool primeiroCruzamento = true;

  double ultimoCruzamento = 0.0;

  double periodos[MAX_PERIODS];

  uint16_t quantidadePeriodos = 0;

  // 4. PERCORRER AS AMOSTRAS

  for (
    uint32_t i = 1;
    i < quantidade;
    i++
  ) {

    double anterior = dados[i - 1];
    double atual = dados[i];

    if (atual <= limiarInferior) {
      armado = true;
    }

    if (
      armado &&
      anterior < offsetDC &&
      atual >= offsetDC
    ) {

      double diferenca = atual - anterior;

      if (diferenca <= 0) {
        continue;
      }

      // INTERPOLACAO LINEAR

      double fraccao =
        (offsetDC - anterior) / diferenca;

      double cruzamento =
        (double)(i - 1) + fraccao;

      resultado.cruzamentosDetectados++;

      // CALCULAR PERIODO

      if (!primeiroCruzamento) {

        double periodo =
          cruzamento - ultimoCruzamento;

        if (
          periodo >= periodoMinimo &&
          periodo <= periodoMaximo
        ) {

          if (quantidadePeriodos < MAX_PERIODS) {

            periodos[quantidadePeriodos] = periodo;

            quantidadePeriodos++;

            resultado.periodosValidos++;

          } else {

            resultado.periodosRejeitados++;
          }

        } else {

          resultado.periodosRejeitados++;
        }

      } else {

        primeiroCruzamento = false;
      }

      ultimoCruzamento = cruzamento;

      armado = false;
    }
  }

  // 5. ESTATISTICAS DOS PERIODOS

  if (quantidadePeriodos == 0) {
    return resultado;
  }

  double somaPeriodos = 0.0;

  for (
    uint16_t i = 0;
    i < quantidadePeriodos;
    i++
  ) {

    somaPeriodos += periodos[i];
  }

  double mediaPeriodos =
    somaPeriodos / quantidadePeriodos;

  double somaDesviosQuadrados = 0.0;

  for (
    uint16_t i = 0;
    i < quantidadePeriodos;
    i++
  ) {

    double desvio =
      periodos[i] - mediaPeriodos;

    somaDesviosQuadrados +=
      desvio * desvio;
  }

  double variancia =
    somaDesviosQuadrados / quantidadePeriodos;

  double desvioPadrao =
    sqrt(variancia);

  double variacaoPercentual =
    (desvioPadrao / mediaPeriodos) * 100.0;

  // 6. CALCULAR FREQUENCIA

  resultado.periodoMedio = mediaPeriodos;

  resultado.desvioPadraoPeriodo = desvioPadrao;

  resultado.variacaoPeriodoPct = variacaoPercentual;

  resultado.frequenciaHz =
    sampleRateHz / mediaPeriodos;

  resultado.frequenciaCalculada = true;

  // 7. VALIDACAO FINAL

  resultado.frequenciaValida =
    resultado.amplitudeValida &&
    resultado.periodosValidos >= MIN_PERIODS &&
    resultado.variacaoPeriodoPct <= MAX_PERIOD_CV &&
    resultado.frequenciaHz >= MIN_FREQ_HZ &&
    resultado.frequenciaHz <= MAX_FREQ_HZ;

  return resultado;
}

// ==================================================
// CALCULO RMS E PROCESSAMENTO DA JANELA
// ==================================================

MedicaoAC processarJanela(
  const uint16_t *dados,
  float sampleRateHz
) {

  MedicaoAC resultado = {};

  uint64_t soma = 0;

  uint16_t minimo = 4095;
  uint16_t maximo = 0;
  uint16_t saturacoes = 0;

  // 1. ESTATISTICAS BASICAS

  for (
    uint32_t i = 0;
    i < WINDOW_SAMPLES;
    i++
  ) {

    uint16_t valor = dados[i];

    soma += valor;

    if (valor < minimo) minimo = valor;
    if (valor > maximo) maximo = valor;

    if (valor == 0 || valor >= 4095) {
      saturacoes++;
    }
  }

  float media =
    (float)soma / WINDOW_SAMPLES;

  // 2. CALCULO RMS AC

  double somaQuadrados = 0.0;

  for (
    uint32_t i = 0;
    i < WINDOW_SAMPLES;
    i++
  ) {

    double componenteAC =
      dados[i] - media;

    somaQuadrados +=
      componenteAC * componenteAC;
  }

  float rms = sqrt(
    somaQuadrados / WINDOW_SAMPLES
  );

  uint16_t picoPico = maximo - minimo;

  // 3. DETECCAO DE FREQUENCIA

  ResultadoFrequencia freq =
    detectarFrequencia(
      dados,
      WINDOW_SAMPLES,
      media,
      sampleRateHz,
      picoPico
    );

  // 4. ORGANIZAR RESULTADOS

  resultado.offsetDC = media;
  resultado.rmsAC = rms;

  resultado.frequenciaHz =
    freq.frequenciaHz;

  resultado.periodoMedio =
    freq.periodoMedio;

  resultado.desvioPadraoPeriodo =
    freq.desvioPadraoPeriodo;

  resultado.variacaoPeriodoPct =
    freq.variacaoPeriodoPct;

  resultado.minimo = minimo;
  resultado.maximo = maximo;
  resultado.picoPico = picoPico;

  resultado.saturacoes = saturacoes;

  resultado.cruzamentosDetectados =
    freq.cruzamentosDetectados;

  resultado.periodosValidos =
    freq.periodosValidos;

  resultado.periodosRejeitados =
    freq.periodosRejeitados;

  resultado.amplitudeValida =
    freq.amplitudeValida;

  resultado.frequenciaCalculada =
    freq.frequenciaCalculada;

  resultado.frequenciaValida =
    freq.frequenciaValida;

  return resultado;
}

// ==================================================
// CALCULO DA TENSAO RMS - V3.3
// ==================================================

void calcularTensaoRMS(float rmsADC) {

  if (FATOR_V_POR_COUNT > FATOR_MINIMO) {

    tensaoRMS =
      rmsADC * FATOR_V_POR_COUNT;

    tensaoCalibrada = true;

  } else {

    tensaoRMS = 0.0f;

    tensaoCalibrada = false;
  }
}

// ==================================================
// APRESENTACAO DOS RESULTADOS - V3.3
// ==================================================

void apresentarResultados(
  const MedicaoAC &m
) {

  // CALCULO DA TENSAO

  calcularTensaoRMS(m.rmsAC);

  Serial.println();

  Serial.println(
    "========== BEENOUS V3.3 =========="
  );

  Serial.printf(
    "Amostras: %u\n",
    WINDOW_SAMPLES
  );

  Serial.printf(
    "Minimo ADC: %u\n",
    m.minimo
  );

  Serial.printf(
    "Maximo ADC: %u\n",
    m.maximo
  );

  Serial.printf(
    "Offset DC: %.2f\n",
    m.offsetDC
  );

  Serial.printf(
    "RMS AC: %.2f counts\n",
    m.rmsAC
  );

  // TENSAO RMS

  if (tensaoCalibrada) {

    Serial.printf(
      "Tensao RMS: %.2f V\n",
      tensaoRMS
    );

  } else {

    Serial.println(
      "Tensao RMS: NAO CALIBRADA"
    );
  }

  Serial.printf(
    "Pico a pico: %u\n",
    m.picoPico
  );

  Serial.printf(
    "Saturacoes: %u\n",
    m.saturacoes
  );

  Serial.printf(
    "Taxa configurada: %d amostras/s\n",
    SAMPLE_RATE
  );

  Serial.println(
    "----------------------------------"
  );

  Serial.printf(
    "Cruzamentos detectados: %u\n",
    m.cruzamentosDetectados
  );

  Serial.printf(
    "Periodos validos: %u\n",
    m.periodosValidos
  );

  Serial.printf(
    "Periodos rejeitados: %u\n",
    m.periodosRejeitados
  );

  Serial.printf(
    "Amplitude do sinal: %s\n",
    m.amplitudeValida ? "VALIDA" : "INSUFICIENTE"
  );

  if (m.frequenciaCalculada) {

    Serial.printf(
      "Periodo medio: %.3f amostras\n",
      m.periodoMedio
    );

    Serial.printf(
      "Desvio padrao: %.3f amostras\n",
      m.desvioPadraoPeriodo
    );

    Serial.printf(
      "Variacao dos periodos: %.2f %%\n",
      m.variacaoPeriodoPct
    );

    Serial.printf(
      "Frequencia estimada: %.2f Hz\n",
      m.frequenciaHz
    );

  } else {

    Serial.println(
      "Frequencia estimada: INDISPONIVEL"
    );
  }

  if (m.frequenciaValida) {

    Serial.println(
      "Frequencia: VALIDADA"
    );

  } else {

    Serial.println(
      "Frequencia: NAO VALIDADA"
    );
  }

  if (m.saturacoes > 0) {

    Serial.println(
      "Estado ADC: SATURACAO DETECTADA"
    );

  } else {

    Serial.println(
      "Estado ADC: SEM SATURACAO"
    );
  }

  Serial.printf(
    "Total acumulado: %lu\n",
    (unsigned long)totalSamples
  );

  Serial.println(
    "=================================="
  );
}

// ==================================================
// RECEPCAO DE AMOSTRAS
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

  Serial.println(
    "BEENOUS V3.3"
  );

  Serial.println(
    "Sistema de medicao AC"
  );

  Serial.println(
    "------------------------------"
  );

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

    stopWithError(
      "Erro na leitura ADC DMA",
      error
    );
  }

  if (
    bytesRead % SOC_ADC_DIGI_RESULT_BYTES != 0
  ) {

    Serial.println(
      "Bloco ADC com tamanho invalido."
    );

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
