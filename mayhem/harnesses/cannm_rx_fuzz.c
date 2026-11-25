/*
 * Fuzz the CAN network management (AUTOSAR CanNm) receive path and state machine:
 * CanIf_RxIndication -> CanNm_RxIndication (NM PDUs from the bus: control bit vector, node id,
 * car wake-up and user data bytes) interleaved with the NM API an application drives (NetworkRequest,
 * NetworkRelease, PassiveStartUp, RepeatMessageRequest, Disable/EnableCommunication,
 * SetUserData, TxConfirmation) and CanNm_MainFunction ticks (repeat message, normal operation,
 * ready sleep, prepare bus sleep timers).
 *
 * CanNm is linked with the configuration upstream's app generates from
 * app/app/config/Com/CanNm.json (tools/generator/CanNm.py, run by mayhem/gen_cfg.py), without
 * USE_NM/USE_COM/USE_CANSM/USE_CANIF, as upstream's standalone CanNm build: the generated
 * CanNm_Cfg.c then supplies CanIf_RxIndication/CanIf_Transmit itself, and the timers count
 * main-function cycles (deterministic). Only the CAN driver (Can_Write, records the NM PDU) and
 * Nm_RemoteSleepCancellation are mocked.
 *
 * Input: a sequence of operations [op:1][args]
 *   op bits 0-2: 0 = rx NM PDU [canid:1][len:1 (mod 9)][data:len] on channel (op bit 3)
 *                1 = NetworkRequest, 2 = NetworkRelease, 3 = PassiveStartUp,
 *                4 = TxConfirmation (E_OK, or E_NOT_OK when op bit 3 is set; channel 0),
 *                5 = RepeatMessageRequest, 6 = Disable (bit 3 clear) / EnableCommunication,
 *                7 = SetUserData [data:6]
 *   op bit 3:    channel for ops 0-3, 5-7
 *   op bits 4-7: n, then n * n * 4 CanNm_MainFunction ticks (0..900, i.e. up to 9 s)
 * Every input starts from a freshly initialised CanNm.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Std_Types.h"
#include "ComStack_Types.h"
#include "Can_GeneralTypes.h"
#include "Can.h"
#include "CanIf_Can.h"
#include "CanNm.h"

extern const CanNm_ConfigType CanNm_Config;

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

/* ---------------- environment mocks: Nm (the one callback CanNm calls without USE_NM) ---------------- */
void Nm_RemoteSleepCancellation(NetworkHandleType nmNetworkHandle) {
  (void)nmNetworkHandle;
}

/* ---------------- environment mocks: CAN driver ---------------- */
/* NM PDUs written to the bus (read by mayhem/tests/selftest_cannm.c) */
static uint32_t txCount;
static uint32_t txSum;
static Can_IdType lastTxId;
static uint8_t lastTxData[64];
static uint8_t lastTxLen;

Std_ReturnType Can_Write(Can_HwHandleType Hth, const Can_PduType *PduInfo) {
  uint8_t i;
  (void)Hth;
  for (i = 0; i < PduInfo->length; i++) {
    txSum += PduInfo->sdu[i]; /* the driver reads the whole frame */
  }
  lastTxId = PduInfo->id;
  lastTxLen = (PduInfo->length > sizeof(lastTxData)) ? (uint8_t)sizeof(lastTxData) : PduInfo->length;
  memcpy(lastTxData, PduInfo->sdu, lastTxLen);
  txCount++;
  return E_OK;
}

/* ---------------- driver ---------------- */
static void configure(void) {
  txCount = 0;
  txSum = 0;
  lastTxLen = 0;
  CanNm_Init(&CanNm_Config);
}

static void tick(void) {
  CanNm_MainFunction();
}

/* deliver one NM PDU as the CAN driver does: its 8-byte frame buffer, the mailbox */
static void rx(NetworkHandleType ch, uint8_t canid, const uint8_t *data, uint8_t len) {
  uint8_t frame[8] = {0};
  Can_HwType mailbox;
  PduInfoType info;
  memcpy(frame, data, len);
  mailbox.CanId = 0x500u + canid;
  mailbox.Hoh = 0;
  mailbox.ControllerId = ch;
  info.SduDataPtr = frame;
  info.MetaDataPtr = (uint8_t *)&mailbox; /* CanIf passes the mailbox (CanIf.c) */
  info.SduLength = len;
  CanIf_RxIndication(&mailbox, &info);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  size_t pos = 0;
  int i, n;
  configure();
  while (pos < size) {
    uint8_t op = data[pos++];
    NetworkHandleType ch = (op & 0x08u) ? 1 : 0;
    switch (op & 0x07u) {
    case 0: {
      uint8_t canid, len;
      if (size - pos < 2) {
        pos = size;
        break;
      }
      canid = data[pos++];
      len = (uint8_t)(data[pos++] % 9u);
      if (len > size - pos) {
        len = (uint8_t)(size - pos);
      }
      rx(ch, canid, &data[pos], len);
      pos += len;
      break;
    }
    case 1:
      (void)CanNm_NetworkRequest(ch);
      break;
    case 2:
      (void)CanNm_NetworkRelease(ch);
      break;
    case 3:
      (void)CanNm_PassiveStartUp(ch);
      break;
    case 4:
      CanNm_TxConfirmation(0, (op & 0x08u) ? E_NOT_OK : E_OK);
      break;
    case 5:
      (void)CanNm_RepeatMessageRequest(ch);
      break;
    case 6:
      if (op & 0x08u) {
        (void)CanNm_EnableCommunication(0);
      } else {
        (void)CanNm_DisableCommunication(0);
      }
      break;
    default: {
      uint8_t user[6] = {0};
      size_t len = (size - pos < sizeof(user)) ? (size - pos) : sizeof(user);
      memcpy(user, &data[pos], len);
      pos += len;
      (void)CanNm_SetUserData(ch, user);
      break;
    }
    }
    n = op >> 4;
    for (i = 0; i < n * n * 4; i++) {
      tick();
    }
  }
  return 0;
}
