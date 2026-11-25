/*
 * Fuzz the Ethernet diagnostic path of the SSAS BSW stack: TCP/UDP payloads read by SoAd
 * (SoAd_MainFunction polls the TcpIp sockets, splits DoIP messages with DoIP_HeaderIndication)
 * -> DoIP (header, vehicle identification, routing activation, alive check, diagnostic
 * messages) -> PduR/Dcm, with MainFunction ticks.
 *
 * SoAd, DoIP and the network buffer pool are configured by mayhem/build.sh with upstream's own
 * generators (tools/generator/Net.py, MemCluster.py) from the bootloader's configuration
 * (app/bootloader/config/Net/Network.json, Mempool.json), which is what upstream's SCons build
 * does. Mocked here: the TcpIp socket layer (in-memory sockets fed by the fuzzer), the Dcm
 * (PduR_DoIP* upper layer: buffers a request and sends it back), and the bootloader's DoIP user
 * callouts (copied from app/bootloader/src/doip.c).
 *
 * Input: a sequence of chunks [sel:1][len:2 LE][data:len]. sel bit 0: 0 = UDP datagram to the
 * DoIP UDP socket, 1 = bytes on a TCP tester connection ((sel >> 1) % 3, accepted on first use);
 * sel bit 7 on a TCP chunk: the peer closes that connection after the chunk (net_mock.h). After each chunk the
 * stack runs until the socket is drained. Every input starts from a freshly initialised stack.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Std_Types.h"
#include "ComStack_Types.h"
#include "TcpIp.h"
#include "SoAd.h"
#include "DoIP.h"
#include "Std_Critical.h"

extern const SoAd_ConfigType SoAd_Config;
extern const DoIP_ConfigType DoIP_Config;

#define NUM_TCP_CONN 3 /* DoIP tester connections in the bootloader configuration */
#include "net_mock.h"

/* ---------------- bootloader DoIP user callouts (as app/bootloader/src/doip.c) ---------------- */
Std_ReturnType DoIP_default_RoutingActivationAuthenticationCallback(
  boolean *Authentified, const uint8_t *AuthenticationReqData, uint8_t *AuthenticationResData) {
  (void)AuthenticationReqData;
  (void)AuthenticationResData;
  *Authentified = TRUE;
  return E_OK;
}
Std_ReturnType DoIP_default_RoutingActivationConfirmationCallback(
  boolean *Confirmed, const uint8_t *ConfirmationReqData, uint8_t *ConfirmationResData) {
  (void)ConfirmationReqData;
  (void)ConfirmationResData;
  *Confirmed = TRUE;
  return E_OK;
}
Std_ReturnType DoIP_CANBL_RoutingActivationAuthenticationCallback(
  boolean *Authentified, const uint8_t *AuthenticationReqData, uint8_t *AuthenticationResData) {
  (void)AuthenticationReqData;
  (void)AuthenticationResData;
  *Authentified = TRUE;
  return E_OK;
}
Std_ReturnType DoIP_CANBL_RoutingActivationConfirmationCallback(
  boolean *Confirmed, const uint8_t *ConfirmationReqData, uint8_t *ConfirmationResData) {
  (void)ConfirmationReqData;
  (void)ConfirmationResData;
  *Confirmed = TRUE;
  return E_OK;
}
Std_ReturnType Dcm_GetVin(uint8_t *Data) {
  memcpy(Data, "ASBL0000000000001", 17);
  return E_OK;
}
Std_ReturnType DoIP_UserGetEID(uint8_t *Data) {
  memcpy(Data, "ASBL01", 6);
  return E_OK;
}
Std_ReturnType DoIP_UserGetGID(uint8_t *Data) {
  memcpy(Data, "ASBL01", 6);
  return E_OK;
}
Std_ReturnType DoIP_UserGetRoutingActivationResponseOem(uint8_t *Data) {
  (void)Data;
  return E_NOT_OK; /* no OEM data */
}
Std_ReturnType DoIP_UserGetPowerModeStatus(uint8_t *PowerState) {
  *PowerState = 1;
  return E_OK;
}

/* ---------------- environment mocks: Dcm via PduR (receive + echo) ---------------- */
#define UL_BUF_SIZE 4096
static uint8_t ulRx[UL_BUF_SIZE];
static PduLengthType ulRxLen;
static PduIdType ulRxId;
static uint8_t ulTx[UL_BUF_SIZE];
static PduLengthType ulTxLen;
static PduLengthType ulTxPos;
static int ulEcho;
static int ulRxDone; /* read by mayhem/tests */

BufReq_ReturnType PduR_DoIPStartOfReception(PduIdType id, const PduInfoType *info,
                                            PduLengthType TpSduLength,
                                            PduLengthType *bufferSizePtr) {
  (void)info;
  if (TpSduLength > UL_BUF_SIZE) {
    return BUFREQ_E_OVFL;
  }
  ulRxId = id;
  ulRxLen = 0;
  *bufferSizePtr = UL_BUF_SIZE;
  return BUFREQ_OK;
}
BufReq_ReturnType PduR_DoIPCopyRxData(PduIdType id, const PduInfoType *info,
                                      PduLengthType *bufferSizePtr) {
  (void)id;
  if ((size_t)ulRxLen + info->SduLength > UL_BUF_SIZE) {
    return BUFREQ_E_OVFL;
  }
  if (info->SduLength > 0) {
    memcpy(&ulRx[ulRxLen], info->SduDataPtr, info->SduLength);
  }
  ulRxLen += info->SduLength;
  *bufferSizePtr = UL_BUF_SIZE - ulRxLen;
  return BUFREQ_OK;
}
void PduR_DoIPRxIndication(PduIdType id, Std_ReturnType result) {
  (void)id;
  if ((E_OK == result) && (ulRxLen > 0)) {
    memcpy(ulTx, ulRx, ulRxLen);
    ulTxLen = ulRxLen;
    ulEcho = 1;
    ulRxDone++;
  }
}
BufReq_ReturnType PduR_DoIPCopyTxData(PduIdType id, const PduInfoType *info,
                                      const RetryInfoType *retry, PduLengthType *availableDataPtr) {
  (void)id;
  (void)retry;
  if ((size_t)ulTxPos + info->SduLength > ulTxLen) {
    return BUFREQ_E_NOT_OK;
  }
  if (info->SduLength > 0) {
    memcpy(info->SduDataPtr, &ulTx[ulTxPos], info->SduLength);
  }
  ulTxPos += info->SduLength;
  *availableDataPtr = ulTxLen - ulTxPos;
  return BUFREQ_OK;
}
void PduR_DoIPTxConfirmation(PduIdType id, Std_ReturnType result) {
  (void)id;
  (void)result;
}

/* ---------------- harness ---------------- */
static void tick(void) {
  PduInfoType pdu;
  SoAd_MainFunction();
  DoIP_MainFunction();
  if (ulEcho) {
    ulEcho = 0;
    ulTxPos = 0;
    pdu.SduDataPtr = NULL;
    pdu.MetaDataPtr = NULL;
    pdu.SduLength = ulTxLen;
    /* the Dcm answers on the connection the request came from (same target node) */
    (void)DoIP_TpTransmit(ulRxId, &pdu);
  }
}

static void configure(void) {
  int i;
  memset(socks, 0, sizeof(socks));
  memset(tcpConn, 0, sizeof(tcpConn));
  numPendingAccept = 0;
  ulRxLen = 0;
  ulTxLen = 0;
  ulTxPos = 0;
  ulEcho = 0;
  lastTxLen = 0;
  SoAd_Init(&SoAd_Config);
  DoIP_Init(&DoIP_Config);
  DoIP_ActivationLineSwitchInactive(); /* DoIP keeps the activation line across inits */
  DoIP_ActivationLineSwitchActive();
  for (i = 0; i < 3; i++) {
    tick(); /* SoAd creates the sockets */
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  size_t pos = 0;
  int i;

  configure();
  while (pos + 3 <= size) {
    uint8_t sel = data[pos];
    uint32_t len = (uint32_t)data[pos + 1] | ((uint32_t)data[pos + 2] << 8);
    pos += 3;
    if (len > size - pos) {
      len = (uint32_t)(size - pos);
    }
    deliver(sel, &data[pos], len);
    pos += len;
  }
  for (i = 0; i < 50; i++) {
    tick();
  }
  return 0;
}
