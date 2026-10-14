#ifndef EEPROM_H
#define EEPROM_H

#include "sdk_project_config.h"
#include <stdint.h>

status_t EEPROM_ReadBytes(uint16_t address,uint8_t *data,uint16_t length);

status_t EEPROM_WriteBytes(uint16_t address,const uint8_t *data,uint16_t length);

#endif
