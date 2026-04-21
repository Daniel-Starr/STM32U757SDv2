/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdmmc.c
  * @brief   SDMMC2 - VIT6原版 + 内部上拉

  ******************************************************************************
  */
/* USER CODE END Header */
#include "sdmmc.h"

SD_HandleTypeDef hsd2;

void MX_SDMMC2_SD_Init(void)
{
  hsd2.Instance = SDMMC2;
  hsd2.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd2.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd2.Init.BusWide = SDMMC_BUS_WIDE_1B;
  hsd2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd2.Init.ClockDiv = 10;
  if (HAL_SD_Init(&hsd2) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_SD_MspInit(SD_HandleTypeDef* sdHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(sdHandle->Instance==SDMMC2)
  {
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_SDMMC;
    PeriphClkInit.SdmmcClockSelection = RCC_SDMMCCLKSOURCE_PLL1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_RCC_SDMMC2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /**SDMMC2 GPIO Configuration
    PB14     ------> SDMMC2_D0   (上拉)
    PB15     ------> SDMMC2_D1   (上拉)
    PB3      ------> SDMMC2_D2   (上拉)
    PB4      ------> SDMMC2_D3   (上拉)
    PD6      ------> SDMMC2_CK   (不上拉)
    PD7      ------> SDMMC2_CMD  (上拉)
    */

    /* D0-D3: 上拉 */
    GPIO_InitStruct.Pin = GPIO_PIN_14|GPIO_PIN_15|GPIO_PIN_3|GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* CK: 不上拉 */
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_SDMMC2;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* CMD: 上拉 */
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_SDMMC2;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
  }
}

void HAL_SD_MspDeInit(SD_HandleTypeDef* sdHandle)
{
  if(sdHandle->Instance==SDMMC2)
  {
    __HAL_RCC_SDMMC2_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14|GPIO_PIN_15|GPIO_PIN_3|GPIO_PIN_4);
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_6|GPIO_PIN_7);
  }
}