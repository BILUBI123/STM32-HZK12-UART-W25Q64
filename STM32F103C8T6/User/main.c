#include "stm32f10x.h"
#include "w25q64.h"
#include <stdint.h>
#include "OLED.h"
#define FONT_BASE_ADDR 0x000000UL
#define UART_BAUD 115200UL
#define PACKET_MAX 128U
#define HEADER_SIZE 12U /* "HZK1" + size(4) + crc32(4) is 12 bytes */

static void USART1_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef us;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1, ENABLE);

    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    us.USART_BaudRate = UART_BAUD;
    us.USART_WordLength = USART_WordLength_8b;
    us.USART_StopBits = USART_StopBits_1;
    us.USART_Parity = USART_Parity_No;
    us.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    us.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &us);
    USART_Cmd(USART1, ENABLE);
}

static void UART_SendByte(uint8_t b)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
    {
    }
    USART_SendData(USART1, b);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET)
    {
    }
}

static uint8_t UART_ReadByteTimeout(uint8_t *out, uint32_t timeout)
{
    while (timeout--)
    {
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET)
        {
            *out = (uint8_t)USART_ReceiveData(USART1);
            return 1;
        }
    }
    return 0;
}

static uint8_t ReadExact(uint8_t *p, uint32_t n, uint32_t timeout_each)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!UART_ReadByteTimeout(&p[i], timeout_each))
            return 0;
    return 1;
}

static uint16_t CRC16_Update(uint16_t crc, uint8_t data)
{
    uint8_t i;
    crc ^= (uint16_t)data << 8;
    for (i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

static uint32_t CRC32_Update(uint32_t crc, uint8_t data)
{
    uint8_t i;
    crc ^= data;
    for (i = 0; i < 8; i++)
        crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    return crc;
}

static uint16_t ReadU16LE(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ReadU32LE(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void SendPacketReply(uint8_t code, uint16_t seq)
{
    UART_SendByte(code);
    UART_SendByte((uint8_t)seq);
    UART_SendByte((uint8_t)(seq >> 8));
}

static void Downloader(void)
{
    uint8_t header[12];
    uint8_t marker, pkt[5 + PACKET_MAX + 2];
    uint8_t *payload;
    uint8_t b;
    uint32_t file_size, expected_crc, addr = FONT_BASE_ADDR;
    uint32_t crc32 = 0xFFFFFFFFUL;
    uint32_t sectors, s, received = 0;
    uint16_t expected_seq = 0, seq, len, rx_crc, calc_crc;
    uint16_t i;
    uint8_t ok;

    /* Wait for 12-byte header. PC starts after user presses Send. */
    if (!ReadExact(header, HEADER_SIZE, 8000000UL))
        return;
    if (header[0] != 'H' || header[1] != 'Z' || header[2] != 'K' || header[3] != '1')
    {
        UART_SendByte('E');
        return;
    }

    file_size = ReadU32LE(&header[4]);
    expected_crc = ReadU32LE(&header[8]);

    if (file_size == 0 || file_size > W25Q64_TOTAL_SIZE - FONT_BASE_ADDR)
    {
        UART_SendByte('E');
        return;
    }

    /* Erase every sector covered by this file. */
    sectors = (file_size + W25Q64_SECTOR_SIZE - 1UL) / W25Q64_SECTOR_SIZE;
    for (s = 0; s < sectors; s++)
        W25Q64_EraseSector(FONT_BASE_ADDR + s * W25Q64_SECTOR_SIZE);

    UART_SendByte('R');

    while (received < file_size)
    {
        if (!UART_ReadByteTimeout(&marker, 8000000UL))
            return;
        if (marker != 0xA5)
        {
            UART_SendByte('E');
            return;
        }

        /* seq(2), len(2), payload(len), crc16(2) */
        if (!ReadExact(pkt, 4, 8000000UL))
            return;
        seq = ReadU16LE(&pkt[0]);
        len = ReadU16LE(&pkt[2]);

        if (len == 0 || len > PACKET_MAX || len > file_size - received)
        {
            SendPacketReply(0x15, seq);
            return;
        }

        payload = &pkt[4];
        if (!ReadExact(payload, len, 8000000UL) || !ReadExact(&pkt[4 + len], 2, 8000000UL))
            return;
        rx_crc = ReadU16LE(&pkt[4 + len]);

        calc_crc = 0xFFFF;
        for (i = 0; i < 4; i++)
            calc_crc = CRC16_Update(calc_crc, pkt[i]);
        for (i = 0; i < len; i++)
            calc_crc = CRC16_Update(calc_crc, payload[i]);

        if (calc_crc != rx_crc)
        {
            SendPacketReply(0x15, seq);
            continue;
        }

        /* If the ACK was lost, the PC may resend the previous packet.
           Acknowledge it again without writing or updating the whole-file CRC twice. */
        if (expected_seq > 0 && seq == (uint16_t)(expected_seq - 1U))
        {
            SendPacketReply(0x06, seq);
            continue;
        }

        if (seq != expected_seq)
        {
            SendPacketReply(0x15, seq);
            continue;
        }

        W25Q64_Write(addr, payload, len);

        /* Verify this packet by reading it back in small chunks. */
        {
            uint8_t verify[PACKET_MAX];
            W25Q64_Read(addr, verify, len);
            ok = 1;
            for (i = 0; i < len; i++)
                if (verify[i] != payload[i])
                {
                    ok = 0;
                    break;
                }
        }

        if (!ok)
        {
            SendPacketReply(0x15, seq);
            return;
        }

        for (i = 0; i < len; i++)
            crc32 = CRC32_Update(crc32, payload[i]);
        addr += len;
        received += len;
        expected_seq++;
        SendPacketReply(0x06, seq);
    }

    if (!UART_ReadByteTimeout(&b, 8000000UL) || b != 0x5A)
    {
        UART_SendByte('E');
        return;
    }

    crc32 ^= 0xFFFFFFFFUL;
    UART_SendByte((crc32 == expected_crc) ? 'D' : 'E');
}

/* 根据 GB2312 编码从 W25Q64 读取汉字并显示
 * gbCode: 例如 "中" = 0xD6D0
 */
void OLED_ShowChineseGB(uint8_t Line, uint8_t X, uint16_t gbCode)
{
    uint8_t hzk[24];
    uint8_t high = gbCode >> 8;
    uint8_t low = gbCode & 0xFF;
    uint32_t offset;

    /* 合法性检查：GB2312 汉字区码 0xA1~0xF7，位码 0xA1~0xFE */
    if (high < 0xA1 || high > 0xF7 || low < 0xA1 || low > 0xFE)
        return;

    /* HZK12 每个汉字 24 字节，按区位码顺序排列 */
    offset = ((uint32_t)(high - 0xA1) * 94 + (low - 0xA1)) * 24;

    W25Q64_Read(FONT_BASE_ADDR + offset, hzk, 24);
    OLED_ShowChinese12x12(Line, X, hzk);
}

// 用 CRC32 整片比对，验证字库是否完整
uint32_t Font_Crc32(uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFUL, i;
    uint8_t buf[256];
    uint32_t done = 0;

    while (done < size)
    {
        uint32_t chunk = sizeof(buf);
        if (chunk > size - done)
            chunk = size - done;

        W25Q64_Read(FONT_BASE_ADDR + done, buf, chunk);
        for (i = 0; i < chunk; i++)
        {
            crc ^= buf[i];
            for (uint8_t k = 0; k < 8; k++)
                crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
        done += chunk;
    }
    return crc ^ 0xFFFFFFFFUL;
}

void Font_VerifyByCrc(void)
{
    uint32_t size = 196272UL;       /* 你的字库文件大小 */
    uint32_t expected = 0x88CAD8F9; /* PC 端算出来的 CRC32 */
    uint32_t actual;

    OLED_Clear();
    // OLED_ShowString(1, 1, "CRC CHECK");

    actual = Font_Crc32(size);

    if (actual == expected)
    {
        OLED_ShowString(3, 1, "CRC OK");
    }
    else
    {
        OLED_ShowString(3, 1, "CRC FAIL");
        OLED_ShowHexNum(4, 1, actual, 8); /* 显示实际算出的值，方便排查 */
    }

    /* 不论成功失败都把实际值显示在最后一行，方便肉眼对比 */
    OLED_ShowHexNum(1, 1, expected, 8);
    OLED_ShowHexNum(2, 1, actual, 8);
}


//通过py脚本生成
#define STR_0 "\xD6\xD0\xCE\xC4\xD7\xD6\xBF\xE2"
#define STR_1 "\xCF\xC2\xD4\xD8\xB3\xC9\xB9\xA6"
#define STR_2 "\xCF\xC2\xD4\xD8\xCA\xA7\xB0\xDC"
#define STR_3 "\xC7\xEB\xD1\xA1\xD4\xF1\xB4\xAE\xBF\xDA"
#define STR_4 "\xD5\xFD\xD4\xDA\xB7\xA2\xCB\xCD"

int main(void)
{
    SystemInit();
    USART1_Init();
    W25Q64_Init();
    OLED_Init();

    /* Optional sanity check: W25Q64 JEDEC ID commonly starts with 0xEF4017. */
    (void)W25Q64_ReadJEDECID();
    OLED_Clear();

    /* 显示 "中文字库" 四个字，每个字宽 12，间隔 16 像素 */
    //    OLED_ShowChineseGB(0,  0, 0xD6D0);  /* 中 */
    //    OLED_ShowChineseGB(0, 16, 0xCEC4);  /* 文 */
    //    OLED_ShowChineseGB(0, 32, 0xD7D6);  /* 字 */
    //    OLED_ShowChineseGB(0, 48, 0xBFE2);  /* 库 */

    OLED_ShowChineseString(0, 0, STR_3);
    OLED_ShowChineseString(2, 0, STR_4);


	  
    // Font_VerifyByCrc();  /* 验证字库是否完整 */
    while (1)
    {
                uint8_t start;
                if (UART_ReadByteTimeout(&start, 8000000UL) && start == 'S')
                    Downloader();
    }
}
