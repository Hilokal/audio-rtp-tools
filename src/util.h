#pragma once

int set_thread_name(const char* name);

size_t get_stack_size_for_thread(const char* thread_type);

#ifdef __APPLE__
int check_for_memory_leaks();
#endif
