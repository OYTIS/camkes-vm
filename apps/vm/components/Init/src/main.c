/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(NICTA_GPL)
 */

/*The init thread for the vmm system*/

#include <stdio.h>
#include <stdint.h>
#include <autoconf.h>

#include <sel4platsupport/arch/io.h>
#include <sel4utils/vspace.h>
#include <sel4utils/stack.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <simple/simple_helpers.h>
#include <vka/capops.h>

#include <Init.h>

#include "vmm/vmm.h"
#include "vmm/driver/pci_helper.h"
#include "vmm/platform/ioports.h"
#include "vmm/platform/boot.h"
#include "vmm/platform/boot_guest.h"
#include "vmm/platform/guest_vspace.h"

#include "vm.h"
#include "virtio_net.h"

#define BRK_VIRTUAL_SIZE 400000000

reservation_t muslc_brk_reservation;
void *muslc_brk_reservation_start;
vspace_t  *muslc_this_vspace;
static sel4utils_res_t muslc_brk_reservation_memory;

seL4_CPtr intready_aep();
seL4_CPtr haveint_aep = 0;

static seL4_CPtr get_async_event_aep() {
    return intready_aep();
}

void platsupport_serial_setup_simple(vspace_t *vspace, simple_t *simple, vka_t *vka) {
    printf("Ignoring call to %s\n", __FUNCTION__);
    printf("Assuming camkes sorted this out\n");
}

void camkes_make_simple(simple_t *simple);

static allocman_t *allocman;
static char allocator_mempool[8388608];
static simple_t camkes_simple;
static vka_t vka;
static vspace_t vspace;
static sel4utils_alloc_data_t vspace_data;
static vmm_t vmm;

/* custom allocator interface for attempting to allocate frames
 * from special frame only memory */
typedef struct proxy_vka {
    uintptr_t last_paddr;
    int have_mem;
    allocman_t *allocman;
    vka_t regular_vka;
} proxy_vka_t;

static proxy_vka_t proxy_vka;

typedef struct ut_node {
    int frame;
    uint32_t cookie;
} ut_node_t;

int proxy_vka_cspace_alloc(void *data, seL4_CPtr *res) {
    proxy_vka_t *vka = (proxy_vka_t*)data;
    return vka_cspace_alloc(&vka->regular_vka, res);
}

void proxy_vka_cspace_make_path(void *data, seL4_CPtr slot, cspacepath_t *res) {
    proxy_vka_t *vka = (proxy_vka_t*)data;
    vka_cspace_make_path(&vka->regular_vka, slot, res);
}

void proxy_vka_cspace_free(void *data, seL4_CPtr slot) {
    proxy_vka_t *vka = (proxy_vka_t*)data;
    vka_cspace_free(&vka->regular_vka, slot);
}

int proxy_vka_utspace_alloc(void *data, const cspacepath_t *dest, seL4_Word type, seL4_Word size_bits, uint32_t *res) {
    proxy_vka_t *vka = (proxy_vka_t*)data;
    int error;
    uint32_t cookie;
    ut_node_t *node = allocman_mspace_alloc(vka->allocman, sizeof(*node), &error);
    if (!node) {
        return -1;
    }
    if (type == seL4_IA32_4K && vka->have_mem) {
        error = simple_get_frame_cap(&camkes_simple, (void*)vka->last_paddr, seL4_PageBits, (cspacepath_t*)dest);
        if (error) {
            vka->have_mem = 0;
        } else {
            node->frame = 1;
            node->cookie = vka->last_paddr;
            vka->last_paddr += PAGE_SIZE_4K;
            return 0;
        }
    }
    error = vka_utspace_alloc(&vka->regular_vka, dest, type, size_bits, &cookie);
    if (!error) {
        node->frame = 0;
        node->cookie = cookie;
        *res = (uint32_t)node;
        return  0;
    }
    free(node);
    return error;
}

void proxy_vka_utspace_free(void *data, seL4_Word type, seL4_Word size_bits, uint32_t target) {
    proxy_vka_t *vka = (proxy_vka_t*)data;
    ut_node_t *node = (ut_node_t*)target;
    if (!node->frame) {
        vka_utspace_free(&vka->regular_vka, type, size_bits, node->cookie);
    }
    allocman_mspace_free(vka->allocman, node, sizeof(*node));
}

uintptr_t proxy_vka_utspace_paddr(void *data, uint32_t target, seL4_Word type, seL4_Word size_bits) {
    proxy_vka_t *vka = (proxy_vka_t*)data;
    ut_node_t *node = (ut_node_t*)target;
    if (node->frame) {
        return node->cookie;
    } else {
        return proxy_vka_utspace_paddr(&vka->regular_vka, node->cookie, type, size_bits);
    }
}

static void make_proxy_vka(vka_t *vka, allocman_t *allocman) {
#ifdef VM_CONFIGURATION_EXTRA_RAM
    proxy_vka_t *proxy = &proxy_vka;
    proxy->allocman = allocman;
    allocman_make_vka(&proxy->regular_vka, allocman);
#define GET_EXTRA_RAM_OUTPUT(num, iteration, data) \
    if (strcmp(get_instance_name(),BOOST_PP_STRINGIZE(vm##iteration)) == 0) { \
        proxy->last_paddr = BOOST_PP_TUPLE_ELEM(0, BOOST_PP_CAT(VM_CONFIGURATION_EXTRA_RAM_,iteration)()); \
        proxy->have_mem = 1; \
    } \
    /**/
    BOOST_PP_REPEAT(VM_NUM_GUESTS, GET_EXTRA_RAM_OUTPUT, _)
    *vka = (vka_t) {
        proxy,
        proxy_vka_cspace_alloc,
        proxy_vka_cspace_make_path,
        proxy_vka_utspace_alloc,
        proxy_vka_cspace_free,
        proxy_vka_utspace_free,
        proxy_vka_utspace_paddr
    };
#else
    allocman_make_vka(vka, allocman);
#endif
}

void pit_pre_init(void);
void i8259_pre_init(void);

void pre_init(void) {
    int error;

    set_putchar(putchar_putchar);

    /* Camkes adds nothing to our address space, so this array is empty */
    void *existing_frames[] = {
        NULL
    };
    camkes_make_simple(&camkes_simple);

    /* Initialize allocator */
    allocman = bootstrap_use_current_1level(
            simple_get_cnode(&camkes_simple),
            simple_get_cnode_size_bits(&camkes_simple),
            simple_last_valid_cap(&camkes_simple) + 1,
            BIT(simple_get_cnode_size_bits(&camkes_simple)),
            sizeof(allocator_mempool), allocator_mempool
    );
    assert(allocman);
    error = allocman_add_simple_untypeds(allocman, &camkes_simple);
    make_proxy_vka(&vka, allocman);

    /* Initialize the vspace */
    error = sel4utils_bootstrap_vspace(&vspace, &vspace_data,
            simple_get_init_cap(&camkes_simple, seL4_CapInitThreadPD), &vka, NULL, NULL, existing_frames);
    assert(!error);

    sel4utils_reserve_range_no_alloc(&vspace, &muslc_brk_reservation_memory, BRK_VIRTUAL_SIZE, seL4_AllRights, 1, &muslc_brk_reservation_start);
    muslc_this_vspace = &vspace;
    muslc_brk_reservation = (reservation_t){.res = &muslc_brk_reservation_memory};

    pit_pre_init();
    i8259_pre_init();
}

typedef struct memory_range {
    uintptr_t base;
    size_t size;
} memory_range_t;

#ifdef CONFIG_APP_CAMKES_VM_GUEST_DMA_ONE_TO_ONE
static memory_range_t guest_ram_regions[] = {
    /* Define one of the standard ram regions
     * Making sure it is page aligned so it will
     * work from device memory */
    {0x8000, 0x9f000 - 0x8000},
};
#else
static memory_range_t guest_ram_regions[] = {
    /* Allocate all the standard low memory areas */
    {0x500, 0x7c00 - 0x500},
    {0x7e00, 0x80000 - 0x7e00},
    {0x80000, 0x9fc00 - 0x80000},
};
#endif

static memory_range_t guest_fake_devices[] = {
    {0xf0000, 0x10000}, // DMI
    {0xe0000, 0x10000}, // PCI BIOS
    {0xc0000, 0xc8000 - 0xc0000}, // VIDEO BIOS
    {0xc8000, 0xe0000 - 0xc8000}, // Mapped hardware and MISC
#ifdef CONFIG_APP_CAMKES_VM_GUEST_DMA_ONE_TO_ONE
    /* Fake a BIOS page. Done in one_to_one case as we don't
     * allocate a low memory frame already */
    {0x0, 0x1000},
    /* Same for EBDA region */
    {0x9f000, 0x1000},
#endif
};

typedef struct host_pci_device {
    uint16_t ven;
    uint16_t dev;
    int fun;
    int irq;
} host_pci_device_t;

#define GUEST_PASSTHROUGH_OUTPUT(num, iteration, data) \
    host_pci_device_t guest_passthrough_devices_vm##iteration[] = { \
        BOOST_PP_CAT(VM_GUEST_PASSTHROUGH_DEVICES_, iteration)() \
    }; \
    /**/
BOOST_PP_REPEAT(VM_NUM_GUESTS, GUEST_PASSTHROUGH_OUTPUT, _)

/* this struct holds a bunch of compile time data for dealing with extra
 * async notifications for native devices. everything here is known at
 * build time but it is convenient to be able to inspect it programatically
 * instead of through insane macros
 */
typedef struct device_notify {
    /* which extra bit to badge */
    uint32_t badge_bit;
    /* the function (as described by the user in the configuration) to call
     * when the message has been received. */
    void (*func)();
    /* record of the name of the proxy function. this is what is actually
     * registered to camkes async notification */
    void (*proxy)();
    /* this is a reference to the camkes 'x_reg_callback' function, where
     * x is the async interface name. It is more convenient to have a reference
     * here than try and generate the full name where need to invoke it */
    int (*reg)(void (*)(void*),void*);
    /* pointer to cptr. this allows us to point at the final capability we
     * generate. doing this via a pointer is convenient for the init code */
    seL4_CPtr *cap;
} device_notify_t;
#define PROXY_CAP_NAME(iter, badge) \
    CAT(CAT(CAT(proxy_cap_,iter),_), badge) \
    /**/
#define ASYNC_PROXY(r, data, elem) \
    static seL4_CPtr PROXY_CAP_NAME(data, BOOST_PP_TUPLE_ELEM(0, elem)); \
    void CAT(proxy_notify_,BOOST_PP_TUPLE_ELEM(0, elem))() { \
        CAT(BOOST_PP_TUPLE_ELEM(2, elem),_reg_callback)(CAT(proxy_notify_,BOOST_PP_TUPLE_ELEM(0, elem)), NULL); \
        seL4_Notify(PROXY_CAP_NAME(data, BOOST_PP_TUPLE_ELEM(0, elem)), 0); \
    } \
    /**/
#define ASYNC_DEVICE_MEMBER(r, data, elem) \
    {BOOST_PP_TUPLE_ELEM(0, elem), BOOST_PP_TUPLE_ELEM(1, elem), CAT(proxy_notify_,BOOST_PP_TUPLE_ELEM(0, elem)), CAT(BOOST_PP_TUPLE_ELEM(2, elem),_reg_callback), &PROXY_CAP_NAME(data, BOOST_PP_TUPLE_ELEM(0, elem))}, \
    /**/
#define ASYNC_DEVICE_OUTPUT(num, iteration, data) \
    BOOST_PP_LIST_FOR_EACH(ASYNC_PROXY, iteration, BOOST_PP_TUPLE_TO_LIST(CAT(VM_ASYNC_DEVICE_BADGES_, iteration)())) \
    device_notify_t device_notify_vm##iteration[] = { \
        BOOST_PP_LIST_FOR_EACH(ASYNC_DEVICE_MEMBER, iteration, BOOST_PP_TUPLE_TO_LIST(CAT(VM_ASYNC_DEVICE_BADGES_, iteration)())) \
    }; \
    /**/
BOOST_PP_REPEAT(VM_NUM_GUESTS, ASYNC_DEVICE_OUTPUT, _)

#define DEVICE_INIT_OUTPUT(num, iteration, data) \
    static void(*device_init_fn_vm##iteration[])(vmm_t *) = { \
        CAT(VM_DEVICE_INIT_FN_, iteration)() \
    }; \
    /**/
BOOST_PP_REPEAT(VM_NUM_GUESTS, DEVICE_INIT_OUTPUT, _)

/* Wrappers for passing PCI config space calls to camkes */
static uint8_t camkes_pci_read8(void *cookie, vmm_pci_address_t addr, unsigned int offset) {
    return pci_config_read8(addr.bus, addr.dev, addr.fun, offset);
}
static uint16_t camkes_pci_read16(void *cookie, vmm_pci_address_t addr, unsigned int offset) {
    return pci_config_read16(addr.bus, addr.dev, addr.fun, offset);
}
static uint32_t camkes_pci_read32(void *cookie, vmm_pci_address_t addr, unsigned int offset) {
    return pci_config_read32(addr.bus, addr.dev, addr.fun, offset);
}
static void camkes_pci_write8(void *cookie, vmm_pci_address_t addr, unsigned int offset, uint8_t val) {
    pci_config_write8(addr.bus, addr.dev, addr.fun, offset, val);
}
static void camkes_pci_write16(void *cookie, vmm_pci_address_t addr, unsigned int offset, uint16_t val) {
    pci_config_write16(addr.bus, addr.dev, addr.fun, offset, val);
}
static void camkes_pci_write32(void *cookie, vmm_pci_address_t addr, unsigned int offset, uint32_t val) {
    pci_config_write32(addr.bus, addr.dev, addr.fun, offset, val);
}

vmm_pci_config_t make_camkes_pci_config() {
    return (vmm_pci_config_t) {
        .cookie = NULL,
        .ioread8 = camkes_pci_read8,
        .ioread16 = camkes_pci_read16,
        .ioread32 = camkes_pci_read32,
        .iowrite8 = camkes_pci_write8,
        .iowrite16 = camkes_pci_write16,
        .iowrite32 = camkes_pci_write32
    };
}

typedef struct ioport_desc {
    uint16_t start_port;
    uint16_t end_port;
    ioport_in_fn port_in;
    ioport_out_fn port_out;
    const char *desc;
} ioport_desc_t;

static int camkes_i8259_port_in(void *cookie, unsigned int port_no, unsigned int size, unsigned int *result) {
    return i8259_port_in(port_no, size, result);
}

static int camkes_i8259_port_out(void *cookie, unsigned int port_no, unsigned int size, unsigned int value) {
    return i8259_port_out(port_no, size, value);
}

static int camkes_cmos_port_in(void *cookie, unsigned int port_no, unsigned int size, unsigned int *result) {
    return cmos_port_in(port_no, size, result);
}

static int camkes_cmos_port_out(void *cookie, unsigned int port_no, unsigned int size, unsigned int value) {
    return cmos_port_out(port_no, size, value);
}

static int camkes_serial_port_in(void *cookie, unsigned int port_no, unsigned int size, unsigned int *result) {
    return serial_port_in(port_no, size, result);
}

static int camkes_serial_port_out(void *cookie, unsigned int port_no, unsigned int size, unsigned int value) {
    return serial_port_out(port_no, size, value);
}

int i8254_port_in(void *cookie, unsigned int port_no, unsigned int size, unsigned int *result);
int i8254_port_out(void *cookie, unsigned int port_no, unsigned int size, unsigned int value);

ioport_desc_t ioport_handlers[] = {
    {X86_IO_SERIAL_1_START,   X86_IO_SERIAL_1_END,   camkes_serial_port_in, camkes_serial_port_out, "COM1 Serial Port"},
//    {X86_IO_SERIAL_3_START,   X86_IO_SERIAL_3_END,   NULL, NULL, "COM3 Serial Port"},
    {X86_IO_PIC_1_START,      X86_IO_PIC_1_END,      camkes_i8259_port_in, camkes_i8259_port_out, "8259 Programmable Interrupt Controller (1st, Master)"},
    {X86_IO_PIC_2_START,      X86_IO_PIC_2_END,      camkes_i8259_port_in, camkes_i8259_port_out, "8259 Programmable Interrupt Controller (2nd, Slave)"},
    {X86_IO_ELCR_START,       X86_IO_ELCR_END,       camkes_i8259_port_in, camkes_i8259_port_out, "ELCR (edge/level control register) for IRQ line"},
    /* PCI config requires a cookie and is specced dynamically in code */
//    {X86_IO_PCI_CONFIG_START, X86_IO_PCI_CONFIG_END, vmm_pci_io_port_in, vmm_pci_io_port_out, "PCI Configuration"},
    {X86_IO_RTC_START,        X86_IO_RTC_END,        camkes_cmos_port_in, camkes_cmos_port_out, "CMOS Registers / RTC Real-Time Clock / NMI Interrupts"},
    {X86_IO_PIT_START,        X86_IO_PIT_END,        i8254_port_in, i8254_port_out, "8253/8254 Programmable Interval Timer"},
//    {X86_IO_PS2C_START,       X86_IO_PS2C_END,       NULL, NULL, "8042 PS/2 Controller"},
//    {X86_IO_POS_START,        X86_IO_POS_END,        NULL, NULL, "POS Programmable Option Select (PS/2)"},

#if 0
    {0xC000,                  0xF000,                NULL, NULL, "PCI Bus IOPort Mapping Space"},
    {0x1060,                  0x1070,                NULL, NULL, "IDE controller"},
    {0x01F0,                  0x01F8,                NULL, NULL, "Primary IDE controller"},
    {0x0170,                  0x0178,                NULL, NULL, "Secondary IDE controller"},
    {0x3f6,                   0x03f7,                NULL, NULL, "Additional ATA register"},
    {0x376,                   0x0377,                NULL, NULL, "Additional ATA register"},
    {0x3b0,                   0x3df,                 NULL, NULL, "IBM VGA"},

    {0x80,                    0x80,                  NULL, NULL, "DMA IOPort timer"},
#endif

#if 0
    {0x164e,                  0x164f,                NULL, NULL, "Serial Configuration Registers"},
#endif
};

#define GUEST_IOPORT_OUTPUT(r, data, elem) \
    BOOST_PP_EXPAND( \
        BOOST_PP_REM BOOST_PP_IF( \
            BOOST_PP_TUPLE_ELEM(2, elem),\
            ({BOOST_PP_TUPLE_ELEM(0,elem),BOOST_PP_TUPLE_ELEM(1,elem),NULL,NULL,BOOST_PP_TUPLE_ELEM(3,elem)},), \
            () \
        ) \
    ) \
    /**/
#define GUEST_IOPORTS_OUTPUT(num, iteration, data) \
    ioport_desc_t ioport_handlers_vm##iteration[] = { \
        BOOST_PP_LIST_FOR_EACH(GUEST_IOPORT_OUTPUT, iteration, BOOST_PP_TUPLE_TO_LIST(CAT(VM_CONFIGURATION_IOPORT_, iteration)())) \
    }; \
    /**/
#define SOME_OP(num) \
    BOOST_PP_LIST_FOR_EACH(GUEST_IOPORTS_OUTPUT, num, BOOST_PP_TUPLE_TO_LIST(CAT(VM_CONFIGURATION_IOPORT_, num)())) \
    /**/

BOOST_PP_REPEAT(VM_NUM_GUESTS, GUEST_IOPORTS_OUTPUT, _)

int pci_config_io_in(void *cookie, uint32_t port, int io_size, uint32_t *result) {
    uint32_t *conf_port_addr = (uint32_t*)cookie;
    uint8_t offset;
    if (port >= PCI_CONF_PORT_ADDR && port < PCI_CONF_PORT_ADDR_END) {
        offset = port - PCI_CONF_PORT_ADDR;
        /* Emulate read addr */
        *result = 0;
        memcpy(result, ((char*)conf_port_addr) + offset, io_size);
        return 0;
    }
    if (port < PCI_CONF_PORT_DATA || port + io_size > PCI_CONF_PORT_DATA_END) {
        return -1;
    }
    offset = port - PCI_CONF_PORT_DATA;
    /* Make an address out of the config */
    uint8_t bus, dev, fun, reg;
    bus = ((*conf_port_addr) >> 16) & MASK(8);
    dev = ((*conf_port_addr) >> 11) & MASK(5);
    fun = ((*conf_port_addr) >> 8) & MASK(3);
    reg = (*conf_port_addr) & MASK(8);
    reg += offset;
    /* Read the real config */
    switch(io_size) {
    case 1:
        *result=pci_config_read8(bus, dev, fun, reg);
        break;
    case 2:
        *result=pci_config_read16(bus, dev, fun, reg);
        break;
    case 4:
        *result=pci_config_read32(bus, dev, fun, reg);
        break;
    default:
        assert(!"Invalid size");
        return -1;
    }
    return 0;
}

int pci_config_io_out(void *cookie, uint32_t port, int io_size, uint32_t val) {
    uint32_t *conf_port_addr = (uint32_t*)cookie;
    uint8_t offset;
    if (port >= PCI_CONF_PORT_ADDR && port < PCI_CONF_PORT_ADDR_END) {
        offset = port - PCI_CONF_PORT_ADDR;
        /* Emulate read addr */
        val &= ~MASK(2);
        memcpy(((char*)conf_port_addr) + offset, &val, io_size);
        return 0;
    }
    if (port < PCI_CONF_PORT_DATA || port + io_size > PCI_CONF_PORT_DATA_END) {
        return -1;
    }
    offset = port - PCI_CONF_PORT_DATA;
    /* Make an address out of the config */
    uint8_t bus, dev, fun, reg;
    bus = ((*conf_port_addr) >> 16) & MASK(8);
    dev = ((*conf_port_addr) >> 11) & MASK(5);
    fun = ((*conf_port_addr) >> 8) & MASK(3);
    reg = (*conf_port_addr) & MASK(8);
    reg += offset;
    /* Read the real config */
    switch(io_size) {
    case 1:
        pci_config_write8(bus, dev, fun, reg, val);
        break;
    case 2:
        pci_config_write16(bus, dev, fun, reg, val);
        break;
    case 4:
        pci_config_write32(bus, dev, fun, reg, val);
        break;
    default:
        assert(!"Invalid size");
        return -1;
    }
    return 0;
}

ps_io_port_ops_t make_pci_io_ops() {
    return (ps_io_port_ops_t) { .cookie = malloc(sizeof(uint32_t)),
                                 .io_port_in_fn = pci_config_io_in,
                                 .io_port_out_fn = pci_config_io_out
                               };
}

static int device_notify_list_len = 0;
static device_notify_t *device_notify_list = NULL;

void pit_timer_interrupt(void);

static int handle_async_event(seL4_Word badge) {
    int ret = 1;
    if (badge & BIT(27)) {
        if ( (badge & VM_INT_MAN_BADGE) == VM_INT_MAN_BADGE) {
            ret = 0;
        }
        if ( (badge & VM_PIT_TIMER_BADGE) == VM_PIT_TIMER_BADGE) {
            pit_timer_interrupt();
        }
        for (int i = 0; i < device_notify_list_len; i++) {
            uint32_t device_badge = BIT(27) | BIT(device_notify_list[i].badge_bit);
            if ( (badge & device_badge) == device_badge) {
                assert(device_notify_list[i].func);
                device_notify_list[i].func();
            }
        }
    }
    /* return 0 to indicate an interrupt occured */
    return ret;
}

static void make_async_aep() {
    cspacepath_t async_path;
    int error;
    seL4_CPtr async_event_aep = intready_aep();
    vka_cspace_make_path(&vka, async_event_aep, &async_path);
    cspacepath_t haveint_path;
    error = vka_cspace_alloc_path(&vka, &haveint_path);
    assert(!error);
    error = vka_cnode_mint(&haveint_path, &async_path, seL4_AllRights, seL4_CapData_Badge_new(VM_INT_MAN_BADGE));
    assert(!error);
    haveint_aep = haveint_path.capPtr;
    for (int i = 0; i < device_notify_list_len; i++) {
        cspacepath_t badge_path;
        error = vka_cspace_alloc_path(&vka, &badge_path);
        assert(!error);
        error = vka_cnode_mint(&badge_path, &async_path, seL4_AllRights, seL4_CapData_Badge_new(BIT(27) | BIT(device_notify_list[i].badge_bit)));
        assert(!error);
        *device_notify_list[i].cap = badge_path.capPtr;
        device_notify_list[i].reg(device_notify_list[i].proxy, NULL);
    }
}

int main_continued(void) {
    int error;
    int i;
    const char *kernel_image = NULL;
    const char *initrd_image = NULL;
    const char *kernel_cmdline = NULL;
    const char *kernel_relocs = NULL;
    int have_initrd = 0;
    ps_io_port_ops_t ioops;
    int iospace_domain;
    ioport_desc_t *vm_ioports;
    int num_vm_ioports;
    void (**device_init_list)(vmm_t*) = NULL;
    int device_init_list_len = 0;

    rtc_time_date_t time_date = rtc_time_date();
    printf("Starting VM %s at: %04d:%02d:%02d %02d:%02d:%02d\n", get_instance_name(), time_date.year, time_date.month, time_date.day, time_date.hour, time_date.minute, time_date.second);

    ioops = make_pci_io_ops();
    libpci_scan(ioops);

    /* Construct a new VM */
    platform_callbacks_t callbacks = (platform_callbacks_t) {
        .get_interrupt = IntManager_get_interrupt,
        .has_interrupt = IntManager_has_interrupt,
        .do_async = handle_async_event,
        .get_async_event_aep = get_async_event_aep
    };
    error = vmm_init(&vmm, camkes_simple, vka, vspace, callbacks);
    assert(!error);

    /* First we initialize any host information */
    error = vmm_init_host(&vmm);
    assert(!error);

    /* Early init of guest. We populate everything later */
    error = vmm_init_guest(&vmm, CONFIG_CAMKES_DEFAULT_PRIORITY);
    assert(!error);

    /* Do per vm configuration */
    int num_guest_passthrough_devices;
    host_pci_device_t *guest_passthrough_devices;
#define PER_VM_CONFIG(num, iteration, data) \
    if (strcmp(get_instance_name(), BOOST_PP_STRINGIZE(vm##iteration)) == 0) { \
        num_guest_passthrough_devices = ARRAY_SIZE(guest_passthrough_devices_vm##iteration); \
        guest_passthrough_devices = guest_passthrough_devices_vm##iteration; \
        vm_ioports = ioport_handlers_vm##iteration; \
        num_vm_ioports = ARRAY_SIZE(ioport_handlers_vm##iteration); \
        kernel_image = BOOST_PP_CAT(VM_GUEST_IMAGE_, iteration)(); \
        kernel_cmdline = BOOST_PP_CAT(VM_GUEST_IMAGE_, iteration)() " " BOOST_PP_CAT(VM_GUEST_CMDLINE_, iteration)(); \
        initrd_image = BOOST_PP_CAT(VM_GUEST_ROOTFS_, iteration)(); \
        have_initrd = !(strcmp(initrd_image, "") == 0); \
        kernel_relocs = BOOST_PP_CAT(VM_GUEST_RELOCS_, iteration)(); \
        iospace_domain = BOOST_PP_CAT(VM_GUEST_IOSPACE_DOMAIN_, iteration)(); \
        device_notify_list = BOOST_PP_CAT(device_notify_vm, iteration); \
        device_notify_list_len = ARRAY_SIZE(BOOST_PP_CAT(device_notify_vm, iteration)); \
        device_init_list = BOOST_PP_CAT(device_init_fn_vm, iteration); \
        device_init_list_len = ARRAY_SIZE(BOOST_PP_CAT(device_init_fn_vm, iteration)); \
    } \
    /**/
    BOOST_PP_REPEAT(VM_NUM_GUESTS, PER_VM_CONFIG, _)

    make_async_aep();

#ifdef CONFIG_APP_CAMKES_VM_GUEST_DMA_IOMMU
    /* Do early device discovery and find any relevant PCI busses that
     * need to get added */
    for (i = 0; i < num_guest_passthrough_devices; i++) {
        libpci_device_t *devices[64];
        /* Retrieve device information from PCI component */
        int num = libpci_find_device_all(guest_passthrough_devices[i].ven, guest_passthrough_devices[i].dev, devices);
        for (int j = 0; j < num; j++) {
            libpci_device_t *device = devices[j];
            if (guest_passthrough_devices[i].fun != -1 && guest_passthrough_devices[i].fun != device->fun) {
                continue;
            }
            LOG_INFO("Adding PCI device %02x:%02x.%d to guest IOSpace list", device->bus, device->dev, device->fun);
            /* Find the IOSpace cap */
            cspacepath_t iospace;
            error = vka_cspace_alloc_path(&vmm.vka, &iospace);
            assert(!error);
            error = simple_get_iospace(&vmm.host_simple, iospace_domain, (device->bus << 8) | (device->dev << 3) | device->fun, &iospace);
            assert(error == seL4_NoError);
            /* Assign to the guest vspace */
            error = vmm_guest_vspace_add_iospace(&vmm.guest_mem.vspace, iospace.capPtr);
            assert(!error);
        }
    }
#endif

    /* Do we need to do any early reservations of guest address space? */
#ifdef CONFIG_APP_CAMKES_VM_GUEST_DMA_ONE_TO_ONE
    for (i = 0; i < ARRAY_SIZE(guest_ram_regions); i++) {
        /* try and put a device here */
        error = vmm_map_guest_device_at(&vmm, guest_ram_regions[i].base, guest_ram_regions[i].base, guest_ram_regions[i].size);
        assert(!error);
    }
    /* We now run the normal loop to allocate ram regions. Because the addresses are
     * already in the vspace no additional frames will get mapped, but it will result
     * in ram regions being defined for the guest */
#endif
    for (i = 0; i < ARRAY_SIZE(guest_ram_regions); i++) {
        error = vmm_alloc_guest_ram_at(&vmm, guest_ram_regions[i].base, guest_ram_regions[i].size);
        assert(!error);
    }

    for (i = 0; i < ARRAY_SIZE(guest_fake_devices); i++) {
        error = vmm_alloc_guest_device_at(&vmm, guest_fake_devices[i].base, guest_fake_devices[i].size);
        assert(!error);
    }

    /* Allocate guest ram. This is the main memory that the guest will actually get
     * told exists. Other memory may get allocated and mapped into the guest */
    int paddr_is_vaddr;
#ifdef CONFIG_APP_CAMKES_VM_GUEST_DMA_ONE_TO_ONE
    paddr_is_vaddr = 1;
#else
    paddr_is_vaddr = 0;
#endif
    error = vmm_alloc_guest_ram(&vmm, CONFIG_APP_CAMKES_VM_GUEST_RAM * 1024 * 1024, paddr_is_vaddr);
    assert(!error);

    /* Perform device discovery and give passthrough device information */
    for (i = 0; i < num_guest_passthrough_devices; i++) {
        libpci_device_t *devices[64];
        /* Retrieve device information from PCI component */
        int num = libpci_find_device_all(guest_passthrough_devices[i].ven, guest_passthrough_devices[i].dev, devices);
        for (int j = 0; j < num; j++) {
            libpci_device_t *device = devices[j];
            if (guest_passthrough_devices[i].fun != -1 && guest_passthrough_devices[i].fun != device->fun) {
                continue;
            }
            /* Allocate resources */
            vmm_pci_bar_t bars[6];
            int num_bars = vmm_pci_helper_map_bars(&vmm, &device->cfg, bars);
            assert(num_bars >= 0);
            vmm_pci_entry_t entry = vmm_pci_create_passthrough((vmm_pci_address_t){device->bus, device->dev, device->fun}, make_camkes_pci_config());
            if (num_bars > 0) {
                entry = vmm_pci_create_bar_emulation(entry, num_bars, bars);
            }
            if (guest_passthrough_devices[i].irq != -1) {
                entry = vmm_pci_create_irq_emulation(entry, guest_passthrough_devices[i].irq);
            }
            entry = vmm_pci_no_msi_cap_emulation(entry);
            error = vmm_pci_add_entry(&vmm.pci, entry, NULL);
            assert(!error);
        }
    }

    for (i = 0; i < device_init_list_len; i++) {
        device_init_list[i](&vmm);
    }

    /* Add any IO ports */
    for (i = 0; i < ARRAY_SIZE(ioport_handlers); i++) {
        if (ioport_handlers[i].port_in) {
            error = vmm_io_port_add_handler(&vmm.io_port, ioport_handlers[i].start_port, ioport_handlers[i].end_port, NULL, ioport_handlers[i].port_in, ioport_handlers[i].port_out, ioport_handlers[i].desc);
            assert(!error);
        } else {
            error = vmm_io_port_add_passthrough(&vmm.io_port, ioport_handlers[i].start_port, ioport_handlers[i].end_port, ioport_handlers[i].desc);
            assert(!error);
        }
    }
    for (i = 0; i < num_vm_ioports; i++) {
        /* These IOPort additions are redundant as they seem to all be for PCI bars that
         * will already get added by vmm_pci_helper_map_bars. Just going to leave this
         * commented out for now */
#if 0
        if (vm_ioports[i].port_in) {
            error = vmm_io_port_add_handler(&vmm.io_port, vm_ioports[i].start_port, vm_ioports[i].end_port, NULL, vm_ioports[i].port_in, vm_ioports[i].port_out, vm_ioports[i].desc);
            assert(!error);
        } else {
            error = vmm_io_port_add_passthrough(&vmm.io_port, vm_ioports[i].start_port, vm_ioports[i].end_port, vm_ioports[i].desc);
            assert(!error);
        }
#endif
    }
    /* config start and end encomposes both addr and data ports */
    error = vmm_io_port_add_handler(&vmm.io_port, X86_IO_PCI_CONFIG_START, X86_IO_PCI_CONFIG_END, &vmm.pci, vmm_pci_io_port_in, vmm_pci_io_port_out, "PCI Configuration Space");
    assert(!error);

    /* Load in an elf file. Hard code alignment to 4M */
    /* TODO: use proper libc file handles with the CPIO file system */
    error = vmm_load_guest_elf(&vmm, kernel_image, BIT(seL4_4MBits));
    assert(!error);

    /* Relocate the elf */
    vmm_plat_guest_elf_relocate(&vmm, kernel_relocs);

    /* Add a boot module */
    if (have_initrd) {
        error = vmm_guest_load_boot_module(&vmm, initrd_image);
        assert(!error);
    }

    vmm_plat_init_guest_boot_structure(&vmm, kernel_cmdline);

    /* Final VMM setup now that everything is defined and loaded */
    error = vmm_finalize(&vmm);
    assert(!error);

//    vmm_exit_init();
    /* Now go run the event loop */
    vmm_run(&vmm);

    return 0;
}

int run(void) {
    sel4utils_run_on_stack(&vspace, main_continued);
    assert(!"Should not get here");

    return 0;
}
