#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usbd_cdc_if.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>


/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_rx;
DMA_HandleTypeDef hdma_spi1_tx;

/* USER CODE BEGIN PV */

#define FPGA_CMD_LOAD_KEY   0x01
#define FPGA_CMD_LOAD_IV    0x02
#define FPGA_CMD_SEND_PT    0x03
#define FPGA_CMD_READ_CT    0x04
#define FPGA_CMD_STATUS     0x05

#define FPGA_STATUS_CT_VALID   0x01
#define FPGA_STATUS_BUSY       0x02
#define FPGA_STATUS_KEY_READY  0x04

#define FPGA_CS_GPIO_Port GPIOA
#define FPGA_CS_Pin       GPIO_PIN_4

static volatile uint8_t usb_cmd_ready = 0;
static char usb_cmd_buffer[256];
static char usb_cmd_line[256];

static uint8_t g_key[16];
static uint8_t g_iv[16];
static uint8_t g_pt[16];
static uint8_t g_ct[16];

#define AES_BLOCK_SIZE          16U
#define USER_DATA_MIN_BYTES     16U
#define USER_DATA_MAX_BYTES     160U

/*****************************************************/

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_SPI1_Init(void);
/* USER CODE BEGIN PFP */

/***************************************************************************/
void APP_UsbCommandReceived(char *data);
static void APP_ProcessCommand(char *cmd);
static void usb_send_text(const char *s);
static void usb_send_line(const char *s);
static int hexchar_to_val(char c);
static int hexstr_to_bytes_16(const char *hex, uint8_t out[16]);
static void bytes_to_hexstr(const uint8_t *in, uint32_t len, char *out);
/****************************************************************************/

static void FPGA_CS_Low(void)
{
    HAL_GPIO_WritePin(FPGA_CS_GPIO_Port, FPGA_CS_Pin, GPIO_PIN_RESET);
}

static void FPGA_CS_High(void)
{
    HAL_GPIO_WritePin(FPGA_CS_GPIO_Port, FPGA_CS_Pin, GPIO_PIN_SET);
}

static HAL_StatusTypeDef fpga_spi_tx(uint8_t *tx, uint16_t len)
{
    return HAL_SPI_Transmit(&hspi1, tx, len, HAL_MAX_DELAY);
}

static HAL_StatusTypeDef fpga_spi_txrx(uint8_t *tx, uint8_t *rx, uint16_t len)
{
    return HAL_SPI_TransmitReceive(&hspi1, tx, rx, len, HAL_MAX_DELAY);
}

static HAL_StatusTypeDef FPGA_LoadKey(const uint8_t key[16])
{
    uint8_t packet[17];
    packet[0] = FPGA_CMD_LOAD_KEY;
    memcpy(&packet[1], key, 16);

    FPGA_CS_Low();
    HAL_StatusTypeDef ret = fpga_spi_tx(packet, sizeof(packet));
    FPGA_CS_High();

    return ret;
}

static HAL_StatusTypeDef FPGA_LoadIV(const uint8_t iv[16])
{
    uint8_t packet[17];
    packet[0] = FPGA_CMD_LOAD_IV;
    memcpy(&packet[1], iv, 16);

    FPGA_CS_Low();
    HAL_StatusTypeDef ret = fpga_spi_tx(packet, sizeof(packet));
    FPGA_CS_High();

    return ret;
}

static HAL_StatusTypeDef FPGA_SendPlaintext(const uint8_t pt[16])
{
    uint8_t packet[17];
    packet[0] = FPGA_CMD_SEND_PT;
    memcpy(&packet[1], pt, 16);

    FPGA_CS_Low();
    HAL_StatusTypeDef ret = fpga_spi_tx(packet, sizeof(packet));
    FPGA_CS_High();

    return ret;
}

static HAL_StatusTypeDef FPGA_ReadStatus(uint8_t *status)
{
    uint8_t tx[2] = {FPGA_CMD_STATUS, 0x00};
    uint8_t rx[2] = {0};

    FPGA_CS_Low();
    HAL_StatusTypeDef ret = fpga_spi_txrx(tx, rx, sizeof(tx));
    FPGA_CS_High();

    if (ret == HAL_OK)
    {
        *status = rx[1];
    }

    return ret;
}

static HAL_StatusTypeDef FPGA_ReadCiphertext(uint8_t ct[16])
{
    uint8_t tx[17] = {0};
    uint8_t rx[17] = {0};

    tx[0] = FPGA_CMD_READ_CT;

    FPGA_CS_Low();
    HAL_StatusTypeDef ret = fpga_spi_txrx(tx, rx, sizeof(tx));
    FPGA_CS_High();

    if (ret == HAL_OK)
    {
        memcpy(ct, &rx[1], 16);
    }

    return ret;
}

HAL_StatusTypeDef FPGA_WaitKeyReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t st = 0;
    char msg[32];

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        if (FPGA_ReadStatus(&st) != HAL_OK)
        {
            usb_send_line("ERR STAT SPI");
            return HAL_ERROR;
        }

        sprintf(msg, "DBG ST %02X", st);
        usb_send_line(msg);

        if (st & 0x04)
            return HAL_OK;

        HAL_Delay(100);
    }

    return HAL_TIMEOUT;
}


static HAL_StatusTypeDef FPGA_WaitCiphertextReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t status = 0;

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        if (FPGA_ReadStatus(&status) != HAL_OK)
        {
            return HAL_ERROR;
        }

        if ((status & FPGA_STATUS_CT_VALID) && !(status & FPGA_STATUS_BUSY))
        {
            return HAL_OK;
        }
    }

    return HAL_TIMEOUT;
}

static HAL_StatusTypeDef FPGA_EncryptBlock(const uint8_t pt[16], uint8_t ct[16])
{
    if (FPGA_SendPlaintext(pt) != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (FPGA_WaitCiphertextReady(100) != HAL_OK)
    {
        return HAL_TIMEOUT;
    }

    if (FPGA_ReadCiphertext(ct) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}

/****************************************************************************************************************************************/


/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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
  MX_SPI1_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */

  FPGA_CS_High();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	      static uint32_t last_blink = 0;

	      if (usb_cmd_ready)
	      {
	          usb_cmd_ready = 0;
	          APP_ProcessCommand(usb_cmd_line);
	      }

	      if (HAL_GetTick() - last_blink >= 150)
	      {
	          last_blink = HAL_GetTick();
	          HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
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
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);

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
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(FPGA_CS_GPIO_Port, FPGA_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : LED_STATUS_Pin */
  GPIO_InitStruct.Pin = LED_STATUS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_STATUS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : FPGA_CS_Pin */
  GPIO_InitStruct.Pin = FPGA_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(FPGA_CS_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

void APP_UsbCommandReceived(char *data)
{
    size_t len;

    strncpy(usb_cmd_line, data, sizeof(usb_cmd_line) - 1);
    usb_cmd_line[sizeof(usb_cmd_line) - 1] = '\0';

    len = strlen(usb_cmd_line);

    while (len > 0U &&
          (usb_cmd_line[len - 1U] == '\r' || usb_cmd_line[len - 1U] == '\n'))
    {
        usb_cmd_line[len - 1U] = '\0';
        len--;
    }

    usb_cmd_ready = 1;
}


static void usb_send_text(const char *s)
{
    while (CDC_Transmit_FS((uint8_t*)s, strlen(s)) == USBD_BUSY)
    {
        HAL_Delay(1);
    }
}

static void usb_send_line(const char *s)
{
    usb_send_text(s);
    usb_send_text("\r\n");
}

static int hexchar_to_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;

    return -1;
}

static int hexstr_to_bytes_16(const char *hex, uint8_t out[16])
{
    for (int i = 0; i < 16; i++)
    {
        int hi = hexchar_to_val(hex[i * 2]);
        int lo = hexchar_to_val(hex[i * 2 + 1]);

        if (hi < 0 || lo < 0)
        {
            return 0;
        }

        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return 1;
}

static void bytes_to_hexstr(const uint8_t *in, uint32_t len, char *out)
{
    static const char hex[] = "0123456789ABCDEF";

    for (uint32_t i = 0; i < len; i++)
    {
        out[2 * i] = hex[(in[i] >> 4) & 0x0F];
        out[2 * i + 1] = hex[in[i] & 0x0F];
    }

    out[2 * len] = '\0';
}

static HAL_StatusTypeDef APP_EncryptUserData(const uint8_t *input,
                                             uint32_t input_len,
                                             uint8_t *output)
{
    uint8_t block[AES_BLOCK_SIZE];
    uint8_t ct[AES_BLOCK_SIZE];
    uint32_t offset = 0;

    if (input_len < USER_DATA_MIN_BYTES || input_len > USER_DATA_MAX_BYTES)
    {
        return HAL_ERROR;
    }

    while (offset < input_len)
    {
        uint32_t remaining = input_len - offset;
        uint32_t chunk_len = (remaining >= AES_BLOCK_SIZE) ? AES_BLOCK_SIZE : remaining;

        memset(block, 0, sizeof(block));
        memcpy(block, input + offset, chunk_len);

        if (FPGA_EncryptBlock(block, ct) != HAL_OK)
        {
            return HAL_ERROR;
        }

        memcpy(output + offset, ct, chunk_len);

        offset += chunk_len;
    }

    return HAL_OK;
}

static void APP_ProcessCommand(char *cmd)
{
    while (*cmd == ' ' || *cmd == '\t')
    {
        cmd++;
    }

    if (strcmp(cmd, "STAT") == 0)
    {
        uint8_t st = 0;
        char resp[16];

        if (FPGA_ReadStatus(&st) == HAL_OK)
        {
            snprintf(resp, sizeof(resp), "ST %02X", st);
            usb_send_line(resp);
        }
        else
        {
            usb_send_line("ERR");
        }

        return;
    }

    if (strncmp(cmd, "KEY ", 4) == 0)
    {
        if (strlen(cmd + 4) != 32U)
        {
            usb_send_line("ERR KEY LEN");
            return;
        }

        if (!hexstr_to_bytes_16(cmd + 4, g_key))
        {
            usb_send_line("ERR KEY HEX");
            return;
        }

        if (FPGA_LoadKey(g_key) != HAL_OK)
        {
            usb_send_line("ERR KEY LOAD");
            return;
        }

        if (FPGA_WaitKeyReady(1000) != HAL_OK)
        {
            usb_send_line("ERR KEY READY");
            return;
        }

        usb_send_line("OK");
        return;
    }

    if (strncmp(cmd, "IV ", 3) == 0)
    {
        if (strlen(cmd + 3) != 32U)
        {
            usb_send_line("ERR IV LEN");
            return;
        }

        if (!hexstr_to_bytes_16(cmd + 3, g_iv))
        {
            usb_send_line("ERR IV HEX");
            return;
        }

        if (FPGA_LoadIV(g_iv) != HAL_OK)
        {
            usb_send_line("ERR IV LOAD");
            return;
        }

        usb_send_line("OK");
        return;
    }

    if (strncmp(cmd, "ENC ", 4) == 0)
    {
        char hexout[33];
        char resp[40];

        if (strlen(cmd + 4) != 32U)
        {
            usb_send_line("ERR ENC LEN");
            return;
        }

        if (!hexstr_to_bytes_16(cmd + 4, g_pt))
        {
            usb_send_line("ERR ENC HEX");
            return;
        }

        if (FPGA_EncryptBlock(g_pt, g_ct) != HAL_OK)
        {
            usb_send_line("ERR ENC");
            return;
        }

        bytes_to_hexstr(g_ct, 16, hexout);
        snprintf(resp, sizeof(resp), "CT %s", hexout);
        usb_send_line(resp);
        return;
    }

    if (strncmp(cmd, "TEXT ", 5) == 0)
    {
        const uint8_t *user_data = (const uint8_t *)(cmd + 5);
        uint32_t user_len = (uint32_t)strlen(cmd + 5);

        uint8_t encrypted[USER_DATA_MAX_BYTES];
        char hexout[(USER_DATA_MAX_BYTES * 2U) + 1U];
        char resp[(USER_DATA_MAX_BYTES * 2U) + 4U];

        if (user_len < USER_DATA_MIN_BYTES)
        {
            usb_send_line("ERR TEXT MIN 16 BYTE");
            return;
        }

        if (user_len > USER_DATA_MAX_BYTES)
        {
            usb_send_line("ERR TEXT MAX 160 BYTE");
            return;
        }

        if (APP_EncryptUserData(user_data, user_len, encrypted) != HAL_OK)
        {
            usb_send_line("ERR TEXT ENC");
            return;
        }

        bytes_to_hexstr(encrypted, user_len, hexout);
        snprintf(resp, sizeof(resp), "CT %s", hexout);
        usb_send_line(resp);
        return;
    }

    usb_send_line("ERR");
}


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
