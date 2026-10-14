#ifndef __W25Q64_H
#define __W25Q64_H

#include "stm32f10x.h"
#include <stdint.h>

#define W25Q64_TOTAL_SIZE  (8UL * 1024UL * 1024UL)
#define W25Q64_SECTOR_SIZE 4096UL
#define W25Q64_PAGE_SIZE   256UL

void W25Q64_Init(void);
uint32_t W25Q64_ReadJEDECID(void);
void W25Q64_Read(uint32_t addr, uint8_t *buf, uint32_t len);
void W25Q64_EraseSector(uint32_t addr);
void W25Q64_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

#endif
