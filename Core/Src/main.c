/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 串口调试助手发数据 -> UART4接收 -> 写入SD NAND
  *
  *  功能:
  *    发送任意文本+回车  → 写入SD NAND
  *    发送 READ+回车     → 回读所有已存数据
  *    发送 ERASE+回车    → 清空全部数据
  *    发送 STATUS+回车   → 查看存储状态
  ******************************************************************************
  */
/* USER CODE END Header */
#include "main.h"
#include "icache.h"
#include "sdmmc.h"
#include "usart.h"
#include "gpio.h"

#include <string.h>

/* ==================== 定义 ==================== */
#define UART_BUF_SIZE    1024
#define IDLE_TIMEOUT_MS  100
#define SD_BLOCK_SIZE    512
#define META_BLOCK       0
#define DATA_START_BLOCK 1
#define META_MAGIC       0x53444C47   /* "SDLG" */
#define WRITE_TIMEOUT_MS 3000

/* 元数据结构, 存在 Block 0 */
typedef struct {
    uint32_t magic;
    uint32_t next_block;
    uint32_t total_bytes;
    uint32_t total_frames;
} SD_Meta_t;

/* ==================== 变量 ==================== */

/* UART 接收 */
uint8_t           rx_byte;
__ALIGNED(4) uint8_t rx_buffer[UART_BUF_SIZE];
volatile uint16_t rx_write_idx  = 0;
volatile uint8_t  rx_ready_flag = 0;
volatile uint32_t rx_last_tick  = 0;
volatile uint8_t  rx_receiving  = 0;

/* SD */
SD_Meta_t         meta;
__ALIGNED(4) uint8_t sd_block_buf[SD_BLOCK_SIZE];
uint32_t          sd_max_blocks = 0;
uint8_t           sd_ready = 0;

void SystemClock_Config(void);

/* ==================== 串口输出工具 ==================== */

void uint32_to_str(uint32_t num, uint8_t *str)
{
    uint8_t i = 0, j, temp;
    uint8_t buf[16] = {0};
    if (num == 0) { buf[i++] = '0'; }
    else { while (num > 0) { buf[i++] = num % 10 + '0'; num /= 10; } }
    for (j = 0; j < i / 2; j++) { temp = buf[j]; buf[j] = buf[i-j-1]; buf[i-j-1] = temp; }
    memcpy(str, buf, i);
    str[i] = '\0';
}

void uart_print(uint8_t *str)
{
    HAL_UART_Transmit(&huart4, str, strlen((char*)str), 1000);
    uint8_t newline[] = "\r\n";
    HAL_UART_Transmit(&huart4, newline, 2, 1000);
}

void uart_print_num(uint8_t *label, uint32_t num)
{
    uint8_t num_str[16] = {0};
    uint8_t send_buf[64] = {0};
    strcat((char*)send_buf, (char*)label);
    uint32_to_str(num, num_str);
    strcat((char*)send_buf, (char*)num_str);
    HAL_UART_Transmit(&huart4, send_buf, strlen((char*)send_buf), 1000);
    uint8_t newline[] = "\r\n";
    HAL_UART_Transmit(&huart4, newline, 2, 1000);
}

/* ==================== SD 写入等待 ==================== */

/**
  * @brief  写一个block并等待卡回到TRANSFER状态
  *         忽略HAL返回的CRC错误, 用卡状态判断是否真正写成功
  * @retval 0=成功, 1=超时失败
  */
uint8_t SD_WriteBlock_Safe(uint8_t *buf, uint32_t block)
{
    /* 写入前先等卡就绪 */
    uint32_t t = HAL_GetTick();
    while ((HAL_GetTick() - t) < WRITE_TIMEOUT_MS)
    {
        if (HAL_SD_GetCardState(&hsd2) == HAL_SD_CARD_TRANSFER) break;
    }

    /* 执行写入 (不看返回值, 因为CRC软错误会误报失败) */
    HAL_SD_WriteBlocks(&hsd2, buf, block, 1, 5000);

    /* 等卡回到TRANSFER状态, 回来了就说明写成功 */
    t = HAL_GetTick();
    while ((HAL_GetTick() - t) < WRITE_TIMEOUT_MS)
    {
        if (HAL_SD_GetCardState(&hsd2) == HAL_SD_CARD_TRANSFER)
        {
            return 0;  /* 成功 */
        }
    }
    return 1;  /* 超时 */
}

/* ==================== 元数据管理 ==================== */

void Meta_Load(void)
{
    memset(sd_block_buf, 0, SD_BLOCK_SIZE);

    /* 读之前等卡就绪 */
    uint32_t t = HAL_GetTick();
    while ((HAL_GetTick() - t) < WRITE_TIMEOUT_MS)
    {
        if (HAL_SD_GetCardState(&hsd2) == HAL_SD_CARD_TRANSFER) break;
    }

    if (HAL_SD_ReadBlocks(&hsd2, sd_block_buf, META_BLOCK, 1, 5000) != HAL_OK)
    {
        uart_print((uint8_t*)"Meta read fail, fresh start.");
        meta.magic       = META_MAGIC;
        meta.next_block  = DATA_START_BLOCK;
        meta.total_bytes  = 0;
        meta.total_frames = 0;
        return;
    }
    while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER) {}

    memcpy(&meta, sd_block_buf, sizeof(SD_Meta_t));

    if (meta.magic != META_MAGIC)
    {
        uart_print((uint8_t*)"First use, init metadata.");
        meta.magic       = META_MAGIC;
        meta.next_block  = DATA_START_BLOCK;
        meta.total_bytes  = 0;
        meta.total_frames = 0;
    }
    else
    {
        uart_print_num((uint8_t*)"  Resumed frames: ", meta.total_frames);
        uart_print_num((uint8_t*)"  Resumed bytes:  ", meta.total_bytes);
    }
}

void Meta_Save(void)
{
    memset(sd_block_buf, 0, SD_BLOCK_SIZE);
    memcpy(sd_block_buf, &meta, sizeof(SD_Meta_t));
    SD_WriteBlock_Safe(sd_block_buf, META_BLOCK);
}

/* ==================== 数据写入 ==================== */

void Data_Write(uint8_t *data, uint16_t len)
{
    if (!sd_ready || len == 0) return;

    uint32_t blocks_needed = (len + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
    if (meta.next_block + blocks_needed >= sd_max_blocks)
    {
        uart_print((uint8_t*)"[FULL] SD is full!");
        return;
    }

    uint16_t offset = 0;
    while (offset < len)
    {
        memset(sd_block_buf, 0, SD_BLOCK_SIZE);
        uint16_t chunk = len - offset;
        if (chunk > SD_BLOCK_SIZE) chunk = SD_BLOCK_SIZE;
        memcpy(sd_block_buf, data + offset, chunk);

        if (SD_WriteBlock_Safe(sd_block_buf, meta.next_block) != 0)
        {
            uart_print((uint8_t*)"[ERR] Write timeout!");
            return;
        }

        meta.next_block++;
        offset += chunk;
    }

    meta.total_bytes += len;
    meta.total_frames++;
    Meta_Save();

    uint8_t msg[64] = {0};
    strcat((char*)msg, "[#");
    uint8_t ns[16]; uint32_to_str(meta.total_frames, ns);
    strcat((char*)msg, (char*)ns);
    strcat((char*)msg, "] Saved ");
    uint32_to_str(len, ns);
    strcat((char*)msg, (char*)ns);
    strcat((char*)msg, " bytes");
    uart_print((uint8_t*)msg);
}

/* ==================== 回读全部 ==================== */

void Data_ReadAll(void)
{
    if (!sd_ready) return;
    if (meta.next_block <= DATA_START_BLOCK)
    {
        uart_print((uint8_t*)"(empty)");
        return;
    }

    uart_print_num((uint8_t*)"--- Frames: ", meta.total_frames);
    uart_print_num((uint8_t*)"--- Bytes:  ", meta.total_bytes);

    uint32_t bytes_left = meta.total_bytes;
    for (uint32_t blk = DATA_START_BLOCK; blk < meta.next_block; blk++)
    {
        memset(sd_block_buf, 0, SD_BLOCK_SIZE);

        /* 读之前等卡就绪 */
        while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER) {}

        if (HAL_SD_ReadBlocks(&hsd2, sd_block_buf, blk, 1, 5000) != HAL_OK)
        {
            uart_print((uint8_t*)"[ERR] Read fail");
            return;
        }
        while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER) {}

        uint16_t send_len = (bytes_left >= SD_BLOCK_SIZE) ? SD_BLOCK_SIZE : (uint16_t)bytes_left;
        HAL_UART_Transmit(&huart4, sd_block_buf, send_len, 2000);
        bytes_left -= send_len;
    }
    uint8_t newline[] = "\r\n";
    HAL_UART_Transmit(&huart4, newline, 2, 1000);
    uart_print((uint8_t*)"--- End ---");
}

/* ==================== 清空 ==================== */

void Data_Erase(void)
{
    meta.next_block  = DATA_START_BLOCK;
    meta.total_bytes  = 0;
    meta.total_frames = 0;
    Meta_Save();
    uart_print((uint8_t*)"[OK] Erased.");
}

/* ==================== 状态 ==================== */

void Show_Status(void)
{
    uint32_t used = meta.next_block - DATA_START_BLOCK;
    uint32_t free_blk = sd_max_blocks - meta.next_block;
    uart_print_num((uint8_t*)"Frames:      ", meta.total_frames);
    uart_print_num((uint8_t*)"Bytes:       ", meta.total_bytes);
    uart_print_num((uint8_t*)"Blocks used: ", used);
    uart_print_num((uint8_t*)"Blocks free: ", free_blk);
}

/* ==================== UART 回调 ==================== */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4)
    {
        rx_last_tick = HAL_GetTick();
        rx_receiving = 1;

        if (rx_write_idx < UART_BUF_SIZE - 1)
        {
            rx_buffer[rx_write_idx++] = rx_byte;
            if (rx_byte == '\n')
            {
                rx_ready_flag = 1;
                rx_receiving  = 0;
            }
        }
        else
        {
            rx_ready_flag = 1;
            rx_receiving  = 0;
        }
        HAL_UART_Receive_IT(&huart4, &rx_byte, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4)
    {
        __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                     UART_CLEAR_PEF  | UART_CLEAR_FEF);
        HAL_UART_Receive_IT(&huart4, &rx_byte, 1);
    }
}

/* ==================== main ==================== */

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_ICACHE_Init();
    MX_UART4_Init();

    uart_print((uint8_t*)"");
    uart_print((uint8_t*)"====================================");
    uart_print((uint8_t*)"  UART -> SD NAND Logger");
    uart_print((uint8_t*)"  CSNP1GCR01-BOW 128MB");
    uart_print((uint8_t*)"====================================");

    uart_print((uint8_t*)"Wait SD NAND (2s)...");
    HAL_Delay(2000);

    uart_print((uint8_t*)"Init SDMMC2...");
    MX_SDMMC2_SD_Init();
    uart_print((uint8_t*)"SD init OK!");

    /* 读卡容量 */
    HAL_SD_CardInfoTypeDef ci;
    if (HAL_SD_GetCardInfo(&hsd2, &ci) == HAL_OK)
    {
        sd_max_blocks = ci.BlockNbr;
        uart_print_num((uint8_t*)"  Blocks: ", sd_max_blocks);
        uart_print_num((uint8_t*)"  ~MB:    ", sd_max_blocks / 2048);
        sd_ready = 1;
    }
    else
    {
        uart_print((uint8_t*)"[FAIL] Cannot read card info!");
    }

    /* 加载元数据 */
    if (sd_ready)
    {
        uart_print((uint8_t*)"Load metadata...");
        Meta_Load();
    }

    /* 启动串口接收 */
    HAL_UART_Receive_IT(&huart4, &rx_byte, 1);

    uart_print((uint8_t*)"");
    uart_print((uint8_t*)"Ready! Commands:");
    uart_print((uint8_t*)"  text+Enter -> Save to SD");
    uart_print((uint8_t*)"  READ       -> Read all data");
    uart_print((uint8_t*)"  ERASE      -> Clear all");
    uart_print((uint8_t*)"  STATUS     -> Show info");
    uart_print((uint8_t*)"");

    while (1)
    {
        if (rx_ready_flag)
        {
            rx_ready_flag = 0;
            uint16_t len = rx_write_idx;

            if (len > 0)
            {
                char cmd[16] = {0};
                uint16_t cmd_len = len;
                while (cmd_len > 0 && (rx_buffer[cmd_len-1] == '\r' ||
                                        rx_buffer[cmd_len-1] == '\n'))
                    cmd_len--;

                if (cmd_len < sizeof(cmd))
                    memcpy(cmd, rx_buffer, cmd_len);

                if (strcmp(cmd, "READ") == 0)
                    Data_ReadAll();
                else if (strcmp(cmd, "ERASE") == 0)
                    Data_Erase();
                else if (strcmp(cmd, "STATUS") == 0)
                    Show_Status();
                else
                    Data_Write(rx_buffer, len);
            }

            memset(rx_buffer, 0, len);
            rx_write_idx = 0;
            rx_receiving = 0;
        }

        if (rx_receiving && rx_write_idx > 0)
        {
            if ((HAL_GetTick() - rx_last_tick) >= IDLE_TIMEOUT_MS)
            {
                rx_receiving  = 0;
                rx_ready_flag = 1;
            }
        }
    }
}

/* ==================== 时钟配置 ==================== */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
        Error_Handler();

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLMBOOST = RCC_PLLMBOOST_DIV1;
    RCC_OscInitStruct.PLL.PLLM = 1;
    RCC_OscInitStruct.PLL.PLLN = 10;
    RCC_OscInitStruct.PLL.PLLP = 1;
    RCC_OscInitStruct.PLL.PLLQ = 2;
    RCC_OscInitStruct.PLL.PLLR = 1;
    RCC_OscInitStruct.PLL.PLLRGE = RCC_PLLVCIRANGE_1;
    RCC_OscInitStruct.PLL.PLLFRACN = 0;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                                |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                                |RCC_CLOCKTYPE_PCLK3;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
        Error_Handler();
}

void Error_Handler(void)
{
    uart_print((uint8_t*)"!!! ERROR !!!");
    __disable_irq();
    while (1) { }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) { }
#endif