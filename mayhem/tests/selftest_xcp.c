/*
 * Known-answer functional tests for the XCP slave (Xcp with the app's generated configuration),
 * driven like the xcp_can_fuzz target. Prints PASS/FAIL per test.
 */
#include <stdio.h>
#include "../harnesses/xcp_can_fuzz.c"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

/* send one command; returns the number of frames sent back */
static uint32_t request(const uint8_t *d, PduLengthType len) {
  int i;
  uint32_t before = txCount;
  frame(d, len);
  for (i = 0; i < 5; i++) {
    tick();
  }
  return txCount - before;
}

static int resp(uint8_t n, const uint8_t *expect) {
  return (lastTxLen >= n) && (0 == memcmp(lastTxData, expect, n));
}

int main(void) {
  static const uint8_t connect[2] = {0xFF, 0x00};
  static const uint8_t status[1] = {0xFD};
  static const uint8_t getSeed[3] = {0xF8, 0x00, 0x01}; /* mode 0, resource CAL/PAG */
  static const uint8_t goodKey[6] = {0xF7, 0x04, 0xD9, 0x21, 0x85, 0xA7}; /* a1b2c3d4 ^ 78934673 */
  static const uint8_t badKey[6] = {0xF7, 0x04, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t unknown[1] = {0xC0};

  configure();
  /* before CONNECT every command is refused with ERR_GENERIC */
  check("xcp_status_before_connect_refused",
        (1 == request(status, 1)) && resp(2, (const uint8_t *)"\xFE\x31"));
  /* CONNECT: RESOURCE = CAL/PAG|DAQ|PGM, MAX_CTO 8, MAX_DTO 8, protocol/transport layer 1 */
  check("xcp_connect", (1 == request(connect, 2)) &&
                           resp(8, (const uint8_t *)"\xFF\x15\x00\x08\x00\x08\x01\x01"));
  /* GET_STATUS: CAL/PAG, DAQ, STIM and PGM protected */
  check("xcp_get_status_locked",
        (1 == request(status, 1)) && resp(6, (const uint8_t *)"\xFF\x00\x1D\x00\x00\x00"));
  /* GET_SEED for CAL/PAG returns the 4-byte seed; UNLOCK with seed ^ mask clears CAL/PAG */
  check("xcp_get_seed",
        (1 == request(getSeed, 3)) && resp(6, (const uint8_t *)"\xFF\x04\xA1\xB2\xC3\xD4"));
  check("xcp_unlock_accepted", (1 == request(goodKey, 6)) && resp(2, (const uint8_t *)"\xFF\x1C"));
  check("xcp_get_status_unlocked",
        (1 == request(status, 1)) && resp(6, (const uint8_t *)"\xFF\x00\x1C\x00\x00\x00"));
  check("xcp_unknown_command", (1 == request(unknown, 1)) && resp(2, (const uint8_t *)"\xFE\x20"));

  configure();
  (void)request(connect, 2);
  (void)request(getSeed, 3);
  check("xcp_unlock_rejected", (1 == request(badKey, 6)) && resp(2, (const uint8_t *)"\xFE\x22"));
  /* GET_SEED without the resource byte is a syntax error */
  check("xcp_get_seed_too_short",
        (1 == request(getSeed, 2)) && resp(2, (const uint8_t *)"\xFE\x21"));

  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
