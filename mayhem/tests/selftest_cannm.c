/*
 * Known-answer functional tests for CanNm (the app's generated configuration), driven like the
 * cannm_rx_fuzz target. Prints PASS/FAIL per test.
 */
#include <stdio.h>
#include "../harnesses/cannm_rx_fuzz.c"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

static void ticks(int n) {
  int i;
  for (i = 0; i < n; i++) {
    tick();
  }
}

static Nm_StateType state(NetworkHandleType ch) {
  Nm_StateType st = NM_STATE_UNINIT;
  Nm_ModeType mode;
  (void)CanNm_GetState(ch, &st, &mode);
  return st;
}

static int pdu(const uint8_t *expect) {
  return (8 == lastTxLen) && (0x500u == lastTxId) && (0 == memcmp(lastTxData, expect, 8));
}

int main(void) {
  static const uint8_t remote[8] = {0x33, 0x00, 0x01, 0, 0, 0, 0, 0};
  static const uint8_t repeatPdu[8] = {0x00, 0x11, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t normalPdu[8] = {0x00, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint32_t n;

  configure();
  check("cannm_init_bus_sleep", NM_STATE_BUS_SLEEP == state(0));
  /* NetworkRequest: Repeat Message state, NM PDU NID 0 with CBV repeat-message|active-wakeup */
  (void)CanNm_NetworkRequest(0);
  ticks(5);
  check("cannm_request_repeat_message",
        (NM_STATE_REPEAT_MESSAGE == state(0)) && (1 == txCount) && pdu(repeatPdu));
  /* after RepeatMessageTime (1050 ms): Normal Operation, 10 immediate + cyclic PDUs */
  ticks(200);
  check("cannm_normal_operation",
        (NM_STATE_NORMAL_OPERATION == state(0)) && (12 == txCount) && pdu(normalPdu));
  /* NetworkRelease: no more PDUs; NM timeout (2 s) -> Prepare Bus Sleep -> Bus Sleep (5 s) */
  (void)CanNm_NetworkRelease(0);
  ticks(300);
  check("cannm_release_prepare_bus_sleep", (NM_STATE_PREPARE_BUS_SLEEP == state(0)) && (12 == txCount));
  ticks(600);
  check("cannm_bus_sleep_again", NM_STATE_BUS_SLEEP == state(0));

  /* a remote NM PDU in Bus Sleep does not wake the node up by itself */
  configure();
  rx(1, 0x33, remote, 8);
  ticks(2);
  check("cannm_rx_in_bus_sleep", (NM_STATE_BUS_SLEEP == state(1)) && (0 == txCount));

  /* remote NM PDUs restart the NM timeout: Ready Sleep is held while they keep coming */
  configure();
  (void)CanNm_NetworkRequest(0);
  ticks(205);
  (void)CanNm_NetworkRelease(0);
  for (n = 0; n < 5; n++) {
    ticks(100);
    rx(0, 0x33, remote, 8);
  }
  check("cannm_rx_keeps_ready_sleep", NM_STATE_READY_SLEEP == state(0));
  ticks(250);
  check("cannm_timeout_after_last_rx", NM_STATE_PREPARE_BUS_SLEEP == state(0));

  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
