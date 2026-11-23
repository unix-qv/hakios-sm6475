#ifndef _LX_LINUX_OF_ADDRESS_H
#define _LX_LINUX_OF_ADDRESS_H
#include <linux/of.h>
#include <linux/ioport.h>
static inline int of_address_to_resource(struct device_node *np, int index, struct resource *r) { return (-ENODEV); }
#endif
