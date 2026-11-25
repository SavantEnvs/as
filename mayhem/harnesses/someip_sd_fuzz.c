/*
 * Fuzz the SOME/IP + Service Discovery path of the SSAS BSW stack: UDP/TCP payloads read by SoAd
 * (SoAd_MainFunction polls the TcpIp sockets) -> Sd (SD entries and options: find/offer/
 * subscribe) and SomeIp (header, request/response, SOME/IP-TP segmentation) -> SomeIpXf
 * deserialisation of the method arguments -> the server's method, with MainFunction ticks.
 *
 * SoAd, Sd, SomeIp, SomeIpXf, the generated Math skeleton and the network buffer pool are
 * configured by mayhem/build.sh with upstream's own generators (tools/generator/Net.py,
 * MemCluster.py) from upstream's SOME/IP example server (app/examples/someip/add/config/server),
 * as its SCons build does. The application callbacks are copied from that example
 * (app/examples/someip/add/add_server.c). The TcpIp socket layer is mocked (net_mock.h).
 *
 * Input: a sequence of chunks [sel:1][len:2 LE][data:len]. sel bit 0: 0 = UDP datagram to one of
 * the SD sockets (multicast / unicast, (sel >> 1)), 1 = bytes on a TCP connection to the Math
 * server ((sel >> 1) % 2 picks the peer; the configuration accepts one at a time); sel bit 7 on
 * a TCP chunk: the peer closes after it. Every input starts from a freshly initialised stack.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Std_Types.h"
#include "ComStack_Types.h"
#include "TcpIp.h"
#include "SoAd.h"
#include "Sd.h"
#include "SomeIp.h"
#include "SomeIpXf.h"
#include "Sd_Cfg.h"
#include "MathSkeleton.h"
#include "SomeIp_Cfg.h"
#include "SomeIpXf_Cfg.h"

#define NUM_TCP_CONN 2
#include "net_mock.h"

void SomeIp_MainFunction(void);

/* ---------------- application callbacks (as app/examples/someip/add/add_server.c) ---------- */
boolean Sd_ServerServiceMath_CRMC(PduIdType pduID, uint8_t type, uint16_t serviceID,
                                  uint16_t instanceID, uint8_t majorVersion, uint32_t minorVersion,
                                  const Sd_ConfigOptionStringType *receivedConfigOptionPtrArray,
                                  const Sd_ConfigOptionStringType *configuredConfigOptionPtrArray) {
  (void)pduID;
  (void)type;
  (void)serviceID;
  (void)instanceID;
  (void)majorVersion;
  (void)minorVersion;
  (void)receivedConfigOptionPtrArray;
  (void)configuredConfigOptionPtrArray;
  return TRUE;
}

void SomeIp_Math_OnConnect(uint16_t conId, boolean isConnected) {
  (void)conId;
  (void)isConnected;
}

static int mathCalls; /* read by mayhem/tests */
Std_ReturnType Math_add(const Vector_Type *A, const Vector_Type *B, Result_Type *returnResult) {
  Std_ReturnType ret = SOMEIP_E_PENDING;
  int32_t length, i;

  mathCalls++;
  if (A->numberLen != B->numberLen) {
    ret = E_NOT_OK;
  } else {
    length = A->numberLen;
    returnResult->ercd = E_OK;
    returnResult->numberLen = length;
    returnResult->summary = 0;
    for (i = 0; i < length; i++) {
      returnResult->number[i] = A->number[i] + B->number[i];
      returnResult->summary += (uint32_t)A->number[i] + B->number[i];
    }
  }
  return ret;
}

Std_ReturnType Math_add_Async(Result_Type *returnResult) {
  (void)returnResult;
  return E_OK;
}

/* ---------------- harness ---------------- */
static void tick(void) {
  SoAd_MainFunction();
  Sd_MainFunction();
  SomeIp_MainFunction();
}

static void configure(void) {
  int i;
  memset(socks, 0, sizeof(socks));
  memset(tcpConn, 0, sizeof(tcpConn));
  numPendingAccept = 0;
  lastTxLen = 0;
  mathCalls = 0;
  SoAd_Init(NULL);
  Sd_Init(NULL);
  SomeIp_Init(NULL);
  (void)Math_OfferService();
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
