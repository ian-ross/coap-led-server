/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(net_coap_server_sample, LOG_LEVEL_DBG);

#include <errno.h>
#include <sys/printk.h>
#include <sys/byteorder.h>
#include <zephyr.h>

#include <net/socket.h>
#include <net/net_mgmt.h>
#include <net/net_ip.h>
#include <net/udp.h>
#include <net/coap.h>
#include <net/coap_link_format.h>

#include "net_private.h"
#include "ipv6.h"


// ==> QUESTION: IS THIS DEFINED BY THE PROTOCOL, OR JUST AN
// APPLICATION LIMIT HERE?
#define MAX_COAP_MSG_LEN 256

// This is the IANA assigned port for CoAP.
#define MY_COAP_PORT 5683

// This is the link local (FF02) version of the "All CoAP Nodes"
// address FF0X::FD, from the "IPv6 Multicast Address Space Registry",
// in the "Variable Scope Multicast Addresses" space (RFC 3307).
#define ALL_NODES_LOCAL_COAP_MCAST {{{0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfd}}}

// ==> TODO: GET THIS FROM OPENTHREAD!!!
#define MY_IP6ADDR {{{0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x1}}}

// ==> QUESTION: HOW BIG DOES THIS NEED TO BE FOR REAL APPLICATIONS?
// THERE NEEDS TO BE LOGGING AND REPORTING OF OVERFLOWS SOMEHOW. DOES
// THAT HAPPEN BY JUST NACKING MESSAGES THAT COME IN WHEN THERE'S NO
// PENDING SLOT AVAILABLE?
#define NUM_PENDINGS 3

// CoAP socket FD.
static int sock;

// ==> QUESTION: WHERE DOES THIS GET INITIALISED? THIS LOOKS WEIRD.
static const uint8_t plain_text_format;

static struct coap_pending pendings[NUM_PENDINGS];

static struct k_delayed_work retransmit_work;

// Is our LED on or off? (Not wired up to anything yet...)
static bool led_state = false;


static bool join_coap_multicast_group(void)
{
	struct net_if *iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("Could not get default interface\n");
		return false;
	}

  // ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ----
  // START NOT NEEDED WITH OPENTHREAD?
  
	static struct in6_addr my_addr = MY_IP6ADDR;
	if (net_addr_pton(AF_INET6, CONFIG_NET_CONFIG_MY_IPV6_ADDR, &my_addr) < 0) {
		LOG_ERR("Invalid IPv6 address %s", CONFIG_NET_CONFIG_MY_IPV6_ADDR);
	}

  // ==> CHECK: I'M HOPING THAT ADDING AN ADDRESS THAT'S ALREADY
  // ASSIGNED IS A NO-OP... IN ANY CASE, WE'LL BE DOING THIS
  // DIFFERENTLY WHEN USING OPENTHREAD, BECAUSE THE MAIN ADDRESS
  // SHOULD ALREADY BE ASSIGNED, AND WE'LL JUST NEED TO SET UP THE
  // MULTICAST ADDRESS HERE.
	struct net_if_addr *ifaddr =
    net_if_ipv6_addr_add(iface, &my_addr, NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		LOG_ERR("Could not add unicast address to interface");
		return false;
	}

	ifaddr->addr_state = NET_ADDR_PREFERRED;

  // END NOT NEEDED WITH OPENTHREAD?
  // ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ---- 8< ----
  
	static struct sockaddr_in6 mcast_addr = {
		.sin6_family = AF_INET6,
		.sin6_addr = ALL_NODES_LOCAL_COAP_MCAST,
		.sin6_port = htons(MY_COAP_PORT) };
	int r = net_ipv6_mld_join(iface, &mcast_addr.sin6_addr);
	if (r < 0) {
		LOG_ERR("Cannot join %s IPv6 multicast group (%d)",
			log_strdup(net_sprint_ipv6_addr(&mcast_addr.sin6_addr)), r);
		return false;
	}

	return true;
}

static int start_coap_server(void)
{
  // The Zephyr CoAP API doesn't have anything to do with sockets, so
  // you set up the low-level server sockets yourself. It's just a
  // simple UDP "socket + bind" thing anyway.
  
	struct sockaddr_in6 addr6;
	memset(&addr6, 0, sizeof(addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_port = htons(MY_COAP_PORT);

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

static int send_coap_reply
  (struct coap_packet *cpkt, const struct sockaddr *addr, socklen_t addr_len)
{
	net_hexdump("Response", cpkt->data, cpkt->offset);

	int r = sendto(sock, cpkt->data, cpkt->offset, 0, addr, addr_len);
	if (r < 0) {
		LOG_ERR("Failed to send %d", errno);
		r = -errno;
	}

	return r;
}

static int well_known_core_get
  (struct coap_resource *resource, struct coap_packet *req,
   struct sockaddr *addr, socklen_t addr_len)
{
  // This is for the "well known" CoAP resources, which are basically
  // an introspection method for learning about what "real" resources
  // are supported. The Zephyr CoAP API deals with processing these.
  
	uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
	if (!data) return -ENOMEM;

	struct coap_packet resp;
	int r = coap_well_known_core_get(resource, req, &resp, data, MAX_COAP_MSG_LEN);
	if (r < 0) goto end;

	r = send_coap_reply(&resp, addr, addr_len);

end:
	k_free(data);
	return r;
}

static int led_get
  (struct coap_resource *resource, struct coap_packet *req,
   struct sockaddr *addr, socklen_t addr_len)
{
	uint8_t code = coap_header_get_code(req);
	uint8_t type = coap_header_get_type(req);
	uint16_t id = coap_header_get_id(req);

	LOG_INF("*******");
	LOG_INF("type: %u code %u id %u", type, code, id);
	LOG_INF("*******");

	uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
	if (!data) return -ENOMEM;

  type = type == COAP_TYPE_CON ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;
	struct coap_packet resp;
	uint8_t tok[8];
	uint8_t toklen = coap_header_get_token(req, tok);
	int r = coap_packet_init
    (&resp, data, MAX_COAP_MSG_LEN, 1, type,
     toklen, (uint8_t *)tok, COAP_RESPONSE_CODE_CONTENT, id);
	if (r < 0) goto end;

	r = coap_packet_append_option
    (&resp,
     COAP_OPTION_CONTENT_FORMAT, &plain_text_format, sizeof(plain_text_format));
	if (r < 0) goto end;

	r = coap_packet_append_payload_marker(&resp);
	if (r < 0) goto end;

	/* The response that coap-client expects */
	uint8_t payload[40];
	r = snprintk((char *)payload, sizeof(payload),
               "T:%u C:%u MID:%u LED:%s\n",
               type, code, id, led_state ? "ON" : "OFF");
	if (r < 0) goto end;

	r = coap_packet_append_payload(&resp, (uint8_t *)payload, strlen(payload));
	if (r < 0) goto end;

	r = send_coap_reply(&resp, addr, addr_len);

end:
	k_free(data);
	return r;
}

static int led_post
  (struct coap_resource *resource, struct coap_packet *req,
   struct sockaddr *addr, socklen_t addr_len)
{
	uint8_t code = coap_header_get_code(req);
	uint8_t type = coap_header_get_type(req);
	uint16_t id = coap_header_get_id(req);

	LOG_INF("*******");
	LOG_INF("type: %u code %u id %u", type, code, id);
	LOG_INF("*******");

	uint16_t payload_len;
	const uint8_t *payload = coap_packet_get_payload(req, &payload_len);
	if (payload) {
		net_hexdump("POST Payload", payload, payload_len);
	}

  if (payload_len > 0) led_state = payload[0] != 0;
  
	uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
	if (!data) return -ENOMEM;

	type = type == COAP_TYPE_CON ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;
	struct coap_packet resp;
	uint8_t tok[8];
	uint8_t toklen = coap_header_get_token(req, tok);
	int r = coap_packet_init(&resp, data, MAX_COAP_MSG_LEN,
                           1, type, toklen, (uint8_t *)tok,
                           COAP_RESPONSE_CODE_CHANGED, id);
	if (r < 0) goto end;

	r = coap_packet_append_option
    (&resp,
     COAP_OPTION_CONTENT_FORMAT, &plain_text_format, sizeof(plain_text_format));
	if (r < 0) goto end;

	r = coap_packet_append_payload_marker(&resp);
	if (r < 0) goto end;

	/* The response that coap-client expects */
	uint8_t rpayload[40];
	r = snprintk((char *)rpayload, sizeof(rpayload),
               "T:%u C:%u MID:%u LED:%s\n",
               type, code, id, led_state ? "ON" : "OFF");
	if (r < 0) goto end;

	r = coap_packet_append_payload(&resp, (uint8_t *)rpayload, strlen(rpayload));
	if (r < 0) goto end;

	r = send_coap_reply(&resp, addr, addr_len);

end:
	k_free(data);
	return r;
}

static void retransmit_request(struct k_work *work)
{
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

static const char * const led_path[] = { "led", NULL };

static struct coap_resource resources[] = {
	{ .get = well_known_core_get,
	  .path = COAP_WELL_KNOWN_CORE_PATH,
	},
	{ .get = led_get,
	  .post = led_post,
	  .path = led_path
	},
	{ },
};

static void process_coap_request
  (uint8_t *data, uint16_t data_len, struct sockaddr *addr, socklen_t addr_len)
{
	struct coap_packet req;
	struct coap_option options[16] = { 0 };
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

	/* Clear CoAP pending request */
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

static int process_client_request(void)
{
  struct sockaddr addr;
  socklen_t addr_len = sizeof(addr);
  uint8_t req[MAX_COAP_MSG_LEN];
  
	do {
		int received = recvfrom(sock, req, sizeof(req), 0, &addr, &addr_len);
		if (received < 0) {
			LOG_ERR("Connection error %d", errno);
			return -errno;
		}

		process_coap_request(req, received, &addr, addr_len);
	} while (true);

	return 0;
}

void main(void)
{
	LOG_DBG("Start CoAP-server sample");

	if (!join_coap_multicast_group()) goto quit;

	if (start_coap_server() < 0) goto quit;

	k_delayed_work_init(&retransmit_work, retransmit_request);

	while (true) {
		if (process_client_request() < 0) goto quit;
	}

	LOG_DBG("Done");
	return;

quit:
	LOG_ERR("Quit");
}
