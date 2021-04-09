#ifndef _H_UTILS_
#define _H_UTILS_

#include <zephyr.h>

static inline void hexdump(const char *str, const uint8_t *pkt, size_t len) {
  if (!len) {
    LOG_DBG("%s zero-length packet", str);
    return;
  }
  LOG_HEXDUMP_DBG(pkt, len, str);
}


#endif
