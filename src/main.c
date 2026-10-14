#include "sdk_project_config.h"
#include "eeprom.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* NVM request status */
#define NVM_REQ_OK                  0U
#define NVM_REQ_RESTORED_FROM_ROM   1U
#define NVM_REQ_INTEGRITY_FAILED    2U

/* EEPROM addresses */
#define COPY1_DATA_ADDR             0x0000U
#define COPY1_CRC_ADDR              0x0004U
#define COPY2_DATA_ADDR             0x0010U
#define COPY2_CRC_ADDR              0x0014U
#define ROM_BLOCK_ADDR              0x0020U

/*
 * EEPROM provisioning control:
 * 1U = Write initial test data to EEPROM
 * 0U = Do not write initial data; use existing EEPROM contents
 *
 * Set to 1U for the first run, then change to 0U.
 */

#define ENABLE_EEPROM_PROVISIONING  1U

/* Set Initial Values for ROM_BLOCK ,COPY 1,2-data */
#define INITIAL_ROM_BLOCK           0U
#define INITIAL_COPY1_DATA          67892U
#define INITIAL_COPY2_DATA          12345U

static lpi2c_master_state_t lpi2cState;
static uint32_t remaining = 0U;
static uint8_t ROM_BLOCK = 0U;
static char txBuff[120];


// UART DISPLAY FUNCTION
static void UART_Print(const char *message)
{
    LPUART_DRV_SendData(INST_LPUART_1,(const uint8_t *)message,(uint32_t)strlen(message));

    while (LPUART_DRV_GetTransmitStatus(INST_LPUART_1,&remaining) == STATUS_BUSY)
    {
    }
}

//CRC CALCULATION FOR THE GIVEN DATA
static uint32_t CalculateCRC(uint32_t data)
{
    CRC_DRV_Init(INST_CRC_1, &crc_1_Cfg0);
    CRC_DRV_WriteData(INST_CRC_1,(const uint8_t *)&data,sizeof(data));
    return CRC_DRV_GetCrcResult(INST_CRC_1);
}


//Read a 32-bit value from EEPROM
static status_t EEPROM_Read32(uint16_t address, uint32_t *value)
{
    if (value == NULL)   return STATUS_ERROR;
    return EEPROM_ReadBytes(address,(uint8_t *)value,sizeof(*value));
}


/*
 * Provision EEPROM with initial test data.
 *
 * Copy 1 data is initialized to INITIAL_COPY1_DATA.
 * Its CRC is deliberately calculated from a different value,
 * so Copy 1 CRC verification will fail.
 *
 * Copy 2 data and CRC are initialized correctly.
 */
static status_t ProvisionEEPROMData(void)
{
    status_t status;

    uint8_t initialRomBlock = INITIAL_ROM_BLOCK;
    uint32_t copy1Data = INITIAL_COPY1_DATA;
    uint32_t copy2Data = INITIAL_COPY2_DATA;

    uint32_t copy1CRC;
    uint32_t copy2CRC;

    //INTIALIZING ROM BLOCK VALUE
    status = EEPROM_WriteBytes(ROM_BLOCK_ADDR,&initialRomBlock,sizeof(initialRomBlock));
    if (status != STATUS_SUCCESS)
    {
        UART_Print("ERROR: ROM_BLOCK write failed\r\n");
        return status;
    }

    //INITIALIZING COPY1 DATA
    status = EEPROM_WriteBytes(COPY1_DATA_ADDR,(const uint8_t *)&copy1Data,sizeof(copy1Data));
    if (status != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 1 data write failed\r\n");
        return status;
    }

    /*
     * Deliberately store an incorrect CRC for Copy 1.
     * This creates the CRC failure needed for the test.
     */
    copy1CRC = CalculateCRC(INITIAL_COPY2_DATA);

    status = EEPROM_WriteBytes(COPY1_CRC_ADDR,(const uint8_t *)&copy1CRC,sizeof(copy1CRC));
    if (status != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 1 CRC write failed\r\n");
        return status;
    }

    //INITIALIZING COPY2 DATA
    status = EEPROM_WriteBytes(COPY2_DATA_ADDR,(const uint8_t *)&copy2Data,sizeof(copy2Data));
    if (status != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 2 data write failed\r\n");
        return status;
    }

    //INITIALIZING COPY2 CRC
    copy2CRC = CalculateCRC(copy2Data);
    status = EEPROM_WriteBytes(COPY2_CRC_ADDR,(const uint8_t *)&copy2CRC,sizeof(copy2CRC));
    if (status != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 2 CRC write failed\r\n");
        return status;
    }

    UART_Print("EEPROM provisioning completed\r\n");

    snprintf(
        txBuff,
        sizeof(txBuff),
        "Initialized ROM_BLOCK=%u, Copy1=%lu, Copy2=%lu\r\n",
        (unsigned int)initialRomBlock,
        (unsigned long)copy1Data,
        (unsigned long)copy2Data
    );

    UART_Print(txBuff);

    return STATUS_SUCCESS;
}


/* Read and validate the mirror copy (Copy 2) */
static status_t NvM_ReadMirrorCopy(uint32_t *mirrorData,uint32_t *mirrorCRC)
{
    uint32_t calculatedCRC;

    if ((mirrorData == NULL) || (mirrorCRC == NULL))
    {
        return STATUS_ERROR;
    }

    if (EEPROM_Read32(COPY2_DATA_ADDR, mirrorData) != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Mirror data read failed\r\n");
        return STATUS_ERROR;
    }

    if (EEPROM_Read32(COPY2_CRC_ADDR, mirrorCRC) != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Mirror CRC read failed\r\n");
        return STATUS_ERROR;
    }

    calculatedCRC = CalculateCRC(*mirrorData);

    snprintf(
        txBuff,
        sizeof(txBuff),
        "Copy 2: data=%lu stored CRC=0x%08lX calculated CRC=0x%08lX\r\n",
        (unsigned long)*mirrorData,
        (unsigned long)*mirrorCRC,
        (unsigned long)calculatedCRC
    );

    UART_Print(txBuff);

    if (calculatedCRC != *mirrorCRC)
    {
        UART_Print("Copy 2 CRC FAILED\r\n");
        return STATUS_ERROR;
    }

    UART_Print("Copy 2 CRC OK\r\n");

    return STATUS_SUCCESS;
}


/* Main redundant-read and fallback test */
static uint32_t RedundantReadTest(void)
{
    uint32_t copy1Data;
    uint32_t copy1CRC;

    uint32_t copy2Data;
    uint32_t copy2CRC;

    uint32_t calculatedCRC;

    /* Step 1: Read ROM_BLOCK from EEPROM */
    if (EEPROM_ReadBytes(ROM_BLOCK_ADDR,&ROM_BLOCK,sizeof(ROM_BLOCK)) != STATUS_SUCCESS)
    {
        UART_Print("ERROR: ROM_BLOCK read failed\r\n");
        return NVM_REQ_INTEGRITY_FAILED;
    }

    snprintf(txBuff,sizeof(txBuff),"ROM_BLOCK = %u\r\n",(unsigned int)ROM_BLOCK);

    UART_Print(txBuff);

    /* ROM_BLOCK must equal 1 for fallback recovery */
    if (ROM_BLOCK == 0U)
    {
        UART_Print("ROM_BLOCK is 0\r\n");
        return NVM_REQ_INTEGRITY_FAILED;
    }

    if (ROM_BLOCK != 1U)
    {
        UART_Print("ROM_BLOCK is invalid\r\n");
        return NVM_REQ_INTEGRITY_FAILED;
    }

    /* Step 2: Read Copy 1 data and its stored CRC */
    if (EEPROM_Read32(COPY1_DATA_ADDR, &copy1Data) != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 1 data read failed\r\n");
        return NVM_REQ_INTEGRITY_FAILED;
    }

    if (EEPROM_Read32(COPY1_CRC_ADDR, &copy1CRC) != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 1 CRC read failed\r\n");
        return NVM_REQ_INTEGRITY_FAILED;
    }

    /* Step 3: Verify Copy 1 CRC */
    calculatedCRC = CalculateCRC(copy1Data);

    snprintf(
        txBuff,
        sizeof(txBuff),
        "Copy 1: data=%lu stored CRC=0x%08lX calculated CRC=0x%08lX\r\n",
        (unsigned long)copy1Data,
        (unsigned long)copy1CRC,
        (unsigned long)calculatedCRC
    );

    UART_Print(txBuff);

    if (calculatedCRC == copy1CRC)
    {
        UART_Print("Copy 1 CRC OK\r\n");
        return NVM_REQ_OK;
    }

    UART_Print("Copy 1 CRC FAILED\r\n");
    UART_Print("Reading mirror copy...\r\n");

    /* Step 4: Read and validate Copy 2 */
    if (NvM_ReadMirrorCopy(&copy2Data, &copy2CRC) != STATUS_SUCCESS)
    {
        return NVM_REQ_INTEGRITY_FAILED;
    }

    /* Step 5: Restore Copy 1 data from Copy 2 */
    if (EEPROM_WriteBytes(COPY1_DATA_ADDR,(const uint8_t *)&copy2Data,sizeof(copy2Data)) != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 1 restoration failed\r\n");
        return NVM_REQ_INTEGRITY_FAILED;
    }

    /* Restore Copy 1 CRC */
    if (EEPROM_WriteBytes(COPY1_CRC_ADDR,(const uint8_t *)&copy2CRC,sizeof(copy2CRC)) != STATUS_SUCCESS)
    {
        UART_Print("ERROR: Copy 1 CRC restoration failed\r\n");
        return NVM_REQ_INTEGRITY_FAILED;
    }

    UART_Print("Copy 1 restored from Copy 2\r\n");

    return NVM_REQ_RESTORED_FROM_ROM;
}


int main(void)
{
    uint32_t result;

    CLOCK_DRV_Init(&clockMan1_InitConfig0);
    PINS_DRV_Init(NUM_OF_CONFIGURED_PINS0,g_pin_mux_InitConfigArr0);
    LPUART_DRV_Init(INST_LPUART_1,&lpUartState0,&lpuart_0_InitConfig0);
    LPI2C_DRV_MasterInit(INST_LPI2C0,&lpi2c0_MasterConfig0,&lpi2cState);
    CRC_DRV_Init(INST_CRC_1, &crc_1_Cfg0);

    UART_Print("\r\nExercise 15: Redundant Read with Fallback\r\n");

#if ENABLE_EEPROM_PROVISIONING
    if (ProvisionEEPROMData() != STATUS_SUCCESS)
    {
        UART_Print("EEPROM provisioning failed\r\n");
        while (1){}
    }
#endif

    /* Execute the existing EEPROM redundant-read test */
    result = RedundantReadTest();
    if (result == NVM_REQ_OK)                       UART_Print("NVM_REQ_OK\r\n");
    else if (result == NVM_REQ_RESTORED_FROM_ROM)   UART_Print("NVM_REQ_RESTORED_FROM_ROM\r\n");
    else                                            UART_Print("NVM_REQ_INTEGRITY_FAILED\r\n");
    while (1)
    {
    }
}
