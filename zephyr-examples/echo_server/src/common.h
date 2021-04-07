/*
 * Copyright (c) 2017-2019 Intel Corporation.
 * Copyright (c) 2018 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#define MY_PORT 4242
#define STACK_SIZE 1024

#define THREAD_PRIORITY K_PRIO_PREEMPT(8)

#define RECV_BUFFER_SIZE 1280
#define STATS_TIMER 60 /* How often to print statistics (in seconds) */

struct data {
  int sock;
  char recv_buffer[RECV_BUFFER_SIZE];
  uint32_t counter;
  atomic_t bytes_received;
  struct k_delayed_work stats_print;
};

extern struct data ipv6;

void start_udp(void);
void stop_udp(void);

void quit(void);

