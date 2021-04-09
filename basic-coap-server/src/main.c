// Basic OpenThread CoAP server.
//
// Originally derived from Zephyr net/socket/coap_server and
// net/socket/echo_server examples.

#include <logging/log.h>
LOG_MODULE_REGISTER(basic_coap_server, LOG_LEVEL_DBG);

#include <zephyr.h>
#include <errno.h>
#include <shell/shell.h>
#include <sys/printk.h>

#include <net/net_ip.h>
#include <net/net_mgmt.h>
#include <net/net_event.h>
#include <net/net_conn_mgr.h>

#include "coap.h"
#include "endpoints.h"


// ----------------------------------------------------------------------
// CONNECTION STATE MANAGEMENT

// Is the network connected?
static bool connected;

// Sempahore used to hold off application initialisation until a valid
// network connection is there.
K_SEM_DEFINE(run_app, 0, 1);

// Quit flag used to communicate between shell command and management
// callback.
static bool want_to_quit;

// Detect network connected and disconnected events.
#define EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)


static void event_handler(struct net_mgmt_event_callback *cb,
                          uint32_t mgmt_event, struct net_if *iface) {
  // Ignore any events we didn't ask for.
  if ((mgmt_event & EVENT_MASK) != mgmt_event) return;

  // Handle quitting.
  if (want_to_quit) {
    k_sem_give(&run_app);
    want_to_quit = false;
  }

  // If we're connected, flag it and release the semaphore that holds
  // off application initialisation.
  if (mgmt_event == NET_EVENT_L4_CONNECTED) {
    LOG_INF("Network connected");
    connected = true;
    k_sem_give(&run_app);
    return;
  }

  // If we're disconnected, flag it and reset the semaphore that holds
  // off application initialisation.
  // ==> NOTE: I'M NOT SURE WHAT THIS REALLY DOES ONCE THE CoAP SERVER
  // IS RUNNING. IT CERTAINLY DOESN'T STOP IT. IN A REAL APPLICATION,
  // YOU WOULD WANT TO BE MORE CAREFUL ABOUT WHAT HAPPENS AS NETWORK
  // CONNECTIONS COME AND GO.
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

// Semaphore and command to signal it used for shutting down the CoAP
// server.

static struct k_sem quit_lock;

void quit(void) { k_sem_give(&quit_lock); }


// Initialise network connection management. All connection events go
// via the event_handler callback, which is used to trigger
// application initialisation when a connected network is detected.

static struct net_mgmt_event_callback mgmt_cb;

static void init_app(void) {
  // Set up the "quit" semaphore.
  k_sem_init(&quit_lock, 0, UINT_MAX);

  LOG_INF("Basic CoAP server");

  // Initialise network connection callback.
  net_mgmt_init_event_callback(&mgmt_cb, event_handler, EVENT_MASK);
  net_mgmt_add_event_callback(&mgmt_cb);

  // Trigger initial call to event handler callback with current
  // connection status.
  net_conn_mgr_resend_status();
}


// ----------------------------------------------------------------------
// SHELL COMMANDS

// Add shell command to stop CoAP server application. This is
// accessible as "basic_coap quit" in the Zephyr shell.

static int cmd_quit(const struct shell *shell, size_t argc, char *argv[]) {
  // Slightly sneaky: just set a flag and use the "resend status" API
  // to trigger a call into the network connection management event
  // handler callback.
  want_to_quit = true;
  net_conn_mgr_resend_status();
  quit();
  return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE
  (basic_coap_commands,
   SHELL_CMD(quit, NULL, "Quit the basic CoAP server application\n", cmd_quit),
   SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER
  (basic_coap, &basic_coap_commands, "Basic CoAP server application commands",
   NULL);


// ----------------------------------------------------------------------
// MAIN PROGRAM

void main(void) {
  // Initialise application start semaphore and network connection
  // management API.
  init_app();

  // Wait for OpenThread connection.
  k_sem_take(&run_app, K_FOREVER);

  // Start CoAP handler thread.
  LOG_INF("Starting...");
  start_coap();

  // Wait for shell "basic_coap quit" command.
  k_sem_take(&quit_lock, K_FOREVER);

  // Kill CoAP server thread if it's running.
  if (connected) {
    LOG_INF("Stopping...");
    stop_coap();
  }

  LOG_DBG("Done");
}
