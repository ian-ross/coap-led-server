/* udp.c - UDP specific code for echo server */

/*
 * Copyright (c) 2017 Intel Corporation.
 * Copyright (c) 2018 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_DECLARE(net_echo_server_sample, LOG_LEVEL_DBG);

#include <zephyr.h>
#include <errno.h>
#include <stdio.h>

#include <net/socket.h>

#include "common.h"

static void process_udp6(void);

K_THREAD_DEFINE(udp6_thread_id, STACK_SIZE,
                process_udp6, NULL, NULL, NULL,
                THREAD_PRIORITY, 0, -1);

static int start_udp_proto(struct data *data, struct sockaddr *bind_addr,
			   socklen_t bind_addrlen)
{
	int ret;

	data->sock = socket(bind_addr->sa_family, SOCK_DGRAM, IPPROTO_UDP);
	if (data->sock < 0) {
		NET_ERR("Failed to create UDP socket: %d", errno);
		return -errno;
	}

	ret = bind(data->sock, bind_addr, bind_addrlen);
	if (ret < 0) {
		NET_ERR("Failed to bind UDP socket: %d", errno);
		ret = -errno;
	}

	return ret;
}

static int process_udp(struct data *data)
{
	int ret = 0;
	int received;
	struct sockaddr client_addr;
	socklen_t client_addr_len;

	NET_INFO("Waiting for UDP packets on port %d...", MY_PORT);

	do {
		client_addr_len = sizeof(client_addr);
		received = recvfrom(data->sock, data->recv_buffer, sizeof(data->recv_buffer),
                        0, &client_addr, &client_addr_len);

		if (received < 0) {
			/* Socket error */
			NET_ERR("UDP: Connection error %d", errno);
			ret = -errno;
			break;
		} else if (received) {
			atomic_add(&data->bytes_received, received);
		}

		ret = sendto(data->sock, data->recv_buffer, received, 0,
                 &client_addr, client_addr_len);
		if (ret < 0) {
			NET_ERR("UDP: Failed to send %d", errno);
			ret = -errno;
			break;
		}

		if (++data->counter % 1000 == 0U) {
			NET_INFO("UDP: Sent %u packets", data->counter);
		}

		NET_DBG("UDP: Received and replied with %d bytes", received);
	} while (true);

	return ret;
}

static void process_udp6(void)
{
	int ret;
	struct sockaddr_in6 addr6;

	(void)memset(&addr6, 0, sizeof(addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_port = htons(MY_PORT);

	ret = start_udp_proto(&ipv6, (struct sockaddr *)&addr6, sizeof(addr6));
	if (ret < 0) {
		quit();
		return;
	}

	k_delayed_work_submit(&ipv6.stats_print, K_SECONDS(STATS_TIMER));

	while (ret == 0) {
		ret = process_udp(&ipv6);
		if (ret < 0) quit();
	}
}

static void print_stats(struct k_work *work)
{
	struct data *data = CONTAINER_OF(work, struct data, stats_print);
	int total_received = atomic_get(&data->bytes_received);

	if (total_received) {
		if ((total_received / STATS_TIMER) < 1024) {
			LOG_INF("UDP: Received %d B/sec", total_received / STATS_TIMER);
		} else {
			LOG_INF("UDP: Received %d KiB/sec", total_received / 1024 / STATS_TIMER);
		}

		atomic_set(&data->bytes_received, 0);
	}

	k_delayed_work_submit(&data->stats_print, K_SECONDS(STATS_TIMER));
}

void start_udp(void)
{
  k_delayed_work_init(&ipv6.stats_print, print_stats);
  k_thread_name_set(udp6_thread_id, "udp6");
  k_thread_start(udp6_thread_id);
}

void stop_udp(void)
{
	/* Not very graceful way to close a thread, but as we may be blocked
	 * in recvfrom call it seems to be necessary
	 */
  k_thread_abort(udp6_thread_id);
  if (ipv6.sock >= 0) (void)close(ipv6.sock);
}
