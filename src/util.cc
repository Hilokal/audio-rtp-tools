#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int set_thread_name(const char* name) {
#ifdef __linux__
  return pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
  return pthread_setname_np(name);
#else
  // Unsupported platform
  return -1;
#endif
}

#ifdef __APPLE__
int check_for_memory_leaks()
{

  FILE *fp;
  char output[1035];

  char command[100];
  snprintf(command, 100, "/usr/bin/leaks %d", getpid());
  printf("Calling %s\n", command);

  /* Open the command for reading. */
  fp = popen(command, "r");
  if (fp == NULL) {
    printf("Failed to run command\n" );
    return -1;
  }

  /* Read the output a line at a time - output it. */
  while (fgets(output, sizeof(output), fp) != NULL) {
    printf("%s", output);
  }

  printf("finished calling %s\n", command);
  /* close */
  pclose(fp);

  return 0;
}
#endif

size_t get_stack_size_for_thread(const char* thread_type) {
    char env_var_name[50];
    snprintf(env_var_name, sizeof(env_var_name), "%s_THREAD_STACK_SIZE", thread_type);

    char* env_var = getenv(env_var_name);
    size_t stack_size = 0;

    if (env_var != NULL) {
        char* endptr;
        stack_size = strtol(env_var, &endptr, 10);

        if (*endptr != '\0') {
            printf("Error: Invalid value for %s_THREAD_STACK_SIZE\n", thread_type);
            stack_size = 0;
        }
    }

    return stack_size;
}
