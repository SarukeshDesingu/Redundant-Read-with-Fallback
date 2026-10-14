#include "eeprom.h"

#define EEPROM_ADDRESS  0x50U
#define EEPROM_TIMEOUT  100U

static uint8_t txBuffer[17];

status_t EEPROM_ReadBytes(uint16_t address,uint8_t *data,uint16_t length)
{
    status_t status;
    uint8_t wordAddress;

    if ((data == NULL) || (length == 0U) ||
        ((uint32_t)address + length > 256U))
    {
        return STATUS_ERROR;
    }

    LPI2C_DRV_MasterSetSlaveAddr(INST_LPI2C0, EEPROM_ADDRESS, false);

    wordAddress = (uint8_t)address;

    status = LPI2C_DRV_MasterSendDataBlocking(INST_LPI2C0,&wordAddress,1U,false,EEPROM_TIMEOUT);

    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    return LPI2C_DRV_MasterReceiveDataBlocking(INST_LPI2C0,data,length,true,EEPROM_TIMEOUT);
}

status_t EEPROM_WriteBytes(uint16_t address,const uint8_t *data,uint16_t length)
{
    status_t status;
    uint16_t i;

    if ((data == NULL) || (length == 0U) ||
        (length > 16U) ||
        ((uint32_t)address + length > 256U) ||
        ((address / 16U) !=
         ((address + length - 1U) / 16U)))
    {
        return STATUS_ERROR;
    }

    txBuffer[0] = (uint8_t)address;

    for (i = 0U; i < length; i++)
    {
        txBuffer[i + 1U] = data[i];
    }

    LPI2C_DRV_MasterSetSlaveAddr(INST_LPI2C0, EEPROM_ADDRESS, false);

    status = LPI2C_DRV_MasterSendDataBlocking(INST_LPI2C0,txBuffer,(uint32_t)(length + 1U),true,EEPROM_TIMEOUT);

    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    OSIF_TimeDelay(5U);

    return STATUS_SUCCESS;
}
