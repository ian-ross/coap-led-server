#ifndef _H_COAP_
#define _H_COAP_

#include <net/net_ip.h>
#include <net/coap.h>

// ==> QUESTION: IS THIS DEFINED BY THE PROTOCOL, OR JUST AN
// APPLICATION LIMIT HERE?
#define MAX_COAP_MSG_LEN 256

int send_coap_reply(struct coap_packet *cpkt,
                    const struct sockaddr *addr, socklen_t addr_len);

int well_known_core_get(struct coap_resource *res,
                        struct coap_packet *req, struct sockaddr *addr,
                        socklen_t addr_len);

void start_coap(void);
void stop_coap(void);


#endif
