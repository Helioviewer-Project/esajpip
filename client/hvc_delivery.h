#ifndef HVC_DELIVERY_H
#define HVC_DELIVERY_H
#include "hvc_jpp.h"

/* Optional delivery histories, separate from the retained data-bin cache. */
typedef struct hvc_delivery hvc_delivery;
hvc_delivery *hvc_delivery_window(hvc_delivery **head, uint64_t window);
/* Returns 1 for an unseen range/final flag, 0 for a replay, -1 on allocation failure. */
int hvc_delivery_receive(hvc_delivery *delivery, const hvc_jpp_message *message);
void hvc_delivery_end(hvc_delivery **head, uint64_t window);
void hvc_delivery_free(hvc_delivery *head);
#endif
