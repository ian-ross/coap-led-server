// Basic OpenThread CoAP server: CoAP handling.

#include <logging/log.h>
LOG_MODULE_DECLARE(basic_coap_server, LOG_LEVEL_DBG);

#include <zephyr.h>
#include <errno.h>

#include <net/coap.h>
#include <net/coap_link_format.h>
#include <net/net_ip.h>
#include <net/socket.h>
#include <net/udp.h>

#include "coap.h"
#include "utils.h"


// CoAP resource definitions: defined in endpoints.c.
extern struct coap_resource coap_resources[];

// Defined in main.c.
extern void quit(void);


// This is the IANA assigned port for CoAP.
#define COAP_PORT 5683

// This is the link local (FF02) version of the "All CoAP Nodes"
// address FF0X::FD, from the "IPv6 Multicast Address Space Registry",
// in the "Variable Scope Multicast Addresses" space (RFC 3307).
#define ALL_NODES_LOCAL_COAP_MCAST {{{0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfd}}}

// CoAP socket file descriptor.
static int sock = -1;


static void process_coap(void);
static int process_client_request(void);
static void process_coap_request(uint8_t *data, uint16_t data_len,
                                 struct sockaddr *addr, socklen_t addr_len);
// static bool join_coap_multicast_group(void);


// ----------------------------------------------------------------------
// CoAP SERVER THREAD DEFINITIONS

#define STACK_SIZE 8192
#define THREAD_PRIORITY K_PRIO_PREEMPT(8)

K_THREAD_DEFINE(coap_thread_id, STACK_SIZE,
                process_coap, NULL, NULL, NULL,
                THREAD_PRIORITY, 0, -1);


// ----------------------------------------------------------------------
// PUBLIC API

// Send a CoAP reply: used from the applicatio-specific endpoint
// definitions.

int send_coap_reply(struct coap_packet *cpkt,
                    const struct sockaddr *addr, socklen_t addr_len) {
  // Debug message (defined in utils.h).
  hexdump("Response", cpkt->data, cpkt->offset);

  // Use the basic socket API to send the reply data over the server
  // socket.
  int r = sendto(sock, cpkt->data, cpkt->offset, 0, addr, addr_len);
  if (r < 0) {
    LOG_ERR("Failed to send %d", errno);
    r = -errno;
  }

  return r;
}


// Send a CoAP reply for the ".well-known/core" resource introspection
// endpoint.

int well_known_core_get(struct coap_resource *res,
                        struct coap_packet *req, struct sockaddr *addr,
                        socklen_t addr_len) {
  // This is for the "well known" CoAP resources, which are basically
  // an introspection method for learning about what "real" resources
  // are supported. The Zephyr CoAP API deals with processing these.

  // Allocate reply buffer.
  uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
  if (!data) return -ENOMEM;

  // Full the reply buffer using a CoAP API function.
  struct coap_packet resp;
  int r = coap_well_known_core_get(res, req, &resp, data, MAX_COAP_MSG_LEN);
  if (r < 0) goto end;

  // Send the reply.
  r = send_coap_reply(&resp, addr, addr_len);

end:
  k_free(data);
  return r;
}


// Public interface to start the CoAP server.

void start_coap(void)
{
  k_thread_name_set(coap_thread_id, "coap");
  k_thread_start(coap_thread_id);
}


// Public interface to stop the CoAP server.

void stop_coap(void)
{
  // Not a very graceful way to close a thread, but we may be blocked
  // waiting for a network message, so there's probably no better way
  // to do it.
  k_thread_abort(coap_thread_id);
  if (sock >= 0) (void)close(sock);
}


// ----------------------------------------------------------------------
// PRIVATE FUNCTIONS

// Initialise the CoAP server. The Zephyr CoAP API doesn't have
// anything to do with sockets, so you set up the low-level server
// sockets yourself. It's just a simple UDP "socket + bind" thing
// anyway.

static int start_coap_server(void) {
  // Create a listener socket address on the well-known CoAP port.
  struct sockaddr_in6 addr6;
  memset(&addr6, 0, sizeof(addr6));
  addr6.sin6_family = AF_INET6;
  addr6.sin6_port = htons(COAP_PORT);

  // Create a UDPv6 ("datagram") socket.
  sock = socket(addr6.sin6_family, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    LOG_ERR("Failed to create UDP socket %d", errno);
    return -errno;
  }

  // Bind the socket to our address: this means that this socket will
  // receive any messages sent to this device's CoAP port.
  int r = bind(sock, (struct sockaddr *)&addr6, sizeof(addr6));
  if (r < 0) {
    LOG_ERR("Failed to bind UDP socket %d", errno);
    return -errno;
  }

  return 0;
}


// Main server thread function: initialises CoAP server then processes
// requests as they come in. Quits on error.

static void process_coap(void) {
  // ==> NOTE: I'VE NOT BEEN ABLE TO GET THIS MULTICAST GROUP STUFF
  // WORKING YET.
  // if (!join_coap_multicast_group()) goto quit;

  // Initialise the CoAP server.
  if (start_coap_server() < 0) goto quit;

  // Process client messages, quitting if there's an error.
  // ==> NOTE: A REAL APPLICATION WOULD NEED BETTER ERROR HANDLING
  // THAN THIS!
  while (true) {
    if (process_client_request() < 0) goto quit;
  }

quit:
  quit();
}


#if 0
// Multicast setup. Still need to work out how to make this go.

static bool join_coap_multicast_group(void) {
  struct net_if *iface = net_if_get_default();
  if (!iface) {
    LOG_ERR("Could not get default interface\n");
    return false;
  }

  static struct sockaddr_in6 mcast_addr =
    { .sin6_family = AF_INET6,
      .sin6_addr = ALL_NODES_LOCAL_COAP_MCAST,
      .sin6_port = htons(COAP_PORT) };
  if (!net_if_ipv6_maddr_add(iface, &mcast_addr.sin6_addr)) {
    LOG_ERR("Cannot join IPv6 multicast group");
   return false;
  }

  return true;
}
#endif


// Process a single CoAP request from a client. This function just
// does the socket-level stuff, then hands off immediately to another
// function.

static int process_client_request(void) {
  struct sockaddr addr;
  socklen_t addr_len = sizeof(addr);
  uint8_t req[MAX_COAP_MSG_LEN];

  do {
    // Receive data from the socket. This also gets the client
    // address, which we need for sending a reply.
    int received = recvfrom(sock, req, sizeof(req), 0, &addr, &addr_len);
    if (received < 0) {
      LOG_ERR("Connection error %d", errno);
      return -errno;
    }
    hexdump("RECEIVED", req, received);

    // Hand off to the CoAP-specific processing function.
    process_coap_request(req, received, &addr, addr_len);
  } while (true);

  return 0;
}


// Process a single CoAP request for a client. This function does the
// CoAP-level packet processing.

static void process_coap_request(uint8_t *data, uint16_t data_len,
                                 struct sockaddr *addr, socklen_t addr_len) {
  // Parse received data as a CoAP packet. This gives us a coap_packet
  // structure containing the broken down request information, as well
  // as the request options pulled out into coap_option values.
  struct coap_packet req;
  struct coap_option options[16] = {0};
  uint8_t opt_num = 16U;
  int r = coap_packet_parse(&req, data, data_len, options, opt_num);
  if (r < 0) {
    LOG_ERR("Invalid data received (%d)\n", r);
    return;
  }

  // Hand the request off to the CoAP API's resource-based request
  // router. We pass in the request along with our coap_resources
  // array, which defines all the CoAP resources we support. The
  // coap_handle_request API function routes the request to the
  // appropriate endpoint handler function.
  r = coap_handle_request(&req, coap_resources, options, opt_num, addr, addr_len);
  if (r < 0) {
    LOG_WRN("No handler for such request (%d)\n", r);
  }
}
