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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BUFFER_SIZE 256
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

/* ---- Non-blocking USART1 TX (PC -> ESP32) via DMA ---- */
void uart1_tx_pump(void);

void uart1_send_byte(uint8_t b)
{
  uint16_t next = (uint16_t)((uart1_tx_head + 1) % UART1_TX_BUF_SIZE);
  if (next == uart1_tx_tail)
  {
    return; /* ring full, drop byte */
  }
  uart1_tx_ring[uart1_tx_head] = b;
  uart1_tx_head = next;
  uart1_tx_pump();
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
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */

  // 1. Force the ESP32 into Serial Bootloader Mode using your PA8 and PB15 pins
  HAL_GPIO_WritePin(ESP32_GPIO0_GPIO_Port, ESP32_GPIO0_Pin, GPIO_PIN_RESET); // PB15 Low (Boot Mode)
  HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_RESET);       // PA8 Low (Reset active)
  HAL_Delay(200);                                                            // Wait for discharge

  HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_SET);         // PA8 High (Release Reset)
  HAL_Delay(50);                                                             // Wait for bootloader to catch
  HAL_GPIO_WritePin(ESP32_GPIO0_GPIO_Port, ESP32_GPIO0_Pin, GPIO_PIN_SET);   // PB15 High (Safe state)

  // 2. Start the non-blocking Circular DMA receiver to listen for the ESP32
  HAL_UART_Receive_DMA(&huart1, esp_dma_buffer, BUFFER_SIZE);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE BEGIN WHILE */

    // Calculate where the DMA hardware is currently writing inside our buffer
    uint32_t esp_write_ptr = BUFFER_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx);

    // If the read pointer lags behind the write pointer, we have new data
    while (esp_read_ptr != esp_write_ptr)
    {
      uint8_t b = esp_dma_buffer[esp_read_ptr];
      esp_read_ptr = (esp_read_ptr + 1) % BUFFER_SIZE;

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
          usb_tx_cur ^= 1;   /* switch to the other buffer */
          usb_tx_len = 0;
        }
      }
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
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
  if (HAL_UART_Init(&huart1) != HAL_OK)
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
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
  /* DMA2_Stream7_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream7_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream7_IRQn);

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
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
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
