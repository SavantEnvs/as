/*
 * Fuzz the XCP slave (calibration/measurement protocol, ASAM MCD-1 XCP on CAN):
 * Xcp_CanIfRxIndication (CanIf's callback for the XCP CTO PDU) -> Xcp_RxIndication (packet
 * queue) -> Xcp_MainFunction (command dispatch: STD/CAL/PGM/DAQ services, DAQ list
 * allocation and ODT setup, event and DAQ transmission).
 *
 * The XCP module is linked with the configuration upstream's app generates from
 * app/app/config/Xcp/Xcp.json (tools/generator/Xcp.py, run by mayhem/gen_cfg.py). Only the
 * environment is mocked: CanIf_Transmit (records the frame; busy on request), the critical
 * section and ECU reset hooks, and the
 * application callbacks of app/app/src/xcp_user.c, re-implemented deterministically (upstream's
 * seed mixes an uninitialised stack value and the wall clock into a static).
 * On Linux, upstream's Xcp_MtaRead/Xcp_MtaWrite do not touch memory, so SET_MTA/UPLOAD/DOWNLOAD
 * addresses from the input are safe.
 *
 * Input: a sequence of frames [ctl:1][data:(ctl & 0x0F)]
 *   ctl bits 0-3: frame length (0..15; XCP on CAN carries up to 8, longer is rejected by Xcp)
 *   ctl bits 4-6: number of extra Xcp_MainFunction ticks after the frame (0..7)
 *   ctl bit 7:    CanIf_Transmit reports busy while this frame is processed
 * Every input starts from a freshly initialised XCP module.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Std_Types.h"
#include "ComStack_Types.h"
#include "Std_Critical.h"
#include "Xcp.h"

extern const Xcp_ConfigType Xcp_Config;

/* ---------------- environment mocks: Det / log level / clock ---------------- */
Std_ReturnType Det_ReportError(uint16_t ModuleId, uint8_t InstanceId, uint8_t ApiId,
                               uint8_t ErrorId) {
  (void)ModuleId;
  (void)InstanceId;
  (void)ApiId;
  (void)ErrorId;
  return E_OK;
}
int std_get_log_level(void) {
  return 1000; /* quiet: no ASLOG output */
}
int std_get_as_log_level(const char *name, int dft) {
  (void)name;
  return dft;
}

imask_t Std_EnterCritical(void) {
  return 0;
}
void Std_ExitCritical(imask_t mask) {
  (void)mask;
}

/* ---------------- environment mocks: ECU reset (PROGRAM_RESET) ---------------- */
static uint32_t resetCount;
void Xcp_PerformReset(void) {
  resetCount++; /* the ECU would reboot here; the fuzz input just goes on */
}

/* ---------------- environment mocks: CanIf ---------------- */
static int canIfBusy;
/* last frame written to the bus (read by mayhem/tests/selftest_xcp.c) */
static uint32_t txCount;
static uint32_t txSum;
static uint8_t lastTxData[64];
static uint8_t lastTxLen;

Std_ReturnType CanIf_Transmit(PduIdType TxPduId, const PduInfoType *PduInfoPtr) {
  PduLengthType i;
  (void)TxPduId;
  if (canIfBusy) {
    return E_NOT_OK;
  }
  for (i = 0; i < PduInfoPtr->SduLength; i++) {
    txSum += PduInfoPtr->SduDataPtr[i]; /* the driver reads the whole frame */
  }
  lastTxLen = (uint8_t)((PduInfoPtr->SduLength > sizeof(lastTxData)) ? sizeof(lastTxData)
                                                                      : PduInfoPtr->SduLength);
  memcpy(lastTxData, PduInfoPtr->SduDataPtr, lastTxLen);
  txCount++;
  return E_OK;
}

/* ---------------- environment mocks: application callbacks (xcp_user.c) ---------------- */
#define XCP_SEED 0xa1b2c3d4u
#define XCP_KEY_MASK 0x78934673u /* key = seed ^ mask, as upstream's App_CompareKey */
Std_ReturnType App_XcpGetConnectPermission(uint8_t mode, Xcp_NegativeResponseCodeType *nrc) {
  if ((0x00 == mode) || (0x01 == mode)) {
    return E_OK;
  }
  *nrc = XCP_E_OUT_OF_RANGE;
  return E_NOT_OK;
}
Std_ReturnType App_XcpDisconnecNotification(void) {
  return E_OK;
}
Std_ReturnType App_XcpGetSeed(uint8_t mode, uint8_t resource, uint8_t *seed, uint16_t *seedLen,
                              Xcp_NegativeResponseCodeType *nrc) {
  (void)resource;
  if (0 != mode) {
    *nrc = XCP_E_OUT_OF_RANGE;
    return E_NOT_OK;
  }
  seed[0] = (uint8_t)(XCP_SEED >> 24);
  seed[1] = (uint8_t)(XCP_SEED >> 16);
  seed[2] = (uint8_t)(XCP_SEED >> 8);
  seed[3] = (uint8_t)XCP_SEED;
  *seedLen = 4;
  return E_OK;
}
Std_ReturnType App_CompareKey(uint8_t *key, uint16_t keyLen, Xcp_NegativeResponseCodeType *nrc) {
  uint32_t u32Key;
  if (4 != keyLen) {
    *nrc = XCP_E_OUT_OF_RANGE;
    return E_NOT_OK;
  }
  u32Key = ((uint32_t)key[0] << 24) + ((uint32_t)key[1] << 16) + ((uint32_t)key[2] << 8) +
           (uint32_t)key[3];
  if ((XCP_SEED ^ XCP_KEY_MASK) != u32Key) {
    *nrc = XCP_E_OUT_OF_RANGE;
    return E_NOT_OK;
  }
  return E_OK;
}
Std_ReturnType Xcp_GetProgramResetPermission(Xcp_OpStatusType opStatus,
                                             Xcp_NegativeResponseCodeType *nrc) {
  (void)opStatus;
  (void)nrc;
  return E_OK;
}
Std_ReturnType Xcp_GetProgramStartPermission(Xcp_OpStatusType opStatus,
                                             Xcp_NegativeResponseCodeType *nrc) {
  (void)opStatus;
  (void)nrc;
  return E_OK;
}

/* ---------------- driver ---------------- */
static void configure(void) {
  canIfBusy = 0;
  resetCount = 0;
  txCount = 0;
  txSum = 0;
  lastTxLen = 0;
  Xcp_Init(&Xcp_Config);
}

static void tick(void) {
  Xcp_MainFunction();
  Xcp_MainFunction_Write();
}

static void frame(const uint8_t *data, PduLengthType len) {
  PduInfoType info;
  info.SduDataPtr = (uint8_t *)data;
  info.MetaDataPtr = NULL;
  info.SduLength = len;
  Xcp_CanIfRxIndication(0, &info);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  size_t pos = 0;
  int i;
  configure();
  while (pos < size) {
    uint8_t ctl = data[pos++];
    size_t len = ctl & 0x0Fu;
    if (len > size - pos) {
      len = size - pos;
    }
    canIfBusy = (ctl & 0x80u) ? 1 : 0;
    frame(&data[pos], (PduLengthType)len);
    pos += len;
    for (i = 0; i <= ((ctl >> 4) & 0x07); i++) {
      tick();
    }
  }
  canIfBusy = 0;
  for (i = 0; i < 4; i++) {
    tick();
  }
  return 0;
}
