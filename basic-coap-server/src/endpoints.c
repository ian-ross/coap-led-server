// Basic OpenThread CoAP server: CoAP endpoint definitions.

#include <logging/log.h>
LOG_MODULE_DECLARE(basic_coap_server, LOG_LEVEL_DBG);

#include <zephyr.h>
#include <errno.h>

#include <net/coap.h>
#include <net/coap_link_format.h>
#include <net/net_ip.h>

#include "coap.h"
#include "led.h"
#include "utils.h"


// From Section 12.3 of RFC 7252: "text/plain" content format.
static const uint8_t text_plain_format = 0;

// Is our LED on or off? (Not wired up to anything yet...)
static bool led_state = false;


// ----------------------------------------------------------------------
// ENDPOINT HANDLERS

// Endpoint handler for "GET led" CoAP requests.

static int led_get(struct coap_resource *res, struct coap_packet *req,
                   struct sockaddr *addr, socklen_t addr_len) {
  // The only one of these that's used for the request processing is
  // "type" (confirmable or non-confirmable). The others are retrieved
  // here just for debugging output. (The "code" is "GET" and "id" is
  // just a unique message ID.)
  uint8_t code = coap_header_get_code(req);
  uint8_t type = coap_header_get_type(req);
  uint16_t id = coap_header_get_id(req);
  LOG_INF("led_get  type: %u code %u id %u", type, code, id);

  // Allocate space for the reply.
  // ==> NOTE: IT WOULD BE WORTH FINDING OUT IF ZEPHYR HAS ANY SORT OF
  // POOL ALLOCATOR THAT COULD BE USED INSTEAD OF k_malloc. IF WE'RE
  // ALWAYS ALLOCATING BUFFERS OF THE SAME SIZE, WE COULD USE A POOL
  // ALLOCATOR TO AVOID FRAGMENTATION, WHICH MIGHT BE IMPORTANT FOR
  // LONG-RUNNING APPLICATIONS.
  uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
  if (!data) return -ENOMEM;

  // For confirmable messages, we need to send an acknowledgement type
  // packet, though we piggyback all the reply data onto that, so
  // there's not really a separate acknowledgement message.
  type = type == COAP_TYPE_CON ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;

  // Build the reply packet: the "token" is retrieved from the request
  // and copied into the reply packet to allow the client to correlate
  // replies with requests. We use the "Content" response code (2.05)
  // to show that we're sending data back.
  struct coap_packet resp;
  uint8_t tok[8];
  uint8_t toklen = coap_header_get_token(req, tok);
  int r = coap_packet_init(&resp, data, MAX_COAP_MSG_LEN, 1, type, toklen,
                           (uint8_t *)tok, COAP_RESPONSE_CODE_CONTENT, id);
  if (r < 0) goto end;

  // Add a "Content-Format" option to show we're sending back plain
  // text data.
  r = coap_packet_append_option(&resp, COAP_OPTION_CONTENT_FORMAT,
                                &text_plain_format, sizeof(text_plain_format));
  if (r < 0) goto end;

  // Mark that there's a payload (this is a 0xFF byte in place of a
  // normal option marker).
  r = coap_packet_append_payload_marker(&resp);
  if (r < 0) goto end;

  // Construct the reply payload.
  uint8_t payload[40];
  r = snprintk((char *)payload, sizeof(payload), "T:%u C:%u MID:%u LED:%s\n",
               type, code, id, led_state ? "ON" : "OFF");
  if (r < 0) goto end;

  // Append the payload.
  r = coap_packet_append_payload(&resp, (uint8_t *)payload, strlen(payload));
  if (r < 0) goto end;

  // Send the reply: note that this is a function we're providing in
  // the coap.c file, not something that the CoAP API provides. The
  // CoAP API only concerns itself with message processing and
  // formatting. You have to deal with the socket-level side of things
  // yourself.
  r = send_coap_reply(&resp, addr, addr_len);

  // This "end:" label pattern is very common on Zephyr (and Linux
  // kernel) code. It's a clean way to provide guaranteed resource
  // cleanup on error exits in C. Don't believe people who say that
  // "goto" is dead!
end:
  k_free(data);
  return r;
}


// Endpoint handler for "POST led" CoAP requests.

static int led_post(struct coap_resource *res, struct coap_packet *req,
                    struct sockaddr *addr, socklen_t addr_len) {
  // The only one of these that's used for the request processing is
  // "type" (confirmable or non-confirmable). The others are retrieved
  // here just for debugging output. (The "code" is "POST" and "id" is
  // just a unique message ID.)
  uint8_t code = coap_header_get_code(req);
  uint8_t type = coap_header_get_type(req);
  uint16_t id = coap_header_get_id(req);
  LOG_INF("led_post  type: %u code %u id %u", type, code, id);

  // Retrieve the POST payload.
  uint16_t payload_len;
  const uint8_t *payload = coap_packet_get_payload(req, &payload_len);
  if (payload) {
    hexdump("POST Payload", payload, payload_len);
  } else {
    LOG_INF("POST with no payload!");
  }

  // Process the payload. If it's ASCII '1' or binary 1, switch the
  // LED on. If it's ASCII '0' or binary 0, switch the LED off.
  // Otherwise ignore it.
  if (payload_len >= 1) {
    if (payload[0] == '1' || payload[0] == 1) {
      led_state = true;
      led_on();
    } else if (payload[0] == '0' || payload[0] == 0) {
      led_state = false;
      led_off();
    }
  }

  // Allocate space for the reply.
  uint8_t *data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
  if (!data) return -ENOMEM;

  // For confirmable messages, we need to send an acknowledgement type
  // packet, though we piggyback all the reply data onto that, so
  // there's not really a separate acknowledgement message.
  type = type == COAP_TYPE_CON ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;

  // Build the reply packet: the "token" is retrieved from the request
  // and copied into the reply packet to allow the client to correlate
  // replies with requests. We use the "Changed" response code (2.04)
  // to show that we may have modified the status of the requested
  // resource.
  struct coap_packet resp;
  uint8_t tok[8];
  uint8_t toklen = coap_header_get_token(req, tok);
  int r = coap_packet_init(&resp, data, MAX_COAP_MSG_LEN, 1, type, toklen,
                           (uint8_t *)tok, COAP_RESPONSE_CODE_CHANGED, id);
  if (r < 0) goto end;

  // Add a "Content-Format" option to show we're sending back plain
  // text data.
  r = coap_packet_append_option(&resp, COAP_OPTION_CONTENT_FORMAT,
                                &text_plain_format, sizeof(text_plain_format));
  if (r < 0) goto end;

  // Mark that there's a payload (this is a 0xFF byte in place of a
  // normal option marker).
  r = coap_packet_append_payload_marker(&resp);
  if (r < 0) goto end;

  // Construct the reply payload.
  uint8_t rpayload[40];
  r = snprintk((char *)rpayload, sizeof(rpayload), "T:%u C:%u MID:%u LED:%s\n",
               type, code, id, led_state ? "ON" : "OFF");
  if (r < 0) goto end;

  // Append the payload.
  r = coap_packet_append_payload(&resp, (uint8_t *)rpayload, strlen(rpayload));
  if (r < 0) goto end;

  // Send the reply.
  r = send_coap_reply(&resp, addr, addr_len);

  // Clean up on exit.
end:
  k_free(data);
  return r;
}


// ----------------------------------------------------------------------
// CoAP RESOURCE DEFINITIONS

// URI path for our LED resource.
static const char *const led_path[] = {"led", NULL};

struct coap_resource coap_resources[] = {
  // Include the ".well-known/core" resource: this is handled by a
  // common function defined in coap.c.
  { .get = well_known_core_get,
    .path = COAP_WELL_KNOWN_CORE_PATH, },

  // Our LED resource: we have GET and POST endpoints, and specify the
  // URI path to the resource.
  { .get = led_get,
    .post = led_post,
    .path = led_path },

  // End marker.
  {},
};
