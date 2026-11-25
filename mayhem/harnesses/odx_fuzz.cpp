/*
 * Fuzz the ODX-F flash container reader (tools/libraries/odx, used by upstream's loader tools)
 * through its public file API, as odx_cat does: Odx_Open on the input file, then Odx_Close.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "odx.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char path[512];
  const char *tmp = getenv("TMPDIR");
  int fd;
  FILE *fp;
  Odx_Type *odx;

  if ((NULL == tmp) || ('\0' == tmp[0])) {
    tmp = "/tmp";
  }
  snprintf(path, sizeof(path), "%s/odxfuzzXXXXXX", tmp);
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
  odx = Odx_Open(path);
  if (NULL != odx) {
    Odx_Close(odx);
  }
  unlink(path);
  return 0;
}
