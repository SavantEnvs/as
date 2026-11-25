/*
 * Known-answer functional test for the ODX-F reader (tools/libraries/odx). Prints PASS/FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "odx.h"

static int passed, failed;
static void check(const char *name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    passed++;
  } else {
    failed++;
  }
}

static Odx_Type *open_text(const char *name, const char *text) {
  char path[256];
  FILE *fp;
  Odx_Type *odx;
  snprintf(path, sizeof(path), "%s/%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", name);
  fp = fopen(path, "w");
  if (!fp) {
    return NULL;
  }
  fputs(text, fp);
  fclose(fp);
  odx = Odx_Open(path);
  unlink(path);
  return odx;
}

int main(void) {
  const char *doc = "<?xml version=\"1.0\"?>\n<ODX><FLASH><ECU-MEMS>\n"
                    "<ECU-MEM ID=\"APP\"><MEM><FLASHDATAS>\n"
                    "<FLASHDATA><SHORT-NAME>blk0</SHORT-NAME><DATA>0102A0FF</DATA></FLASHDATA>\n"
                    "<FLASHDATA><SHORT-NAME>blk1</SHORT-NAME><DATA>DEADBEEF00</DATA></FLASHDATA>\n"
                    "</FLASHDATAS></MEM></ECU-MEM>\n"
                    "<ECU-MEM ID=\"CAL\"><MEM><FLASHDATAS>\n"
                    "<FLASHDATA><SHORT-NAME>c</SHORT-NAME><DATA>55</DATA></FLASHDATA>\n"
                    "</FLASHDATAS></MEM></ECU-MEM>\n"
                    "</ECU-MEMS></FLASH></ODX>\n";
  Odx_Type *odx = open_text("selftest.odx", doc);
  int ok = (NULL != odx);
  if (ok) {
    ok = (odx->NumOfMems == 2) && (0 == strcmp(odx->Mems[0]->Name, "APP")) &&
         (odx->Mems[0]->NumOfBlocks == 2) && (0 == strcmp(odx->Mems[0]->Blocks[0]->Name, "blk0")) &&
         (odx->Mems[0]->Blocks[0]->Size == 4) &&
         (0 == memcmp(odx->Mems[0]->Blocks[0]->Data, "\x01\x02\xA0\xFF", 4)) &&
         (odx->Mems[0]->Blocks[1]->Size == 5) &&
         (0 == memcmp(odx->Mems[0]->Blocks[1]->Data, "\xDE\xAD\xBE\xEF\x00", 5)) &&
         (0 == strcmp(odx->Mems[1]->Name, "CAL")) && (odx->Mems[1]->NumOfBlocks == 1) &&
         (odx->Mems[1]->Blocks[0]->Data[0] == 0x55);
    Odx_Close(odx);
  }
  check("odx_flashdata_blocks", ok);

  odx = open_text("selftest_bad.odx", "<ODX><FLASH></FLASH></ODX>");
  check("odx_no_ecu_mems_rejected", NULL == odx);
  odx = open_text("selftest_bad2.odx", "<ODX><FLASH><ECU-MEMS>");
  check("odx_malformed_xml_rejected", NULL == odx);

  printf("RESULT passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
