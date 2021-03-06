/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

#include <sel4/sel4.h>
#include <string.h>
#include <pico_addressing.h>

#define MAX_UDP_PACKET 4096

/*? macros.show_includes(me.instance.type.includes) ?*/

/*- set ep = alloc('ep', seL4_EndpointObject, write=True, grantreply=True) -*/
/*- set badge = configuration[me.instance.name].get('%s_attributes' % me.interface.name) -*/
/*- if badge is not none -*/
    /*- set badge = badge.strip('"') -*/
    /*- do cap_space.cnode[ep].set_badge(int(badge, 0)) -*/
/*- endif -*/

/* assume a dataport symbols exists */
extern void * /*? me.interface.name?*/_buf;

int /*? me.interface.name ?*/_send(void *p, unsigned int len, uint32_t addr) {
    seL4_SetMR(0, len);
    seL4_SetMR(1, addr);
    if (len > MAX_UDP_PACKET) {
        len = MAX_UDP_PACKET;
    }
    memcpy(/*? me.interface.name?*/_buf, p, len);
    seL4_Call(/*? ep ?*/, seL4_MessageInfo_new(0, 0, 0, 2));
    return len;
}
