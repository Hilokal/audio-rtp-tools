#pragma once

#include <inttypes.h>

// number of microseconds in a second
#define MICROSECONDS 1000000

// Difference between Unix epoch (Jan 1, 1970) and NTP epoch (Jan 1, 1900)
#define NTP_OFFSET 2208988800ULL
#define NTP_OFFSET_US (NTP_OFFSET * 1000000ULL)

// Converts NTP timestamps into unix timestamps in microseconds
int64_t ntp_to_realtime(uint64_t ntp_timestamp);
