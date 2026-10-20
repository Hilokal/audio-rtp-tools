#pragma once

// Opus packets this short carry no audio: libopus decodes payloads of 1 byte
// plus the TOC (or none) with PLC/DTX (opus_decode_frame). The encoder emits
// them for DTX silence; WebRTC senders and our keep-alives put them on the wire.
#define DTX_PACKET_MAX_BYTES 2

int set_thread_name(const char* name);

size_t get_stack_size_for_thread(const char* thread_type);

#ifdef __APPLE__
int check_for_memory_leaks();
#endif
