/*
 * Known-answer functional tests for SoAd + Sd + SomeIp + SomeIpXf with upstream's SOME/IP
 * example server configuration (Math service 0xADDA, method add 0x0ADD), driven like the
 * someip_sd_fuzz target. Prints PASS/FAIL per test.
 */
#include <stdio.h>
#include "../harnesses/someip_sd_fuzz.c"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

static uint8_t resp[256];
static uint32_t respLen;
/* run until the stack sends something on a TCP (accepted) socket */
static void run_tcp(int n) {
  int i;
  respLen = 0;
  if ((lastTxLen > 0) && (TCPIP_IPPROTO_TCP == socks[lastTxSock].proto)) {
    respLen = lastTxLen; /* sent while the request was being delivered */
    memcpy(resp, lastTx, lastTxLen < sizeof(resp) ? lastTxLen : sizeof(resp));
  }
  for (i = 0; i < n; i++) {
    lastTxLen = 0;
    tick();
    if ((lastTxLen > 0) && (TCPIP_IPPROTO_TCP == socks[lastTxSock].proto)) {
      respLen = lastTxLen;
      memcpy(resp, lastTx, lastTxLen < sizeof(resp) ? lastTxLen : sizeof(resp));
    }
  }
}

int main(void) {
  /* add([1,2,3,4], [10,20,30,40]) */
  static const uint8_t req[] = {0xAD, 0xDA, 0x0A, 0xDD, 0, 0, 0, 0x18, 0x00, 0x01, 0x00, 0x01,
                                0x01, 0x00, 0x00, 0x00, 0, 0, 0, 4, 1, 2, 3, 4,
                                0, 0, 0, 4, 10, 20, 30, 40};
  /* the same request for a method the service does not have */
  static const uint8_t badMethod[] = {0xAD, 0xDA, 0x0B, 0xBB, 0, 0, 0, 0x08,
                                      0x00, 0x01, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00};
  int i, offer = 0;

  /* the server offers Math 0xADDA/0x1001 on the SD multicast socket with its TCP endpoint */
  configure();
  for (i = 0; (i < 100) && !offer; i++) {
    lastTxLen = 0;
    tick();
    offer = (lastTxLen == 56) && (lastTx[0] == 0xFF) && (lastTx[1] == 0xFF) &&
            (lastTx[2] == 0x81) && (lastTx[3] == 0x00) && (lastTx[24] == 0x01) &&
            (lastTx[28] == 0xAD) && (lastTx[29] == 0xDA) && (lastTx[30] == 0x10) &&
            (lastTx[31] == 0x01) && (lastTx[48] == 192) && (lastTx[49] == 168) &&
            (lastTx[53] == 0x06) && (lastTx[54] == 0x77) && (lastTx[55] == 0xD8);
  }
  check("sd_offer_service", offer);

  /* a TCP request is decoded, Math_add runs, and the encoded result comes back */
  lastTxLen = 0;
  deliver(0x01, req, sizeof(req));
  run_tcp(20);
  check("someip_method_call", mathCalls == 1);
  check("someip_method_response",
        (respLen == 33) && (resp[0] == 0xAD) && (resp[1] == 0xDA) && (resp[2] == 0x0A) &&
          (resp[3] == 0xDD) && (resp[7] == 0x19) && (resp[11] == 0x01) &&
          (resp[14] == 0x80) /* RESPONSE */ && (resp[15] == 0x00) /* E_OK */ &&
          (resp[22] == 110) /* summary */ && (resp[24] == 4) && (resp[25] == 11) &&
          (resp[26] == 22) && (resp[27] == 33) && (resp[28] == 44));

  /* an unknown method does not reach the application */
  configure();
  lastTxLen = 0;
  deliver(0x01, badMethod, sizeof(badMethod));
  run_tcp(20);
  check("someip_unknown_method_not_dispatched", mathCalls == 0);

  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
