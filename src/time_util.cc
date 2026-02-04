#include "time_util.h"

extern "C" {
  #include <libavutil/mathematics.h>
}

int64_t ntp_to_realtime(uint64_t ntp_timestamp) {
  if (ntp_timestamp > INT64_MAX) {
    return av_rescale(ntp_timestamp - (NTP_OFFSET << 32), MICROSECONDS, 1LL << 32);
  } else {
    return av_rescale(ntp_timestamp, MICROSECONDS, 1LL << 32) - NTP_OFFSET_US;
  }
}

/*
 // To test:
 // gcc time_util.cc -I/opt/homebrew/include -L/opt/homebrew/lib -lavutil
 // ./a.out

 #include <stdio.h>
 #include <assert.h>
 int main() {
  // Newer timestamp
  assert(ntp_to_realtime(16926700461382759874ULL) == 1732065763360000ULL);

  // Older timestamp from 1900
  assert(ntp_to_realtime(34918880717524816ULL) == -2200858614526866ULL);
  assert(ntp_to_realtime(5278618062092763ULL) == -2207759775959000ULL);
  printf("pass!\n");
 }
 */