/* echo-server.c - Networking echo server */

/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright (c) 2018 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(net_echo_server_sample, LOG_LEVEL_DBG);

#include <zephyr.h>
#include <linker/sections.h>
#include <errno.h>
#include <shell/shell.h>

#include <net/net_core.h>
#include <net/tls_credentials.h>

#include <net/net_mgmt.h>
#include <net/net_event.h>
#include <net/net_conn_mgr.h>

#include "common.h"

#define APP_BANNER "Run echo server"

static struct k_sem quit_lock;
static struct net_mgmt_event_callback mgmt_cb;
static bool connected;
K_SEM_DEFINE(run_app, 0, 1);
static bool want_to_quit;

#define EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

struct data ipv6;

void quit(void) {
	k_sem_give(&quit_lock);
}

static void event_handler(struct net_mgmt_event_callback *cb,
			  uint32_t mgmt_event, struct net_if *iface) {
	if ((mgmt_event & EVENT_MASK) != mgmt_event) return;

	if (want_to_quit) {
		k_sem_give(&run_app);
		want_to_quit = false;
	}

	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network connected");
		connected = true;
		k_sem_give(&run_app);
		return;
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		if (!connected) {
			LOG_INF("Waiting network to be connected");
		} else {
			LOG_INF("Network disconnected");
			connected = false;
		}

		k_sem_reset(&run_app);
		return;
	}
}

static void init_app(void)
{
	k_sem_init(&quit_lock, 0, UINT_MAX);

	LOG_INF(APP_BANNER);

	if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
		net_mgmt_init_event_callback(&mgmt_cb, event_handler, EVENT_MASK);
		net_mgmt_add_event_callback(&mgmt_cb);
		net_conn_mgr_resend_status();
	}
}

static int cmd_sample_quit(const struct shell *shell,
			  size_t argc, char *argv[])
{
	want_to_quit = true;
	net_conn_mgr_resend_status();
	quit();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sample_commands,
	SHELL_CMD(quit, NULL, "Quit the sample application\n", cmd_sample_quit),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sample, &sample_commands, "Sample application commands", NULL);


void main(void)
{
	init_app();

  /* If the config library has not been configured to start the app
   * only after we have a connection, then we can start it right away.
   */
	if (!IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) k_sem_give(&run_app);

	/* Wait for the connection. */
	k_sem_take(&run_app, K_FOREVER);

	LOG_INF("Starting...");
	if (IS_ENABLED(CONFIG_NET_UDP)) start_udp();

	k_sem_take(&quit_lock, K_FOREVER);

	if (connected) {
    LOG_INF("Stopping...");
    if (IS_ENABLED(CONFIG_NET_UDP)) stop_udp();
  }
}