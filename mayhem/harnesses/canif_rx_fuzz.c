/*
 * Fuzz the CAN receive path of the SSAS BSW stack: CanIf_RxIndication (the CAN driver's
 * callback) -> CanIf rx-PDU lookup -> CanTp (ISO 15765-2) / J1939Tp (BAM/CMDT) reassembly
 * -> PduR -> upper layer, with MainFunction ticks between frames so the TP state machines
 * (flow control, timeouts, transmission of responses) run.
 *
 * The stack is linked with the checked-in configuration that upstream's own isotp PC tool uses
 * (tools/libraries/isotp/config, the *_Cfg.c files). Only the environment is mocked here: the CAN driver
 * (Can_Write & co.) and the isotp tool's upper-layer buffers (IsoTp_* callbacks).
 *
 * Input: byte 0 selects the channel parameters, then a sequence of frames
 *   [controller:1][canid:4 LE][dlc:1][data:dlc (<= 64)]
 * Each frame is delivered with CanIf_RxIndication, then one tick of every MainFunction.
 * Every input starts from a freshly re-configured and re-initialised stack.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Std_Types.h"
#include "ComStack_Types.h"
#include "Can.h"
#include "CanIf.h"
#include "CanIf_Can.h"
#include "CanIf_Cfg.h"
#include "CanTp.h"
#include "CanTp_Cfg.h"
#include "J1939Tp.h"
#include "J1939Tp_Cfg.h"
#include "PduR.h"

extern const CanIf_ConfigType CanIf_Config;
extern const CanTp_ConfigType CanTp_Config;
extern const J1939Tp_ConfigType J1939Tp_Config;
extern const PduR_ConfigType PduR_Config;
void CanTp_MainFunction_Fast(void);
void J1939Tp_MainFunctionFast(void);

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

/* ---------------- environment mocks: CAN driver ---------------- */
#define MAX_PENDING_TX 64
static PduIdType pendingTx[MAX_PENDING_TX];
static int numPendingTx;
static uint32_t txSum;
/* last frame written to the bus (read by mayhem/tests/selftest.c) */
static uint32_t txCount;
static Can_IdType lastTxId;
static uint8_t lastTxData[64];
static uint8_t lastTxLen;

void Can_ReConfig(uint8_t controller, const char *device, int port, uint32_t baudrate) {
  (void)controller;
  (void)device;
  (void)port;
  (void)baudrate;
}
Std_ReturnType Can_Write(Can_HwHandleType Hth, const Can_PduType *PduInfo) {
  uint8_t i;
  (void)Hth;
  if (numPendingTx >= MAX_PENDING_TX) {
    return CAN_BUSY;
  }
  for (i = 0; i < PduInfo->length; i++) {
    txSum += PduInfo->sdu[i]; /* the driver reads the whole frame */
  }
  txCount++;
  lastTxId = PduInfo->id;
  lastTxLen = PduInfo->length;
  memcpy(lastTxData, PduInfo->sdu, (PduInfo->length <= 64) ? PduInfo->length : 64);
  pendingTx[numPendingTx++] = PduInfo->swPduHandle;
  return E_OK;
}
Std_ReturnType Can_SetControllerMode(uint8_t Controller, Can_ControllerStateType Transition) {
  (void)Controller;
  (void)Transition;
  return E_OK;
}
Std_ReturnType Can_GetControllerMode(uint8_t Controller, Can_ControllerStateType *ControllerModePtr) {
  (void)Controller;
  *ControllerModePtr = CAN_CS_STARTED;
  return E_OK;
}

/* ---------------- environment mocks: isotp upper layer (receive + echo) ---------------- */
#define UL_BUF_SIZE 4096
typedef struct {
  uint8_t rx[UL_BUF_SIZE];
  PduLengthType rxLen;
  PduLengthType rxTotal;
  uint8_t tx[UL_BUF_SIZE];
  PduLengthType txLen;
  PduLengthType txPos;
  int echo; /* a message was received: send it back like a diagnostic response */
} UpperType;
static UpperType upperCanTp[2];
static UpperType upperJ1939[1];

static BufReq_ReturnType ul_start(UpperType *ul, size_t n, PduIdType id, const PduInfoType *info,
                                  PduLengthType total, PduLengthType *bufferSizePtr) {
  if (id >= n) {
    return BUFREQ_E_NOT_OK;
  }
  ul = &ul[id];
  if (total > UL_BUF_SIZE) {
    return BUFREQ_E_OVFL;
  }
  ul->rxLen = 0;
  ul->rxTotal = total;
  /* info (if any) only lets the upper layer peek at the first bytes; the data itself comes with
   * the following CopyRxData calls */
  (void)info;
  *bufferSizePtr = UL_BUF_SIZE - ul->rxLen;
  return BUFREQ_OK;
}
static BufReq_ReturnType ul_copyrx(UpperType *ul, size_t n, PduIdType id, const PduInfoType *info,
                                   PduLengthType *bufferSizePtr) {
  if (id >= n) {
    return BUFREQ_E_NOT_OK;
  }
  ul = &ul[id];
  if ((size_t)ul->rxLen + info->SduLength > UL_BUF_SIZE) {
    return BUFREQ_E_OVFL;
  }
  if (info->SduLength > 0) {
    memcpy(&ul->rx[ul->rxLen], info->SduDataPtr, info->SduLength);
  }
  ul->rxLen += info->SduLength;
  *bufferSizePtr = UL_BUF_SIZE - ul->rxLen;
  return BUFREQ_OK;
}
static void ul_rxind(UpperType *ul, size_t n, PduIdType id, Std_ReturnType result) {
  if (id >= n) {
    return;
  }
  ul = &ul[id];
  if ((E_OK == result) && (ul->rxLen > 0)) {
    memcpy(ul->tx, ul->rx, ul->rxLen);
    ul->txLen = ul->rxLen;
    ul->echo = 1;
  }
}
static BufReq_ReturnType ul_copytx(UpperType *ul, size_t n, PduIdType id, const PduInfoType *info,
                                   PduLengthType *availableDataPtr) {
  if (id >= n) {
    return BUFREQ_E_NOT_OK;
  }
  ul = &ul[id];
  if ((size_t)ul->txPos + info->SduLength > ul->txLen) {
    return BUFREQ_E_NOT_OK;
  }
  if (info->SduLength > 0) {
    memcpy(info->SduDataPtr, &ul->tx[ul->txPos], info->SduLength);
  }
  ul->txPos += info->SduLength;
  *availableDataPtr = ul->txLen - ul->txPos;
  return BUFREQ_OK;
}

BufReq_ReturnType IsoTp_CanTpStartOfReception(PduIdType id, const PduInfoType *info,
                                              PduLengthType TpSduLength,
                                              PduLengthType *bufferSizePtr) {
  return ul_start(upperCanTp, 2, id, info, TpSduLength, bufferSizePtr);
}
BufReq_ReturnType IsoTp_CanTpCopyRxData(PduIdType id, const PduInfoType *info,
                                        PduLengthType *bufferSizePtr) {
  return ul_copyrx(upperCanTp, 2, id, info, bufferSizePtr);
}
BufReq_ReturnType IsoTp_CanTpCopyTxData(PduIdType id, const PduInfoType *info,
                                        const RetryInfoType *retry,
                                        PduLengthType *availableDataPtr) {
  (void)retry;
  return ul_copytx(upperCanTp, 2, id, info, availableDataPtr);
}
void IsoTp_CanTpRxIndication(PduIdType id, Std_ReturnType result) {
  ul_rxind(upperCanTp, 2, id, result);
}
void IsoTp_CanTpTxConfirmation(PduIdType id, Std_ReturnType result) {
  (void)id;
  (void)result;
}
BufReq_ReturnType IsoTp_J1939TpStartOfReception(PduIdType id, const PduInfoType *info,
                                                PduLengthType TpSduLength,
                                                PduLengthType *bufferSizePtr) {
  return ul_start(upperJ1939, 1, id, info, TpSduLength, bufferSizePtr);
}
BufReq_ReturnType IsoTp_J1939TpCopyRxData(PduIdType id, const PduInfoType *info,
                                          PduLengthType *bufferSizePtr) {
  return ul_copyrx(upperJ1939, 1, id, info, bufferSizePtr);
}
BufReq_ReturnType IsoTp_J1939TpCopyTxData(PduIdType id, const PduInfoType *info,
                                          const RetryInfoType *retry,
                                          PduLengthType *availableDataPtr) {
  (void)retry;
  return ul_copytx(upperJ1939, 1, id, info, availableDataPtr);
}
void IsoTp_J1939TpRxIndication(PduIdType id, Std_ReturnType result) {
  ul_rxind(upperJ1939, 1, id, result);
}
void IsoTp_J1939TpTxConfirmation(PduIdType id, Std_ReturnType result) {
  (void)id;
  (void)result;
}

/* ---------------- harness ---------------- */
static void configure(uint8_t sel) {
  CanTp_ParamType cp;
  J1939Tp_ParamType jp;
  int i;

  memset(&cp, 0, sizeof(cp));
  cp.device = "fuzz";
  cp.baudrate = 500000;
  cp.RxCanId = 0x731;
  cp.TxCanId = 0x732;
  cp.N_TA = 0xFFFF; /* normal addressing */
  cp.LinkLayerDataLength = (sel & 0x01) ? 64 : 8;
  cp.STmin = 0;
  CanTp_ReConfig(0, &cp);

  cp.RxCanId = 0x18DA01F1;
  cp.TxCanId = 0x18DAF101;
  cp.N_TA = (sel & 0x02) ? 0x55 : 0xFFFF; /* extended or normal addressing */
  cp.LinkLayerDataLength = (sel & 0x04) ? 64 : 8;
  CanTp_ReConfig(1, &cp);

  memset(&jp, 0, sizeof(jp));
  jp.device = "fuzz";
  jp.baudrate = 250000;
  jp.protocol = (sel >> 3) & 0x01;
  jp.RX.CM = 0x18ECFF00;
  jp.RX.DT = 0x18EBFF00;
  jp.RX.Direct = 0x18EF0000;
  jp.RX.FC = 0x18EC00FF;
  jp.TX.CM = 0x18ECFF01;
  jp.TX.DT = 0x18EBFF01;
  jp.TX.Direct = 0x18EF0001;
  jp.TX.FC = 0x18EC01FF;
  jp.STMin = 0;
  jp.Tr = 200;
  jp.T1 = 750;
  jp.T2 = 1250;
  jp.T3 = 1250;
  jp.T4 = 1050;
  jp.TxMaxPacketsPerBlock = (sel >> 4) & 0x0F;
  jp.LinkLayerDataLength = (sel & 0x80) ? 64 : 8;
  J1939Tp_ReConfig(0, &jp);

  CanIf_Init(&CanIf_Config);
  for (i = 0; i < CANTP_MAX_CHANNELS + J1939TP_MAX_CHANNELS; i++) {
    (void)CanIf_SetPduMode((uint8_t)i, CANIF_ONLINE); /* what CanSM does once the bus is up */
  }
  PduR_Init(&PduR_Config);
  CanTp_Init(&CanTp_Config);
  J1939Tp_Init(&J1939Tp_Config);
  memset(upperCanTp, 0, sizeof(upperCanTp));
  memset(upperJ1939, 0, sizeof(upperJ1939));
  numPendingTx = 0;
}

static void tick(void) {
  int i, n;
  PduInfoType pdu;

  /* the CAN driver confirms what was written since the last tick */
  n = numPendingTx;
  numPendingTx = 0;
  for (i = 0; i < n; i++) {
    CanIf_TxConfirmation(pendingTx[i]);
  }
  CanIf_MainFunction_Fast();
  CanIf_MainFunction();
  CanTp_MainFunction_Fast();
  CanTp_MainFunction();
  J1939Tp_MainFunctionFast();
  J1939Tp_MainFunction();

  /* the upper layer answers a completed request with the same payload */
  for (i = 0; i < 2; i++) {
    if (upperCanTp[i].echo) {
      upperCanTp[i].echo = 0;
      upperCanTp[i].txPos = 0;
      pdu.SduDataPtr = NULL;
      pdu.MetaDataPtr = NULL;
      pdu.SduLength = upperCanTp[i].txLen;
      (void)CanTp_Transmit((PduIdType)i, &pdu);
    }
  }
  if (upperJ1939[0].echo) {
    upperJ1939[0].echo = 0;
    upperJ1939[0].txPos = 0;
    pdu.SduDataPtr = NULL;
    pdu.MetaDataPtr = NULL;
    pdu.SduLength = upperJ1939[0].txLen;
    (void)J1939Tp_Transmit(0, &pdu);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  uint8_t frame[64];
  Can_HwType mailbox;
  PduInfoType pdu;
  size_t pos = 1;
  int i;

  if (size < 1) {
    return -1;
  }
  configure(data[0]);

  while (pos + 6 <= size) {
    uint8_t ctrl = data[pos];
    uint32_t canid = (uint32_t)data[pos + 1] | ((uint32_t)data[pos + 2] << 8) |
                     ((uint32_t)data[pos + 3] << 16) | ((uint32_t)data[pos + 4] << 24);
    uint8_t dlc = data[pos + 5] % 65;
    pos += 6;
    if (pos + dlc > size) {
      dlc = (uint8_t)(size - pos);
    }
    memset(frame, 0, sizeof(frame));
    memcpy(frame, &data[pos], dlc);
    pos += dlc;

    mailbox.CanId = canid;
    mailbox.Hoh = ctrl;
    mailbox.ControllerId = ctrl;
    pdu.SduDataPtr = frame;
    pdu.MetaDataPtr = NULL;
    pdu.SduLength = dlc;
    CanIf_RxIndication(&mailbox, &pdu);
    tick();
  }
  /* let pending transfers time out */
  for (i = 0; i < 200; i++) {
    tick();
  }
  return 0;
}
