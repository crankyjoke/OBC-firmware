#include "bl_config.h"
#include "bl_flash.h"
#include "bl_uart.h"
#include "bl_errors.h"
#include <metadata_struct.h>
#include <stdio.h>
#include <string.h>
/* LINKER EXPORTED SYMBOLS */
extern uint32_t __ramFuncsLoadStart__;
extern uint32_t __ramFuncsSize__;

extern uint32_t __ramFuncsRunStart__;
extern uint32_t __ramFuncsRunEnd__;

/* DEFINES */
// These values were chosen so that the UART transfers and flash writes are quick, but don't
// use too much RAM
#define BL_BIN_RX_CHUNK_SIZE 128U   // Bytes
#define BL_ECC_FIX_CHUNK_SIZE 128U  // Bytes
#define BL_MAX_MSG_SIZE 64U
#define RM46_FLASH_BANK 0U
#define LAST_SECTOR_START_ADDR blFlashSectorStartAddr(15U)

/* TYPEDEFS */
typedef void (*appStartFunc_t)(void);

typedef enum {
  BL_STATE_IDLE,
  BL_STATE_DOWNLOAD_IMAGE,
  BL_STATE_ERASE_IMAGE,
  BL_STATE_RUN_APP,
} bl_state_t;

/* PUBLIC FUNCTIONS */
int main(void) {
  bl_error_code_t errCode = BL_ERR_CODE_SUCCESS;

  blUartInit();

  // F021 API and the functions that use it must be executed from RAM since they
  // can't execute from the same flash bank being modified
  memcpy(&__ramFuncsRunStart__, &__ramFuncsLoadStart__, (uint32_t)&__ramFuncsSize__);

  bl_state_t state = BL_STATE_IDLE;

  // TODO: Modify the transfer protocol to be faster and more robust
  while (1) {
    switch (state) {
      case BL_STATE_IDLE: {
        blUartWriteBytes(strlen("Waiting for input\r\n"), (uint8_t *)"Waiting for input\r\n");

        char c = '\0';
        blUartReadBytes((uint8_t *)&c, 1);

        if (c == 'd') {
          state = BL_STATE_DOWNLOAD_IMAGE;
        } else if (c == 'e') {
          state = BL_STATE_ERASE_IMAGE;
        } else if (c == 'r') {
          state = BL_STATE_RUN_APP;
        }

        break;
      }
      case BL_STATE_DOWNLOAD_IMAGE: {
        blUartWriteBytes(strlen("Downloading application\r\n"), (uint8_t *)"Downloading application\r\n");

        uint8_t recvBuffer[sizeof(app_metadata_t)] = {0U};

        blUartReadBytes(recvBuffer, sizeof(app_metadata_t));

        app_metadata_t appHeader = {0};
        memcpy((void *)&appHeader, (void *)recvBuffer, sizeof(app_metadata_t));

        if (appHeader.binSize == 0U) {
          blUartWriteBytes(strlen("Invalid image size\r\n"), (uint8_t *)"Invalid image size\r\n");
          state = BL_STATE_IDLE;
          break;
        }

        if (!blFlashIsStartAddrValid(APP_START_ADDRESS, appHeader.binSize)) {
          blUartWriteBytes(strlen("Invalid start address\r\n"), (uint8_t *)"Invalid start address\r\n");
          state = BL_STATE_IDLE;
          break;
        }

        blUartWriteBytes(strlen("Received header\r\n"), (uint8_t *)"Received header\r\n");

        errCode = blFlashFapiInitBank(RM46_FLASH_BANK);
        if (errCode != BL_ERR_CODE_SUCCESS) {
          char blUartWriteBuffer[BL_MAX_MSG_SIZE] = {0};
          int32_t blUartWriteBufferLen =
              snprintf(blUartWriteBuffer, BL_MAX_MSG_SIZE, "Failed to init flash, error code: %d\r\n", errCode);
          if (blUartWriteBufferLen < 0) {
            blUartWriteBytes(strlen("Error with processing message buffer length\r\n"),
                             (uint8_t *)"Error with processing message buffer length\r\n");
          } else {
            blUartWriteBytes(blUartWriteBufferLen, (uint8_t *)blUartWriteBuffer);
          }
          state = BL_STATE_IDLE;
          break;
        }

        errCode = blFlashFapiBlockErase(APP_START_ADDRESS, appHeader.binSize);
        if (errCode != BL_ERR_CODE_SUCCESS) {
          char blUartWriteBuffer[BL_MAX_MSG_SIZE] = {0};
          int32_t blUartWriteBufferLen =
              snprintf(blUartWriteBuffer, BL_MAX_MSG_SIZE, "Failed to erase flash, error code: %d\r\n", errCode);
          if (blUartWriteBufferLen < 0) {
            blUartWriteBytes(strlen("Error with processing message buffer length\r\n"),
                             (uint8_t *)"Error with processing message buffer length\r\n");
          } else {
            blUartWriteBytes(blUartWriteBufferLen, (uint8_t *)blUartWriteBuffer);
          }
          state = BL_STATE_IDLE;
          break;
        }

        blUartWriteBytes(strlen("Erased flash\r\n"), (uint8_t *)"Erased flash\r\n");

        // Host will send a 'D' before sending the image
        while (1) {
          char waitChar = '\0';

          blUartReadBytes((uint8_t *)&waitChar, 1U);

          if (waitChar == 'D') {
            break;
          }
        }

        blFlashFapiInitBank(RM46_FLASH_BANK);

        // Receive image in chunks and write to flash
        uint32_t numAppBytesToFlash = appHeader.binSize;
        while (numAppBytesToFlash > 0) {
          uint8_t recvBuffer[BL_BIN_RX_CHUNK_SIZE] = {0U};

          uint32_t numBytesToRead =
              (numAppBytesToFlash > BL_BIN_RX_CHUNK_SIZE) ? BL_BIN_RX_CHUNK_SIZE : numAppBytesToFlash;

          blUartReadBytes(recvBuffer, numBytesToRead);

          blFlashFapiBlockWrite(APP_START_ADDRESS + (appHeader.binSize - numAppBytesToFlash), (uint32_t)recvBuffer,
                                numBytesToRead);

          numAppBytesToFlash -= numBytesToRead;
        }

        blUartWriteBytes(strlen("Wrote application\r\n"), (uint8_t *)"Wrote application\r\n");

        blFlashFapiInitBank(RM46_FLASH_BANK);

        // Check if last sector (where metadata is located) was already erased when erasing sectors for the app
        // bin, if not, erase it
        if (APP_START_ADDRESS + appHeader.binSize < LAST_SECTOR_START_ADDR) {
          errCode = blFlashFapiBlockErase(METADATA_START_ADDRESS, METADATA_SIZE_BYTES - 1);
          if (errCode != BL_ERR_CODE_SUCCESS) {
            char blUartWriteBuffer[BL_MAX_MSG_SIZE] = {0};
            int32_t blUartWriteBufferLen =
                snprintf(blUartWriteBuffer, BL_MAX_MSG_SIZE, "Failed to erase flash, error code: %d\r\n", errCode);
            if (blUartWriteBufferLen < 0) {
              blUartWriteBytes(strlen("Error with processing message buffer length\r\n"),
                               (uint8_t *)"Error with processing message buffer length\r\n");
            } else {
              blUartWriteBytes(blUartWriteBufferLen, (uint8_t *)blUartWriteBuffer);
            }
            state = BL_STATE_IDLE;
            break;
          }
          blUartWriteBytes(strlen("Erased last flash sector\r\n"), (uint8_t *)"Erased last flash sector\r\n");
        }

        blFlashFapiInitBank(RM46_FLASH_BANK);

        bl_error_code_t errCode =
            blFlashFapiBlockWrite(METADATA_START_ADDRESS, (uint32_t)&appHeader, sizeof(app_metadata_t));
        if (errCode != BL_ERR_CODE_SUCCESS) {
          char blUartWriteBuffer[BL_MAX_MSG_SIZE] = {0};
          int32_t blUartWriteBufferLen = snprintf(blUartWriteBuffer, BL_MAX_MSG_SIZE,
                                                  "Failed to write metadata to flash, error code: %d \r\n", errCode);
          if (blUartWriteBufferLen < 0) {
            blUartWriteBytes(strlen("Error with processing message buffer length\r\n"),
                             (uint8_t *)"Error with processing message buffer length\r\n");
          } else {
            blUartWriteBytes(blUartWriteBufferLen, (uint8_t *)blUartWriteBuffer);
          }
          state = BL_STATE_IDLE;
          break;
        }
        blUartWriteBytes(strlen("Wrote metadata \r\n"), ((uint8_t *)"Wrote metadata \r\n"));

        blUartWriteBytes(strlen("Fixing ECC\r\n"), (uint8_t *)"Fixing ECC\r\n");

        // Fix the ECC for any flash memory that was erased, but not overwritten by the new app
        uint8_t eccFixWriteBuf[BL_ECC_FIX_CHUNK_SIZE] = {0U};
        memset(eccFixWriteBuf, 0xFFU, sizeof(eccFixWriteBuf));  // Erased flash defaults to 0xFF

        const uint32_t eccFixTotalBytes =
            blFlashSectorEndAddr(blFlashSectorOfAddr(APP_START_ADDRESS + appHeader.binSize)) -
            (APP_START_ADDRESS + appHeader.binSize);

        uint32_t eccFixBytesLeft = eccFixTotalBytes;
        while (eccFixBytesLeft > 0) {
          const uint32_t numBytesToWrite =
              (eccFixBytesLeft > BL_ECC_FIX_CHUNK_SIZE) ? BL_ECC_FIX_CHUNK_SIZE : eccFixBytesLeft;

          const uint32_t baseAddr = APP_START_ADDRESS + appHeader.binSize + 1U;
          const uint32_t addr = baseAddr + (eccFixTotalBytes - eccFixBytesLeft);

          blFlashFapiBlockWrite(addr, (uint32_t)eccFixWriteBuf, numBytesToWrite);

          eccFixBytesLeft -= numBytesToWrite;
        }

        blUartWriteBytes(strlen("Finished writing to flash\r\n"), (uint8_t *)"Finished writing to flash\r\n");

        state = BL_STATE_IDLE;
        break;
      }
      case BL_STATE_ERASE_IMAGE: {
        blUartWriteBytes(strlen("NOT IMPLEMENTED\r\n"), (uint8_t *)"NOT IMPLEMENTED\r\n");

        // TODO: Erase entire application space

        state = BL_STATE_IDLE;
        break;
      }
      case BL_STATE_RUN_APP: {
        blUartWriteBytes(strlen("Running application\r\n"), (uint8_t *)"Running application\r\n");

        // Go to the application's entry point
        uint32_t appStartAddress = (uint32_t)APP_START_ADDRESS;
        ((appStartFunc_t)appStartAddress)();

        blUartWriteBytes(strlen("Failed to run application\r\n"), (uint8_t *)"Failed to run application\r\n");

        // TODO: Restart device if application fails to run or returns

        break;
      }
    }
  }
}
