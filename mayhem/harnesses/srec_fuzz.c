/*
 * Fuzz the S-record / Intel-HEX / binary firmware loader (tools/libraries/srec) through its public
 * file API, as srec_cat does: Srec_Open/IHex_Open, then Srec_Print, Srec_Range, Srec_Close.
 * The first input byte picks the loader (Srec_Open dispatches on the file name's extension).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "srec.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const char *exts[] = {".s19", ".hex", ".bin", ".img"};
  char path[512];
  const char *tmp = getenv("TMPDIR");
  Srec_FileType *srec;
  uint32_t length = 0;
  int fd;
  FILE *fp;
  uint8_t sel;

  if (size < 1) {
    return -1;
  }
  sel = data[0];
  data++;
  size--;
  if ((NULL == tmp) || ('\0' == tmp[0])) {
    tmp = "/tmp";
  }
  snprintf(path, sizeof(path), "%s/srecfuzzXXXXXX", tmp);
  fd = mkstemp(path);
  if (fd < 0) {
    return 0;
  }
  fp = fdopen(fd, "wb");
  if (NULL == fp) {
    close(fd);
    unlink(path);
    return 0;
  }
  if (size > 0) {
    fwrite(data, 1, size, fp);
  }
  fclose(fp);
  /* the loader picks its format from the path, so give the file the selected extension */
  {
    char named[600];
    snprintf(named, sizeof(named), "%s%s", path, exts[sel % 4]);
    if (0 != rename(path, named)) {
      unlink(path);
      return 0;
    }
    if ((sel & 0x80) != 0) {
      srec = IHex_Open(named);
    } else {
      srec = Srec_Open(named);
    }
    if (NULL != srec) {
      Srec_Print(srec);
      (void)Srec_Range(srec, &length);
      Srec_Close(srec);
    }
    unlink(named);
  }
  return 0;
}
