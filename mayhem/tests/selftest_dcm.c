/*
 * Known-answer functional tests for the UDS server (Dcm + CanTp + PduR with the bootloader's
 * generated configuration), driven like the dcm_cantp_fuzz target. Prints PASS/FAIL per test.
 */
#include <stdio.h>
#include "../harnesses/dcm_cantp_fuzz.c"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

/* send one single-frame request on P2P; returns the number of frames sent back */
static uint32_t request(const uint8_t *d) {
  int i;
  uint32_t before = txCount;
  frame(0, d, 8);
  for (i = 0; i < 5; i++) {
    tick();
  }
  return txCount - before;
}

static int resp(uint8_t n, const uint8_t *expect) {
  return (lastTxLen == 8) && (lastTxData[0] == n) && (0 == memcmp(&lastTxData[1], expect, n));
}

int main(void) {
  static const uint8_t extended[8] = {0x02, 0x10, 0x03};
  static const uint8_t seedReq[8] = {0x02, 0x27, 0x01};
  static const uint8_t goodKey[8] = {0x06, 0x27, 0x02, 0xEE, 0xDD, 0xCC, 0xBB};
  static const uint8_t badKey[8] = {0x06, 0x27, 0x02, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t unknown[8] = {0x02, 0x99, 0x00};
  static const uint8_t seedInDefault[8] = {0x02, 0x27, 0x01};

  configure();
  check("dcm_session_control_extended",
        (1 == request(extended)) && resp(6, (const uint8_t *)"\x50\x03\x00\x32\x01\xF4"));
  check("dcm_security_seed", (1 == request(seedReq)) && resp(6, (const uint8_t *)"\x67\x01\x11\x22\x33\x44"));
  check("dcm_security_key_accepted", (1 == request(goodKey)) && resp(2, (const uint8_t *)"\x67\x02"));
  check("dcm_unknown_service_nrc", (1 == request(unknown)) && resp(3, (const uint8_t *)"\x7F\x99\x11"));

  configure();
  (void)request(extended);
  (void)request(seedReq);
  check("dcm_security_key_rejected",
        (1 == request(badKey)) && resp(3, (const uint8_t *)"\x7F\x27\x35"));

  /* security access is not available in the default session */
  configure();
  check("dcm_security_needs_session",
        (1 == request(seedInDefault)) && (lastTxData[1] == 0x7F) && (lastTxData[2] == 0x27));

  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
