/*
 * Known-answer functional tests for SoAd + DoIP with the bootloader's generated configuration,
 * driven like the doip_soad_fuzz target. Prints PASS/FAIL per test.
 */
#include <stdio.h>
#include "../harnesses/doip_soad_fuzz.c"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

/* configure, then let the 3 vehicle announcements go out so they don't mask the responses */
static void setup(void) {
  int i;
  configure();
  for (i = 0; i < 100; i++) {
    tick();
  }
  lastTxLen = 0;
}

int main(void) {
  static const uint8_t vidReq[] = {0x02, 0xFD, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t badHdr[] = {0x03, 0xFD, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t raReq[] = {0x02, 0xFD, 0x00, 0x05, 0x00, 0x00, 0x00, 0x07,
                                  0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t diagReq[] = {0x02, 0xFD, 0x80, 0x01, 0x00, 0x00, 0x00, 0x06,
                                    0xBE, 0xEF, 0xDE, 0xAD, 0x3E, 0x00};
  int i, before;

  /* UDP vehicle identification request -> vehicle announcement with the VIN */
  setup();
  deliver(0x00, vidReq, sizeof(vidReq));
  check("doip_udp_vehicle_identification",
        (lastTxLen == 8 + 33) && (lastTx[0] == 0x02) && (lastTx[1] == 0xFD) &&
          (lastTx[2] == 0x00) && (lastTx[3] == 0x04) &&
          (0 == memcmp(&lastTx[8], "ASBL0000000000001", 17)));

  /* a wrong protocol version is answered with a generic header NACK */
  setup();
  deliver(0x00, badHdr, sizeof(badHdr));
  check("doip_bad_header_nack", (lastTxLen == 9) && (lastTx[2] == 0x00) && (lastTx[3] == 0x00) &&
                                  (lastTx[8] == 0x00));

  /* TCP routing activation for tester 0xBEEF -> success (0x10) */
  setup();
  deliver(0x01, raReq, sizeof(raReq));
  check("doip_tcp_routing_activation", (lastTxLen >= 13) && (lastTx[2] == 0x00) &&
                                         (lastTx[3] == 0x06) && (lastTx[8] == 0xBE) &&
                                         (lastTx[9] == 0xEF) && (lastTx[12] == 0x10));

  /* diagnostic message to 0xDEAD -> positive ACK, Dcm gets the UDS bytes, echo comes back */
  before = ulRxDone;
  deliver(0x01, diagReq, sizeof(diagReq));
  check("doip_diag_message_delivered",
        (ulRxDone == before + 1) && (ulRxLen == 2) && (ulRx[0] == 0x3E) && (ulRx[1] == 0x00));
  for (i = 0; i < 5; i++) {
    tick();
  }
  check("doip_diag_message_response",
        (lastTxLen == 14) && (lastTx[2] == 0x80) && (lastTx[3] == 0x01) && (lastTx[8] == 0xDE) &&
          (lastTx[9] == 0xAD) && (lastTx[10] == 0xBE) && (lastTx[11] == 0xEF) &&
          (lastTx[12] == 0x3E) && (lastTx[13] == 0x00));

  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
