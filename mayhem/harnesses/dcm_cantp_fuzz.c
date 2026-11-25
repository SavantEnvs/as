/*
 * Fuzz the UDS diagnostic server of the SSAS BSW stack as the bootloader configures it: CAN
 * frames for the diagnostic channels (physical P2P / functional P2A) delivered with
 * CanTp_RxIndication (what CanIf calls) -> CanTp reassembly -> PduR -> Dcm (session control,
 * security access, routine control, request download / transfer data / exit, DIDs, ECU reset,
 * communication control), with MainFunction ticks so responses go out and timers run.
 *
 * CanTp, PduR and Dcm are configured by mayhem/build.sh with upstream's own generators from the
 * bootloader configuration (app/bootloader/config/{CanTp,PduR,Dcm}), as upstream's SCons build
 * does. Mocked here: CanIf_Transmit (the bus; the frame is confirmed on the next tick) and the
 * bootloader application callouts BL_* (app/bootloader/src/bl_core.c needs the flash driver,
 * A/B partitions and memory map; this stand-in accepts every request with fixed data, a
 * fixed seed and key = ~seed, so the Dcm's own request parsing is what gets fuzzed).
 *
 * Input: a sequence of CAN frames [channel:1][dlc:1][data:dlc (<= 8)]; channel bit 0 picks
 * P2P or P2A. Each frame is delivered, then one tick. Every input starts from a fresh init.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Std_Types.h"
#include "ComStack_Types.h"
#include "CanIf.h"
#include "CanTp.h"
#include "CanTp_Cfg.h"
#include "PduR.h"
#include "Dcm.h"

extern const CanTp_ConfigType CanTp_Config;
extern const PduR_ConfigType PduR_Config;
extern const Dcm_ConfigType Dcm_Config;
void CanTp_MainFunction_Fast(void);

/* ---------------- environment mocks: Det / log level ---------------- */
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

/* ---------------- environment mocks: CanIf (the bus) ---------------- */
#define MAX_PENDING_TX 16
static PduIdType pendingTx[MAX_PENDING_TX];
static int numPendingTx;
static uint32_t txSum;
static uint32_t txCount; /* read by mayhem/tests */
static uint8_t lastTxData[64];
static PduLengthType lastTxLen;
static PduIdType lastTxPdu;

Std_ReturnType CanIf_Transmit(PduIdType TxPduId, const PduInfoType *PduInfoPtr) {
  PduLengthType i;
  if (numPendingTx >= MAX_PENDING_TX) {
    return E_NOT_OK;
  }
  for (i = 0; i < PduInfoPtr->SduLength; i++) {
    txSum += PduInfoPtr->SduDataPtr[i]; /* the driver reads the whole frame */
  }
  txCount++;
  lastTxPdu = TxPduId;
  lastTxLen = PduInfoPtr->SduLength;
  memcpy(lastTxData, PduInfoPtr->SduDataPtr,
         (PduInfoPtr->SduLength <= sizeof(lastTxData)) ? PduInfoPtr->SduLength
                                                        : sizeof(lastTxData));
  pendingTx[numPendingTx++] = TxPduId;
  return E_OK;
}

/* ---------------- bootloader application stand-in (BL_* callouts) ---------------- */
static const uint8_t blSeed[4] = {0x11, 0x22, 0x33, 0x44};
static int blResets; /* read by mayhem/tests */

static Std_ReturnType bl_seed(uint8_t *seed) {
  memcpy(seed, blSeed, sizeof(blSeed));
  return E_OK;
}
static Std_ReturnType bl_key(const uint8_t *key, Dcm_NegativeResponseCodeType *errorCode) {
  int i;
  for (i = 0; i < 4; i++) {
    if (key[i] != (uint8_t)~blSeed[i]) {
      *errorCode = DCM_E_INVALID_KEY;
      return E_NOT_OK;
    }
  }
  return E_OK;
}
Std_ReturnType BL_GetSessionChangePermission(Dcm_SesCtrlType sesCtrlTypeActive,
                                             Dcm_SesCtrlType sesCtrlTypeNew,
                                             Dcm_NegativeResponseCodeType *nrc) {
  (void)sesCtrlTypeActive;
  (void)sesCtrlTypeNew;
  (void)nrc;
  return E_OK;
}
Std_ReturnType BL_GetEcuResetPermission(Dcm_OpStatusType opStatus,
                                        Dcm_NegativeResponseCodeType *errorCode) {
  (void)opStatus;
  (void)errorCode;
  return E_OK;
}
Std_ReturnType BL_ReadFingerPrint(Dcm_OpStatusType opStatus, uint8_t *data, uint16_t length,
                                  Dcm_NegativeResponseCodeType *errorCode) {
  (void)opStatus;
  (void)errorCode;
  memset(data, 0x5A, length);
  return E_OK;
}
Std_ReturnType BL_WriteFingerPrint(Dcm_OpStatusType opStatus, uint8_t *data, uint16_t length,
                                   Dcm_NegativeResponseCodeType *errorCode) {
  uint16_t i;
  (void)opStatus;
  (void)errorCode;
  for (i = 0; i < length; i++) {
    txSum += data[i];
  }
  return E_OK;
}
Std_ReturnType BL_GetExtendedLevelSeed(uint8_t *seed, Dcm_NegativeResponseCodeType *errorCode) {
  (void)errorCode;
  return bl_seed(seed);
}
Std_ReturnType BL_CompareExtendedLevelKey(const uint8_t *key,
                                          Dcm_NegativeResponseCodeType *errorCode) {
  return bl_key(key, errorCode);
}
Std_ReturnType BL_GetProgramLevelSeed(uint8_t *seed, Dcm_NegativeResponseCodeType *errorCode) {
  (void)errorCode;
  return bl_seed(seed);
}
Std_ReturnType BL_CompareProgramLevelKey(const uint8_t *key,
                                         Dcm_NegativeResponseCodeType *errorCode) {
  return bl_key(key, errorCode);
}
Std_ReturnType BL_ComCtrlEnableRxAndTx(uint8_t comType, Dcm_NegativeResponseCodeType *errorCode) {
  (void)comType;
  (void)errorCode;
  return E_OK;
}
Std_ReturnType BL_ComCtrlDisableRxAndTx(uint8_t comType, Dcm_NegativeResponseCodeType *errorCode) {
  (void)comType;
  (void)errorCode;
  return E_OK;
}
static Std_ReturnType bl_routine(const uint8_t *dataIn, uint8_t *dataOut,
                                 uint16_t *currentDataLength) {
  (void)dataIn;
  (void)dataOut;
  *currentDataLength = 0;
  return E_OK;
}
Std_ReturnType BL_StartEraseFlash(const uint8_t *dataIn, Dcm_OpStatusType OpStatus,
                                  uint8_t *dataOut, uint16_t *currentDataLength,
                                  Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)errorCode;
  return bl_routine(dataIn, dataOut, currentDataLength);
}
Std_ReturnType BL_CheckProgrammingDependencies(const uint8_t *dataIn, Dcm_OpStatusType OpStatus,
                                               uint8_t *dataOut, uint16_t *currentDataLength,
                                               Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)errorCode;
  return bl_routine(dataIn, dataOut, currentDataLength);
}
Std_ReturnType BL_CheckIntegrity(const uint8_t *dataIn, Dcm_OpStatusType OpStatus,
                                 uint8_t *dataOut, uint16_t *currentDataLength,
                                 Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)errorCode;
  return bl_routine(dataIn, dataOut, currentDataLength);
}
Std_ReturnType BL_CheckProgrammingPreconditions(const uint8_t *dataIn, Dcm_OpStatusType OpStatus,
                                                uint8_t *dataOut, uint16_t *currentDataLength,
                                                Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)errorCode;
  return bl_routine(dataIn, dataOut, currentDataLength);
}
Std_ReturnType BL_CheckProgrammingIntegrity(const uint8_t *dataIn, Dcm_OpStatusType OpStatus,
                                            uint8_t *dataOut, uint16_t *currentDataLength,
                                            Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)errorCode;
  return bl_routine(dataIn, dataOut, currentDataLength);
}
Std_ReturnType BL_ProcessRequestDownload(Dcm_OpStatusType OpStatus, uint8_t DataFormatIdentifier,
                                         uint8_t MemoryIdentifier, uint32_t MemoryAddress,
                                         uint32_t MemorySize, uint32_t *BlockLength,
                                         Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)MemoryIdentifier;
  (void)MemoryAddress;
  (void)MemorySize;
  (void)BlockLength;
  if (0x00 != DataFormatIdentifier) { /* as bl_core.c: only uncompressed, unencrypted */
    *errorCode = DCM_E_REQUEST_OUT_OF_RANGE;
    return E_NOT_OK;
  }
  return E_OK;
}
Dcm_ReturnWriteMemoryType BL_ProcessTransferDataWrite(Dcm_OpStatusType OpStatus,
                                                      uint8_t MemoryIdentifier,
                                                      uint32_t MemoryAddress, uint32_t MemorySize,
                                                      const Dcm_RequestDataArrayType MemoryData,
                                                      Dcm_NegativeResponseCodeType *errorCode) {
  uint32_t i;
  (void)OpStatus;
  (void)MemoryIdentifier;
  (void)MemoryAddress;
  (void)errorCode;
  for (i = 0; i < MemorySize; i++) {
    txSum += MemoryData[i]; /* the flash driver reads all the data it is given */
  }
  return DCM_WRITE_OK;
}
Dcm_ReturnReadMemoryType BL_ProcessTransferDataRead(Dcm_OpStatusType OpStatus,
                                                    uint8_t MemoryIdentifier,
                                                    uint32_t MemoryAddress, uint32_t MemorySize,
                                                    Dcm_RequestDataArrayType MemoryData,
                                                    Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)MemoryIdentifier;
  (void)MemoryAddress;
  (void)errorCode;
  memset(MemoryData, 0xA5, MemorySize); /* the flash driver fills what it is asked for */
  return DCM_READ_OK;
}
Std_ReturnType BL_ProcessRequestTransferExit(Dcm_OpStatusType OpStatus,
                                             Dcm_NegativeResponseCodeType *errorCode) {
  (void)OpStatus;
  (void)errorCode;
  return E_OK;
}
void Dcm_PerformReset(uint8_t resetType) {
  (void)resetType;
  blResets++;
}
void Dcm_SessionChangeIndication(Dcm_SesCtrlType sesCtrlTypeActive, Dcm_SesCtrlType sesCtrlTypeNew,
                                 boolean timeout) {
  (void)sesCtrlTypeActive;
  (void)sesCtrlTypeNew;
  (void)timeout;
}

/* ---------------- harness ---------------- */
static void tick(void) {
  int i, n = numPendingTx;
  numPendingTx = 0;
  for (i = 0; i < n; i++) {
    CanTp_TxConfirmation(pendingTx[i], E_OK);
  }
  CanTp_MainFunction_Fast();
  CanTp_MainFunction();
  Dcm_MainFunction();
}

static void configure(void) {
  numPendingTx = 0;
  blResets = 0;
  PduR_Init(&PduR_Config);
  CanTp_Init(&CanTp_Config);
  Dcm_Init(&Dcm_Config);
}

static void frame(uint8_t ch, const uint8_t *data, uint8_t len) {
  uint8_t buf[8];
  PduInfoType pdu;
  memset(buf, 0, sizeof(buf));
  memcpy(buf, data, len);
  pdu.SduDataPtr = buf;
  pdu.MetaDataPtr = NULL;
  pdu.SduLength = len;
  CanTp_RxIndication((ch & 0x01) ? CANTP_P2A_RX : CANTP_P2P_RX, &pdu);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  size_t pos = 0;
  int i;

  configure();
  while (pos + 2 <= size) {
    uint8_t ch = data[pos];
    uint8_t len = data[pos + 1] % 9;
    pos += 2;
    if (pos + len > size) {
      len = (uint8_t)(size - pos);
    }
    frame(ch, &data[pos], len);
    pos += len;
    tick();
  }
  for (i = 0; i < 100; i++) {
    tick();
  }
  return 0;
}
