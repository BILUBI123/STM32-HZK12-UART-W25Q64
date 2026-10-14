#include "w25q64.h"

#define W25Q_CMD_WRITE_ENABLE  0x06
#define W25Q_CMD_READ_STATUS  0x05
#define W25Q_CMD_READ_DATA    0x03
#define W25Q_CMD_PAGE_PROGRAM 0x02
#define W25Q_CMD_SECTOR_ERASE 0x20
#define W25Q_CMD_JEDEC_ID     0x9F

static void CS_Low(void)  { GPIO_ResetBits(GPIOB, GPIO_Pin_12); }
static void CS_High(void) { GPIO_SetBits(GPIOB, GPIO_Pin_12); }

static uint8_t SPI1_Transfer(uint8_t data)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET) {}
    SPI_I2S_SendData(SPI1, data);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET) {}
    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}

static void W25Q_WriteEnable(void)
{
    CS_Low();
    SPI1_Transfer(W25Q_CMD_WRITE_ENABLE);
    CS_High();
}

static uint8_t W25Q_ReadStatus(void)
{
    uint8_t s;
    CS_Low();
    SPI1_Transfer(W25Q_CMD_READ_STATUS);
    s = SPI1_Transfer(0xFF);
    CS_High();
    return s;
}

static void W25Q_WaitBusy(void)
{
    uint32_t timeout = 80000000UL;
    while ((W25Q_ReadStatus() & 0x01) && timeout) timeout--;
    /* A timeout is intentionally not silent in production: add error handling here. */
}

static void W25Q_SendAddress(uint32_t addr)
{
    SPI1_Transfer((uint8_t)(addr >> 16));
    SPI1_Transfer((uint8_t)(addr >> 8));
    SPI1_Transfer((uint8_t)addr);
}

void W25Q64_Init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_SPI1, ENABLE);

    /* PA5 SCK, PA7 MOSI: alternate-function push-pull; PA6 MISO: input floating */
    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = GPIO_Pin_12;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio);
    CS_High();

    SPI_I2S_DeInit(SPI1);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial = 7;
    SPI_Init(SPI1, &spi);
    SPI_Cmd(SPI1, ENABLE);
}

uint32_t W25Q64_ReadJEDECID(void)
{
    uint32_t id;
    CS_Low();
    SPI1_Transfer(W25Q_CMD_JEDEC_ID);
    id = ((uint32_t)SPI1_Transfer(0xFF) << 16);
    id |= ((uint32_t)SPI1_Transfer(0xFF) << 8);
    id |= (uint32_t)SPI1_Transfer(0xFF);
    CS_High();
    return id;
}

void W25Q64_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;
    if (!buf || addr >= W25Q64_TOTAL_SIZE || len > W25Q64_TOTAL_SIZE - addr) return;
    CS_Low();
    SPI1_Transfer(W25Q_CMD_READ_DATA);
    W25Q_SendAddress(addr);
    for (i = 0; i < len; i++) buf[i] = SPI1_Transfer(0xFF);
    CS_High();
}

void W25Q64_EraseSector(uint32_t addr)
{
    if (addr >= W25Q64_TOTAL_SIZE) return;
    addr &= ~(W25Q64_SECTOR_SIZE - 1UL);
    W25Q_WriteEnable();
    CS_Low();
    SPI1_Transfer(W25Q_CMD_SECTOR_ERASE);
    W25Q_SendAddress(addr);
    CS_High();
    W25Q_WaitBusy();
}

void W25Q64_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t n, i;
    if (!buf || addr >= W25Q64_TOTAL_SIZE || len > W25Q64_TOTAL_SIZE - addr) return;

    while (len)
    {
        n = W25Q64_PAGE_SIZE - (addr % W25Q64_PAGE_SIZE);
        if (n > len) n = len;

        W25Q_WriteEnable();
        CS_Low();
        SPI1_Transfer(W25Q_CMD_PAGE_PROGRAM);
        W25Q_SendAddress(addr);
        for (i = 0; i < n; i++) SPI1_Transfer(buf[i]);
        CS_High();
        W25Q_WaitBusy();

        addr += n;
        buf += n;
        len -= n;
    }
}
