/*
 * Known-answer functional tests for the code the fuzz targets exercise (upstream has no unit
 * test suite). Built with normal flags by mayhem/build.sh, run by mayhem/test.sh.
 *  - srec/ihex: parse fixed S-record / Intel-HEX images and check blocks, addresses, bytes, range.
 *  - CanIf/CanTp/J1939Tp: drive frames through CanIf_RxIndication and check the frames the
 *    stack writes back (flow control, echo response).
 * Prints "PASS <name>" / "FAIL <name>" per test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../harnesses/canif_rx_fuzz.c"
#include "srec.h"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

static Srec_FileType *open_text(const char *name, const char *text, int ihex) {
  char path[256];
  FILE *fp;
  Srec_FileType *s;
  snprintf(path, sizeof(path), "%s/%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", name);
  fp = fopen(path, "w");
  if (!fp) {
    return NULL;
  }
  fputs(text, fp);
  fclose(fp);
  s = ihex ? IHex_Open(path) : Srec_Open(path);
  unlink(path);
  return s;
}

static void test_srec(void) {
  /* two S1 records forming one block at 0x1000 plus one S3 record at 0x20000000 */
  const char *s19 = "S00F000068656C6C6F202020202000003C\n"
                    "S107100001020304DE\n"
                    "S1071004AABBCCDDD6\n"
                    "S30920000000DEADBEEF9E\n"
                    "S9030000FC\n";
  Srec_FileType *s = open_text("selftest_a.s19", s19, 0);
  uint32_t len = 0, start;
  int ok = (NULL != s);
  if (ok) {
    ok = (s->NumOfBlks == 2) && (s->Blks[0].Address == 0x1000) && (s->Blks[0].Length == 8) &&
         (0 == memcmp(s->Blks[0].Data, "\x01\x02\x03\x04\xAA\xBB\xCC\xDD", 8)) &&
         (s->Blks[1].Address == 0x20000000) && (s->Blks[1].Length == 4) &&
         (0 == memcmp(s->Blks[1].Data, "\xDE\xAD\xBE\xEF", 4));
    start = Srec_Range(s, &len);
    ok = ok && (start == 0x1000) && (len == 0x20000004 - 0x1000);
    Srec_Close(s);
  }
  check("srec_s19_blocks", ok);

  /* a corrupted checksum stops the parse: the record after it is not loaded */
  s = open_text("selftest_b.s19", "S107100001020304DF\nS107200001020304CE\n", 0);
  ok = (NULL == s) || ((s->NumOfBlks == 1) && (s->Blks[0].Address == 0x1000));
  if (s) {
    Srec_Close(s);
  }
  check("srec_bad_checksum_rejected", ok);
}

static void test_ihex(void) {
  /* extended linear address 0x0800, 4 data bytes at 0x0010, EOF */
  const char *hex = ":020000040800F2\n"
                    ":040010001122334442\n"
                    ":00000001FF\n";
  Srec_FileType *s = open_text("selftest_c.hex", hex, 1);
  int ok = (NULL != s);
  if (ok) {
    ok = (s->NumOfBlks == 1) && (s->Blks[0].Address == 0x08000010) && (s->Blks[0].Length == 4) &&
         (0 == memcmp(s->Blks[0].Data, "\x11\x22\x33\x44", 4));
    Srec_Close(s);
  }
  check("ihex_extended_linear", ok);
}

static void rx(uint8_t ctrl, uint32_t id, const uint8_t *d, uint8_t len) {
  uint8_t buf[64];
  Can_HwType mb;
  PduInfoType p;
  memcpy(buf, d, len);
  mb.CanId = id;
  mb.Hoh = ctrl;
  mb.ControllerId = ctrl;
  p.SduDataPtr = buf;
  p.MetaDataPtr = NULL;
  p.SduLength = len;
  CanIf_RxIndication(&mb, &p);
}

static void test_cantp(void) {
  static const uint8_t sf[8] = {0x02, 0x3E, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55};
  static const uint8_t ff[8] = {0x10, 0x0A, 1, 2, 3, 4, 5, 6};
  static const uint8_t cf[8] = {0x21, 7, 8, 9, 10, 0x55, 0x55, 0x55};
  uint32_t before;
  int i, ok;

  configure(0);
  before = txCount;
  rx(0, 0x731, sf, 8);
  tick();
  tick();
  ok = (txCount > before) && (lastTxId == 0x732) && (lastTxLen == 8) && (lastTxData[0] == 0x02) &&
       (lastTxData[1] == 0x3E) && (lastTxData[2] == 0x00) && (upperCanTp[0].rxLen == 2);
  check("cantp_single_frame_echo", ok);

  /* a frame for an unknown CAN ID (below every configured one) is dropped */
  configure(0);
  before = txCount;
  rx(0, 0x001, sf, 8);
  for (i = 0; i < 5; i++) {
    tick();
  }
  check("canif_unknown_id_dropped", (txCount == before) && (upperCanTp[0].rxLen == 0));

  /* first frame -> flow control CTS, consecutive frame completes a 10-byte message */
  configure(0);
  rx(0, 0x731, ff, 8);
  tick();
  ok = (lastTxId == 0x732) && ((lastTxData[0] & 0xF0) == 0x30);
  rx(0, 0x731, cf, 8);
  tick();
  ok = ok && (upperCanTp[0].rxLen == 10) &&
       (0 == memcmp(upperCanTp[0].rx, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A", 10));
  check("cantp_multi_frame_reassembly", ok);

  /* the 10-byte echo goes out as a first frame */
  tick();
  ok = (lastTxId == 0x732) && (lastTxData[0] == 0x10) && (lastTxData[1] == 0x0A) &&
       (lastTxData[2] == 0x01);
  check("cantp_multi_frame_echo_ff", ok);
}

int main(void) {
  test_srec();
  test_ihex();
  test_cantp();
  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
