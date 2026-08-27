/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usbd_cdc_if.h"
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BUFFER_SIZE 1024
#define UART1_TX_BUF_SIZE 512
#define USB_TX_BUF_SIZE   512
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart1_tx;

/* USER CODE BEGIN PV */
uint8_t esp_dma_buffer[BUFFER_SIZE];
uint32_t esp_read_ptr = 0;

/* USART1 TX ring buffer (PC -> ESP32), non-blocking DMA */
uint8_t uart1_tx_ring[UART1_TX_BUF_SIZE];
volatile uint16_t uart1_tx_head = 0;
volatile uint16_t uart1_tx_tail = 0;
volatile uint16_t uart1_tx_last_chunk = 0;
volatile uint8_t  uart1_tx_busy = 0;

/* USB TX ping-pong buffers (ESP32 -> PC) */
uint8_t usb_tx_buf[2][USB_TX_BUF_SIZE];
volatile uint8_t  usb_tx_cur = 0;
volatile uint16_t usb_tx_len = 0;
volatile uint8_t  usb_tx_busy = 0;

/* Command line buffer for ESP32 control commands (RST/BOOT/RUN/HELP) */
#define CMD_BUF_SIZE 16
char cmd_line[CMD_BUF_SIZE];
volatile uint16_t cmd_line_len = 0;

/* Command ack buffer - queued in IRQ, flushed in main loop.
 * We must NOT call CDC_Transmit_FS from the USB interrupt context
 * (the USB CDC stack is not re-entrant). */
char cmd_ack_buf[32];
volatile uint16_t cmd_ack_len = 0;

/* Debug counters */
volatile uint32_t rx_byte_count = 0;   /* bytes read from ESP32 RX DMA */
volatile uint32_t usb_tx_byte_count = 0; /* bytes sent to PC over USB */
volatile uint32_t uart1_tx_byte_count = 0; /* bytes sent to ESP32 over UART */

/* Deferred ESP32 reset request (set in USB IRQ, executed in main loop) */
volatile uint8_t esp32_reset_pending = 0;
volatile uint8_t esp32_reset_boot_mode = 0;

/* Non-blocking ESP32 reset state machine */
#define ESP32_RESET_HOLD_MS  100   /* how long EN is held low */
#define ESP32_BOOT0_EXTRA_MS 200   /* GPIO0 held low this much longer than EN */
volatile uint8_t  esp32_reset_active = 0;
volatile uint32_t esp32_reset_start = 0;
volatile uint8_t  esp32_reset_phase = 0; /* 0=idle,1=EN low,2=EN high wait GPIO0 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ---- ESP32 control commands over USB CDC ----
 * Commands (single line, terminated by \r or \n):
 *   RST  -> reset the ESP32 (normal boot)
 *   BOOT -> reset the ESP32 into Serial Bootloader mode
 *   RUN  -> reset the ESP32 into normal run mode
 *   HELP -> print available commands
 */
/* ---- Non-blocking ESP32 reset state machine ----
 * Called every main-loop iteration. No HAL_Delay anywhere.
 *
 * BOOT mode (bootloader):
 *   phase 1: GPIO0 low, EN low, wait ESP32_RESET_HOLD_MS
 *   phase 2: EN high (release reset) while GPIO0 stays low, wait ESP32_BOOT0_EXTRA_MS
 *   phase 3: GPIO0 high (safe), done
 *
 * Normal mode (RST/RUN):
 *   phase 1: GPIO0 high, EN low, wait ESP32_RESET_HOLD_MS
 *   phase 2: EN high, GPIO0 already high, done
 */
/* Flush all bridge buffers so stale data (e.g. "Hello" from a previous
 * normal boot) is not mixed into the next bootloader response. */
void bridge_flush(void)
{
  // Discard any unread bytes in the RX DMA circular buffer
  esp_read_ptr = (BUFFER_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx)) % BUFFER_SIZE;
  // Drop any pending USB TX data
  usb_tx_len = 0;
  usb_tx_cur = 0;
  usb_tx_busy = 0;
  // Drop any pending command ack
  cmd_ack_len = 0;
}

void esp32_reset_poll(void)
{
  if (esp32_reset_pending && !esp32_reset_active)
  {
    esp32_reset_pending = 0;
    esp32_reset_active = 1;
    esp32_reset_phase = 1;
    // Flush stale bridge data before resetting the ESP32
    bridge_flush();
    // Set GPIO0: low = bootloader, high = normal
    HAL_GPIO_WritePin(ESP32_GPIO0_GPIO_Port, ESP32_GPIO0_Pin,
                      esp32_reset_boot_mode ? GPIO_PIN_RESET : GPIO_PIN_SET);
    // Assert reset (EN low)
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_RESET);
    esp32_reset_start = HAL_GetTick();
  }
  else if (esp32_reset_active)
  {
    if (esp32_reset_phase == 1)
    {
      // Hold EN low for ESP32_RESET_HOLD_MS, then release EN
      if ((HAL_GetTick() - esp32_reset_start) >= ESP32_RESET_HOLD_MS)
      {
        HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_SET); // EN high
        if (esp32_reset_boot_mode)
        {
          // Bootloader: keep GPIO0 low longer, then release
          esp32_reset_phase = 2;
          esp32_reset_start = HAL_GetTick();
        }
        else
        {
          // Normal boot: GPIO0 already high, done
          esp32_reset_active = 0;
          bridge_flush(); // discard glitch garbage from EN toggle
        }
      }
    }
    else if (esp32_reset_phase == 2)
    {
      // Hold GPIO0 low for ESP32_BOOT0_EXTRA_MS after EN released, then release
      if ((HAL_GetTick() - esp32_reset_start) >= ESP32_BOOT0_EXTRA_MS)
      {
        HAL_GPIO_WritePin(ESP32_GPIO0_GPIO_Port, ESP32_GPIO0_Pin, GPIO_PIN_SET); // GPIO0 safe
        esp32_reset_active = 0;
        bridge_flush(); // discard glitch garbage from EN/GPIO0 toggle
      }
    }
  }
}

void esp32_handle_command(const char *cmd, uint16_t len)
{
  if (len >= 3)
  {
    if (cmd[0]=='R' && cmd[1]=='S' && cmd[2]=='T')
    {
      esp32_reset_boot_mode = 0; // normal boot
      esp32_reset_pending = 1;   // defer to main loop (no HAL_Delay in IRQ)
      memcpy(cmd_ack_buf, "OK RST\r\n", 8);
      cmd_ack_len = 8;
    }
    else if (len >= 4 && cmd[0]=='B' && cmd[1]=='O' && cmd[2]=='O' && cmd[3]=='T')
    {
      esp32_reset_boot_mode = 1; // bootloader mode
      esp32_reset_pending = 1;   // defer to main loop
      memcpy(cmd_ack_buf, "OK BOOT\r\n", 9);
      cmd_ack_len = 9;
    }
    else if (cmd[0]=='R' && cmd[1]=='U' && cmd[2]=='N')
    {
      esp32_reset_boot_mode = 0; // normal run
      esp32_reset_pending = 1;   // defer to main loop
      memcpy(cmd_ack_buf, "OK RUN\r\n", 8);
      cmd_ack_len = 8;
    }
    else if (len >= 4 && cmd[0]=='H' && cmd[1]=='E' && cmd[2]=='L' && cmd[3]=='P')
    {
      memcpy(cmd_ack_buf, "RST|BOOT|RUN|HELP\r\n", 19);
      cmd_ack_len = 19;
    }
    else if (len >= 4 && cmd[0]=='S' && cmd[1]=='T' && cmd[2]=='A' && cmd[3]=='T')
    {
      // Report debug counters
      int n = snprintf(cmd_ack_buf, sizeof(cmd_ack_buf),
                       "RX=%lu USB=%lu UART=%lu\r\n",
                       (unsigned long)rx_byte_count,
                       (unsigned long)usb_tx_byte_count,
                       (unsigned long)uart1_tx_byte_count);
      cmd_ack_len = (uint16_t)n;
    }
    else if (len >= 5 && cmd[0]=='F' && cmd[1]=='L' && cmd[2]=='U' && cmd[3]=='S' && cmd[4]=='H')
    {
      // Flush the RX DMA buffer (discard stale banner data) so the next
      // bootloader response is not corrupted. Called by the flash script
      // after the bootloader banner is drained, right before esptool runs.
      bridge_flush();
      memcpy(cmd_ack_buf, "OK FLUSH\r\n", 10);
      cmd_ack_len = 10;
    }
  }
}

/* ---- Non-blocking USART1 TX (PC -> ESP32) via DMA ---- */
void uart1_tx_pump(void);

/* Bulk-send a whole buffer to the ESP32 in one DMA transfer. This preserves
 * the integrity of esptool's SLIP-framed packets (per-byte forwarding can
 * fragment them and corrupt the bootloader's parsing). */
void uart1_send_bulk(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++)
  {
    uint16_t next = (uint16_t)((uart1_tx_head + 1) % UART1_TX_BUF_SIZE);
    if (next == uart1_tx_tail)
    {
      break; // ring full, drop
    }
    uart1_tx_ring[uart1_tx_head] = data[i];
    uart1_tx_head = next;
    uart1_tx_byte_count++;
  }
  uart1_tx_pump();

  // Detect human-readable command lines (RST/BOOT/RUN/HELP/STAT)
  for (uint16_t i = 0; i < len; i++)
  {
    uint8_t b = data[i];
    if (b == '\r' || b == '\n')
    {
      if (cmd_line_len > 0)
      {
        cmd_line[cmd_line_len] = 0;
        esp32_handle_command(cmd_line, cmd_line_len);
        cmd_line_len = 0;
      }
    }
    else if (cmd_line_len < CMD_BUF_SIZE - 1)
    {
      cmd_line[cmd_line_len++] = (char)b;
    }
    else
    {
      cmd_line_len = 0; // overflow, reset
    }
  }
}

void uart1_send_byte(uint8_t b)
{
  // Forward the byte to the ESP32 immediately (transparent bridge).
  // This is critical for esptool, which sends binary data without newlines.
  uint16_t next = (uint16_t)((uart1_tx_head + 1) % UART1_TX_BUF_SIZE);
  if (next != uart1_tx_tail)
  {
    uart1_tx_ring[uart1_tx_head] = b;
    uart1_tx_head = next;
    uart1_tx_byte_count++;
    uart1_tx_pump();
  }

  // Separately detect human-readable command lines (RST/BOOT/RUN/HELP).
  // Only intercept if the whole line matches a known command.
  if (b == '\r' || b == '\n')
  {
    if (cmd_line_len > 0)
    {
      cmd_line[cmd_line_len] = 0;
      esp32_handle_command(cmd_line, cmd_line_len);
      cmd_line_len = 0;
    }
  }
  else if (cmd_line_len < CMD_BUF_SIZE - 1)
  {
    cmd_line[cmd_line_len++] = (char)b;
  }
  else
  {
    cmd_line_len = 0; // overflow, reset
  }
}

void uart1_tx_pump(void)
{
  if (uart1_tx_busy) return;
  if (uart1_tx_head == uart1_tx_tail) return;

  uint16_t avail;
  if (uart1_tx_head > uart1_tx_tail)
  {
    avail = (uint16_t)(uart1_tx_head - uart1_tx_tail);
  }
  else
  {
    avail = (uint16_t)(UART1_TX_BUF_SIZE - uart1_tx_tail);
  }

  uart1_tx_last_chunk = avail;
  uart1_tx_busy = 1;
  HAL_UART_Transmit_DMA(&huart1, &uart1_tx_ring[uart1_tx_tail], avail);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    uart1_tx_tail = (uint16_t)((uart1_tx_tail + uart1_tx_last_chunk) % UART1_TX_BUF_SIZE);
    uart1_tx_busy = 0;
    uart1_tx_pump();
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USB_Device_Init();
  /* USER CODE BEGIN 2 */

  // 1. Boot the ESP32 normally (GPIO0 high = normal boot, EN released)
  HAL_GPIO_WritePin(ESP32_GPIO0_GPIO_Port, ESP32_GPIO0_Pin, GPIO_PIN_SET);   // GPIO0 High (Normal Boot)
  HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_RESET);       // EN Low (Reset active)
  HAL_Delay(200);                                                            // Wait for discharge

  HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_SET);         // EN High (Release Reset)
  HAL_Delay(50);                                                             // Wait for boot to start

// 2. Start the non-blocking Circular DMA receiver to listen for the ESP32
  HAL_UART_Receive_DMA(&huart1, esp_dma_buffer, BUFFER_SIZE);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE BEGIN WHILE */

// Calculate where the DMA hardware is currently writing inside our buffer.
    // The counter counts down from BUFFER_SIZE to 0 then reloads; when it is 0
    // the write pointer wraps to 0, so take modulo to stay in [0, BUFFER_SIZE).
    uint32_t esp_write_ptr = (BUFFER_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx)) % BUFFER_SIZE;

    // If the read pointer lags behind the write pointer, we have new data
    while (esp_read_ptr != esp_write_ptr)
    {
      uint8_t b = esp_dma_buffer[esp_read_ptr];
      esp_read_ptr = (esp_read_ptr + 1) % BUFFER_SIZE;
      rx_byte_count++;

      // Accumulate into the current USB TX buffer
      if (usb_tx_len < USB_TX_BUF_SIZE)
      {
        usb_tx_buf[usb_tx_cur][usb_tx_len++] = b;
      }

      // If USB is free, flush the accumulated bytes to the PC
      if (!usb_tx_busy && usb_tx_len > 0)
      {
        if (CDC_Transmit_FS(usb_tx_buf[usb_tx_cur], usb_tx_len) == USBD_OK)
        {
          usb_tx_busy = 1;
          usb_tx_byte_count += usb_tx_len;
          usb_tx_cur ^= 1;   /* switch to the other buffer */
          usb_tx_len = 0;
        }
        else
        {
          // USB busy: keep the data in the current buffer and retry next
          // iteration. Do NOT switch buffers or reset usb_tx_len.
        }
      }
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // Flush any pending command ack to the PC (deferred from IRQ context)
    if (cmd_ack_len > 0 && !usb_tx_busy)
    {
      if (CDC_Transmit_FS((uint8_t*)cmd_ack_buf, cmd_ack_len) == USBD_OK)
      {
        usb_tx_busy = 1;
        cmd_ack_len = 0;
      }
    }
    // Drive the non-blocking ESP32 reset state machine (no HAL_Delay)
    esp32_reset_poll();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_CRSInitTypeDef pInit = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV2;
  RCC_OscInitStruct.PLL.PLLN = 84;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enable the SYSCFG APB clock
  */
  __HAL_RCC_CRS_CLK_ENABLE();

  /** Configures CRS
  */
  pInit.Prescaler = RCC_CRS_SYNC_DIV1;
  pInit.Source = RCC_CRS_SYNC_SOURCE_USB;
  pInit.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  pInit.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000,1000);
  pInit.ErrorLimitValue = 34;
  pInit.HSI48CalibrationValue = 32;

  HAL_RCCEx_CRSConfig(&pInit);
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_DMADISABLEONERROR_INIT;
  huart1.AdvancedInit.DMADisableonRxError = UART_ADVFEATURE_DMA_ENABLEONRXERROR;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
  /* DMA1_Channel2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(ESP32_GPIO0_GPIO_Port, ESP32_GPIO0_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : ESP32_GPIO0_Pin */
  GPIO_InitStruct.Pin = ESP32_GPIO0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ESP32_GPIO0_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : ESP32_EN_Pin */
  GPIO_InitStruct.Pin = ESP32_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ESP32_EN_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
