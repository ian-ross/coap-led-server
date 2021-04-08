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

#include <net/coap.h>
#include <net/coap_link_format.h>
#include <net/net_ip.h>
#include <net/net_mgmt.h>
#include <net/net_event.h>
#include <net/net_conn_mgr.h>
#include <net/socket.h>
#include <net/udp.h>


static inline void hexdump(const char *str, const uint8_t *pkt, size_t len) {
  if (!len) {
    LOG_DBG("%s zero-length packet", str);
    return;
  }
  LOG_HEXDUMP_DBG(pkt, len, str);
}

// ==> QUESTION: IS THIS DEFINED BY THE PROTOCOL, OR JUST AN
// APPLICATION LIMIT HERE?
#define MAX_COAP_MSG_LEN 256

// This is the IANA assigned port for CoAP.
#define COAP_PORT 5683

// This is the link local (FF02) version of the "All CoAP Nodes"
// address FF0X::FD, from the "IPv6 Multicast Address Space Registry",
// in the "Variable Scope Multicast Addresses" space (RFC 3307).
#define ALL_NODES_LOCAL_COAP_MCAST {{{0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfd}}}

// ==> QUESTION: HOW BIG DOES THIS NEED TO BE FOR REAL APPLICATIONS?
// THERE NEEDS TO BE LOGGING AND REPORTING OF OVERFLOWS SOMEHOW. DOES
// THAT HAPPEN BY JUST NACKING MESSAGES THAT COME IN WHEN THERE'S NO
// PENDING SLOT AVAILABLE?
#define NUM_PENDINGS 3

// CoAP socket FD.
static int sock = -1;

// From Section 12.3 of RFC 7252: "text/plain" content format.
static const uint8_t text_plain_format = 0;

static struct coap_pending pendings[NUM_PENDINGS];

static struct k_delayed_work retransmit_work;

// Is our LED on or off? (Not wired up to anything yet...)
static bool led_state = false;


static struct k_sem quit_lock;
static struct net_mgmt_event_callback mgmt_cb;
static bool connected;
K_SEM_DEFINE(run_app, 0, 1);
static bool want_to_quit;

#define EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

#define STACK_SIZE 8192

#define THREAD_PRIORITY K_PRIO_PREEMPT(8)

static void process_coap(void);
static int process_client_request(void);


K_THREAD_DEFINE(coap_thread_id, STACK_SIZE,
                process_coap, NULL, NULL, NULL,
                THREAD_PRIORITY, 0, -1);


void quit(void) { k_sem_give(&quit_lock); }

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

static void init_app(void) {
  k_sem_init(&quit_lock, 0, UINT_MAX);

  LOG_INF("Basic CoAP server");

  net_mgmt_init_event_callback(&mgmt_cb, event_handler, EVENT_MASK);
  net_mgmt_add_event_callback(&mgmt_cb);
  net_conn_mgr_resend_status();
}


static int cmd_quit(const struct shell *shell, size_t argc, char *argv[]) {
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

static int start_coap_server(void) {
  // The Zephyr CoAP API doesn't have anything to do with sockets, so
  // you set up the low-level server sockets yourself. It's just a
  // simple UDP "socket + bind" thing anyway.

  struct sockaddr_in6 addr6;
  memset(&addr6, 0, sizeof(addr6));
  addr6.sin6_family = AF_INET6;
  addr6.sin6_port = htons(COAP_PORT);

  sock = socket(addr6.sin6_family, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    LOG_ERR("Failed to create UDP socket %d", errno);
    return -errno;
  }

  int r = bind(sock, (struct sockaddr *)&addr6, sizeof(addr6));
  if (r < 0) {
    LOG_ERR("Failed to bind UDP socket %d", errno);
    return -errno;
  }

  return 0;
}

static int send_coap_reply(struct coap_packet *cpkt,
                           const struct sockaddr *addr, socklen_t addr_len) {
  hexdump("Response", cpkt->data, cpkt->offset);

  int r = sendto(sock, cpkt->data, cpkt->offset, 0, addr, addr_len);
  if (r < 0) {
    LOG_ERR("Failed to send %d", errno);
    r = -errno;
  }

  return r;
}

static int well_known_core_get(struct coap_resource *res,
                               struct coap_packet *req, struct sockaddr *addr,
                               socklen_t addr_len) {
  // This is for the "well known" CoAP resources, which are basically
  // an introspection method for learning about what "real" resources
  // are supported. The Zephyr CoAP API deals with processing these.

  uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
  if (!data)
    return -ENOMEM;

  struct coap_packet resp;
  int r = coap_well_known_core_get(res, req, &resp, data, MAX_COAP_MSG_LEN);
  if (r < 0)
    goto end;

  r = send_coap_reply(&resp, addr, addr_len);

end:
  k_free(data);
  return r;
}

static int led_get(struct coap_resource *res, struct coap_packet *req,
                   struct sockaddr *addr, socklen_t addr_len) {
  uint8_t code = coap_header_get_code(req);
  uint8_t type = coap_header_get_type(req);
  uint16_t id = coap_header_get_id(req);

  LOG_INF("led_get  type: %u code %u id %u", type, code, id);

  uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
  if (!data)
    return -ENOMEM;

  type = type == COAP_TYPE_CON ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;
  struct coap_packet resp;
  uint8_t tok[8];
  uint8_t toklen = coap_header_get_token(req, tok);
  int r = coap_packet_init(&resp, data, MAX_COAP_MSG_LEN, 1, type, toklen,
                           (uint8_t *)tok, COAP_RESPONSE_CODE_CONTENT, id);
  if (r < 0)
    goto end;

  r = coap_packet_append_option(&resp, COAP_OPTION_CONTENT_FORMAT,
                                &text_plain_format, sizeof(text_plain_format));
  if (r < 0)
    goto end;

  r = coap_packet_append_payload_marker(&resp);
  if (r < 0)
    goto end;

  uint8_t payload[40];
  r = snprintk((char *)payload, sizeof(payload), "T:%u C:%u MID:%u LED:%s\n",
               type, code, id, led_state ? "ON" : "OFF");
  if (r < 0)
    goto end;

  r = coap_packet_append_payload(&resp, (uint8_t *)payload, strlen(payload));
  if (r < 0)
    goto end;

  r = send_coap_reply(&resp, addr, addr_len);

end:
  k_free(data);
  return r;
}

static int led_post(struct coap_resource *res, struct coap_packet *req,
                    struct sockaddr *addr, socklen_t addr_len) {
  uint8_t code = coap_header_get_code(req);
  uint8_t type = coap_header_get_type(req);
  uint16_t id = coap_header_get_id(req);

  LOG_INF("led_post  type: %u code %u id %u", type, code, id);

  uint16_t payload_len;
  const uint8_t *payload = coap_packet_get_payload(req, &payload_len);
  if (payload) {
    hexdump("POST Payload", payload, payload_len);
  } else {
    LOG_INF("POST with no payload!");
  }

  if (payload_len == 2 &&
      payload[0] == 'o' && payload[1] == 'n')
    led_state = true;
  else if (payload_len == 3 &&
           payload[0] == 'o' && payload[1] == 'f' && payload[2] == 'f')
    led_state = false;

  uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
  if (!data)
    return -ENOMEM;

  type = type == COAP_TYPE_CON ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;
  struct coap_packet resp;
  uint8_t tok[8];
  uint8_t toklen = coap_header_get_token(req, tok);
  int r = coap_packet_init(&resp, data, MAX_COAP_MSG_LEN, 1, type, toklen,
                           (uint8_t *)tok, COAP_RESPONSE_CODE_CHANGED, id);
  if (r < 0)
    goto end;

  r = coap_packet_append_option(&resp, COAP_OPTION_CONTENT_FORMAT,
                                &text_plain_format, sizeof(text_plain_format));
  if (r < 0)
    goto end;

  r = coap_packet_append_payload_marker(&resp);
  if (r < 0)
    goto end;

  uint8_t rpayload[40];
  r = snprintk((char *)rpayload, sizeof(rpayload), "T:%u C:%u MID:%u LED:%s\n",
               type, code, id, led_state ? "ON" : "OFF");
  if (r < 0)
    goto end;

  r = coap_packet_append_payload(&resp, (uint8_t *)rpayload, strlen(rpayload));
  if (r < 0)
    goto end;

  r = send_coap_reply(&resp, addr, addr_len);

end:
  k_free(data);
  return r;
}

static void retransmit_request(struct k_work *work) {
  struct coap_pending *pending =
      coap_pending_next_to_expire(pendings, NUM_PENDINGS);
  if (!pending) return;

  if (!coap_pending_cycle(pending)) {
    k_free(pending->data);
    coap_pending_clear(pending);
    return;
  }

  k_delayed_work_submit(&retransmit_work, K_MSEC(pending->timeout));
}

static const char *const led_path[] = {"led", NULL};

static struct coap_resource resources[] = {
    { .get = well_known_core_get,
      .path = COAP_WELL_KNOWN_CORE_PATH, },
    { .get = led_get,
      .post = led_post,
      .path = led_path },
    {},
};

void start_coap(void)
{
  k_thread_name_set(coap_thread_id, "coap");
  k_thread_start(coap_thread_id);
}

void stop_coap(void)
{
  // Not a very graceful way to close a thread, but we may be blocked
  // in recvfrom call, so it seems to be necessary.
  k_thread_abort(coap_thread_id);
  if (sock >= 0) (void)close(sock);
}

static void process_coap(void) {
  // if (!join_coap_multicast_group()) goto quit;
  if (start_coap_server() < 0) goto quit;
  k_delayed_work_init(&retransmit_work, retransmit_request);

  while (true) {
    if (process_client_request() < 0) goto quit;
  }

quit:
  quit();
}

static void process_coap_request(uint8_t *data, uint16_t data_len,
                                 struct sockaddr *addr, socklen_t addr_len) {
  struct coap_packet req;
  struct coap_option options[16] = {0};
  uint8_t opt_num = 16U;
  int r = coap_packet_parse(&req, data, data_len, options, opt_num);
  if (r < 0) {
    LOG_ERR("Invalid data received (%d)\n", r);
    return;
  }

  uint8_t type = coap_header_get_type(&req);

  struct coap_pending *pending =
      coap_pending_received(&req, pendings, NUM_PENDINGS);
  if (!pending) {
    goto end;
  }

  // Clear CoAP pending request.
  if (type == COAP_TYPE_ACK) {
    k_free(pending->data);
    coap_pending_clear(pending);
  }

  return;

end:
  r = coap_handle_request(&req, resources, options, opt_num, addr, addr_len);
  if (r < 0) {
    LOG_WRN("No handler for such request (%d)\n", r);
  }
}

static int process_client_request(void) {
  struct sockaddr addr;
  socklen_t addr_len = sizeof(addr);
  uint8_t req[MAX_COAP_MSG_LEN];

  do {
    int received = recvfrom(sock, req, sizeof(req), 0, &addr, &addr_len);
    if (received < 0) {
      LOG_ERR("Connection error %d", errno);
      return -errno;
    }
    hexdump("RECEIVED", req, received);

    process_coap_request(req, received, &addr, addr_len);
  } while (true);

  return 0;
}


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
