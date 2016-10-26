/*
 * Copyright 2016, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(D61_BSD)
 */

#ifndef __EMITS_EVENT_IOCTL_COMMANDS_H
#define __EMITS_EVENT_IOCTL_COMMANDS_H

#define EMITS_EVENT_MAGIC 'E'

enum {
    __EMITS_EVENT_EMIT,
};

#define EMITS_EVENT_EMIT _IOW(EMITS_EVENT_MAGIC, __EMITS_EVENT_EMIT, unsigned int)

#endif
