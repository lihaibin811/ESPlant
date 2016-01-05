#include <stdbool.h>

#include "stm32f0xx_hal.h"

#include "esp_adc_intf.h"
static void i2c_init(void);
static void adc_init(void);

#define ADC1_IRQ_PRIORITY 2 /* higher priority than i2c, prevents race when copying value */
#define I2C1_IRQ_PRIORITY 3

ADC_HandleTypeDef hadc;
I2C_HandleTypeDef hi2c1;

static volatile uint8_t adc_channel = 0xFF; /* single byte ADC register number */
static volatile uint16_t adc_value;
static volatile uint8_t adc_value_tx_index; /* index (0-2) of next byte to transmit to master. ADC_NOT_READY when conversion still being completed. */

#define ADC_NOT_READY 0xFF

/* number of ADC channels exposed over i2c */
#define ADC_NUM_CHANNELS 7

/* Map from the i2c-facing channel numbers to internal STM32 channels */
static const uint8_t ADC_CHANNEL_MAP[ADC_NUM_CHANNELS] = {
  ADC_CHANNEL_0, /* 0 = "ADC0" */
  ADC_CHANNEL_1, /* 1 = "ADC1" */
  ADC_CHANNEL_4, /* 2 = "ADC2" */
  ADC_CHANNEL_5, /* 3 = "ADC3/SOIL1" */
  ADC_CHANNEL_6, /* 4 = "ADC4/SOIL2" */
  ADC_CHANNEL_9, /* 5 = Battery voltage monitor */
  ADC_CHANNEL_TEMPSENSOR, /* 6 = internal temp sensor */
};

void esp_adc_intf_init(void)
{
  adc_init();
  i2c_init();
}

static void adc_init(void)
{
  /**Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
   */
  hadc.Instance = ADC1;
  hadc.Init.ClockPrescaler = ADC_CLOCK_ASYNC;
  hadc.Init.Resolution = ADC_RESOLUTION12b;
  hadc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc.Init.ScanConvMode = DISABLE;
  hadc.Init.EOCSelection = EOC_SINGLE_CONV;
  hadc.Init.LowPowerAutoWait = DISABLE;
  hadc.Init.LowPowerAutoPowerOff = DISABLE;
  hadc.Init.ContinuousConvMode = DISABLE;
  hadc.Init.DiscontinuousConvMode = DISABLE;
  hadc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc.Init.DMAContinuousRequests = DISABLE;
  hadc.Init.Overrun = OVR_DATA_OVERWRITTEN;
  HAL_ADC_Init(&hadc);

  __ADC1_CLK_ENABLE();

  NVIC_SetPriority(ADC1_IRQn, ADC1_IRQ_PRIORITY);
  NVIC_EnableIRQ(ADC1_IRQn);
}

static void i2c_init(void)
{
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x2000090E;
  hi2c1.Init.OwnAddress1 = 0x50<<1;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLED;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLED;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  HAL_I2C_Init(&hi2c1);

  /**Configure Analogue filter
  */
  //HAL_I2CEx_AnalogFilter_Config(&hi2c1, I2C_ANALOGFILTER_ENABLED);

  /* Interrupt mask - enable ERR, TC, STOP, NACK, TXI, RXI interrupt */
  /* possible to enable all of these */
  /* I2C_IT_ERRI | I2C_IT_TCI| I2C_IT_STOPI| I2C_IT_NACKI | I2C_IT_ADDRI | I2C_IT_RXI | I2C_IT_TXI */
  __HAL_I2C_ENABLE_IT(&hi2c1,I2C_IT_ERRI | I2C_IT_TCI| I2C_IT_STOPI | I2C_IT_NACKI | I2C_IT_ADDRI | I2C_IT_TXI | I2C_IT_RXI );

  NVIC_SetPriority(I2C1_IRQn, I2C1_IRQ_PRIORITY);
  NVIC_EnableIRQ(I2C1_IRQn);
}

static void i2c_tx_byte()
{
  volatile uint8_t *tx_buf = (volatile uint8_t *)&adc_value;
  hi2c1.Instance->TXDR = tx_buf[adc_value_tx_index++];
}

void I2C1_IRQHandler(void)
{
  bool is_i2c_write = __HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_DIR);

  /* Check for address interrupt */
  if(__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_ADDR)) {
    __HAL_I2C_CLEAR_FLAG(&hi2c1,I2C_FLAG_ADDR);
    if(!is_i2c_write) {
      /* Flush the TXDR register on commencing read */
      hi2c1.Instance->ISR = I2C_FLAG_TXE;
    }
  }

  /* Check for TXI interrupt (i2c master reading from us) */
  if(__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_TXIS)
     && !__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_STOPF)) {
    if(adc_value_tx_index < 2) {
      i2c_tx_byte();
    }
  }

  /* Check for RXNE interrupt (i2c master sent us stuff) */
  if(__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_RXNE)) {
    adc_channel = hi2c1.Instance->RXDR;

    if(adc_channel < ADC_NUM_CHANNELS) {
      /* got ADC channel, configure ADC, start conversion */
      int i;
      for(i = 0; i < ADC_NUM_CHANNELS; i++) {
	/* disable every channel except the one we're using */
	ADC_ChannelConfTypeDef adc_config;
	adc_config.Channel = ADC_CHANNEL_MAP[i];
	adc_config.Rank = (adc_channel == i) ?  ADC_RANK_CHANNEL_NUMBER : ADC_RANK_NONE;
	adc_config.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
	HAL_ADC_ConfigChannel(&hadc, &adc_config);
      }
      adc_value_tx_index = ADC_NOT_READY;
      HAL_ADC_Start_IT(&hadc);
    } else {
      /* invalid channel, prepare to return an invalid read */
      adc_value = 0xFFFF;
      adc_value_tx_index = 0;
    }
  }

  /* Check for STOP interrupt (i2c transaction done) */
  if(__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_STOPF)) {
    hi2c1.Instance->CR2 |= I2C_CR2_NACK;
    __HAL_I2C_CLEAR_FLAG(&hi2c1, I2C_FLAG_STOPF);
    if(is_i2c_write) {
    }
  }

  /* Check for NACK */
  if(__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_AF)) {
    __HAL_I2C_CLEAR_FLAG(&hi2c1, I2C_FLAG_AF);
  }

  /* Check for overrun */
  if(__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_OVR)) {
    __HAL_I2C_CLEAR_FLAG(&hi2c1, I2C_FLAG_OVR);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc_unused)
{
  adc_value = (uint16_t)HAL_ADC_GetValue(&hadc);
  /* ready to read back */
  adc_value_tx_index = 0;
  /* queue first byte, this will cancel any clock stretching if necessary */
  i2c_tx_byte();
}
