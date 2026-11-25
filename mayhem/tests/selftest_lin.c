/*
 * Known-answer functional tests for the LIN slave stack (LinIf + LinTp with the bootloader's
 * generated configuration), driven like the linif_slave_fuzz target. Prints PASS/FAIL per test.
 */
#include <stdio.h>
#include "../harnesses/linif_slave_fuzz.c"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

int main(void) {
  static const uint8_t sf[8] = {0x01, 0x02, 0x3E, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t ff[8] = {0x01, 0x10, 0x08, 0x31, 0x01, 0xFF, 0x00, 0xAA};
  static const uint8_t cf[8] = {0x01, 0x21, 0xBB, 0xCC, 0xFF, 0xFF, 0xFF, 0xFF};
  Lin_PduType pdu;
  int i, before;

  /* a PID below every schedule entry is not ours */
  configure();
  memset(&pdu, 0, sizeof(pdu));
  pdu.Pid = 0x01;
  check("linif_unknown_pid_rejected", E_NOT_OK == LinIf_HeaderIndication(0, &pdu));

  /* the diagnostic request PID is received with the configured DLC */
  configure();
  memset(&pdu, 0, sizeof(pdu));
  pdu.Pid = 0x3C;
  check("linif_mrf_header_rx",
        (E_OK == LinIf_HeaderIndication(0, &pdu)) && (LIN_FRAMERESPONSE_RX == pdu.Drc) &&
          (8 == pdu.Dl));

  /* single-frame request -> the response goes out on the next slave response header */
  configure();
  before = ulRxDone;
  slot(0x3C, sf, 8);
  tick();
  tick();
  memset(lastTxFrame, 0, sizeof(lastTxFrame));
  slot(0x3D, NULL, 0);
  check("lintp_single_frame_echo", (ulRxDone == before + 1) && (ulRxLen == 2) &&
                                     (lastTxFrame[0] == 0x01) && (lastTxFrame[1] == 0x02) &&
                                     (lastTxFrame[2] == 0x3E) && (lastTxFrame[3] == 0x00));

  /* first frame + consecutive frame (no flow control on LIN) reassemble an 8-byte request */
  configure();
  before = ulRxDone;
  slot(0x3C, ff, 8);
  tick();
  slot(0x3C, cf, 8);
  tick();
  check("lintp_multi_frame_reassembly",
        (ulRxDone == before + 1) && (ulRxLen == 8) &&
          (0 == memcmp(ulRx, "\x31\x01\xFF\x00\xAA\xBB\xCC\xFF", 8)));

  /* the 8-byte echo starts with a first frame */
  for (i = 0; i < 2; i++) {
    tick();
  }
  memset(lastTxFrame, 0, sizeof(lastTxFrame));
  slot(0x3D, NULL, 0);
  check("lintp_multi_frame_echo_ff",
        (lastTxFrame[0] == 0x01) && (lastTxFrame[1] == 0x10) && (lastTxFrame[2] == 0x08) &&
          (lastTxFrame[3] == 0x31));

  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
