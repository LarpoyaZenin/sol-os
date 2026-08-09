#include "virtio_input.h"
#include "virtio.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "klog.h"
#include "mm/pmm.h"
#include <stddef.h>

/* ---- virtio-input protocol constants ---- */

#define VIRTIO_ID_INPUT        0x12u
#define VIRTIO_PCI_MODERN_ID   (0x1040u | VIRTIO_ID_INPUT)   /* 0x1052 */
#define VIRTIO_PCI_LEGACY_ID   (0x1000u | VIRTIO_ID_INPUT)   /* 0x1012 */

#define VIRTIO_INPUT_F_EVENTS  0u

#define VIRTIO_INPUT_MAX_DEVICES 4u
#define VIRTIO_INPUT_MAX_QUEUE   256u   /* events array is 256 * 8 = 2 KiB/page */
#define VIRTIO_INPUT_MAX_NAME    48u

/* Event types (Linux input event codes). */
#define EV_SYN  0x00u
#define EV_KEY  0x01u
#define EV_REL  0x02u
#define EV_ABS  0x03u
#define EV_MSC  0x04u

#define SYN_REPORT 0u

#define REL_X      0u
#define REL_Y      1u
#define REL_HWHEEL 6u
#define REL_WHEEL  8u

#define ABS_X 0u
#define ABS_Y 1u

/* Mouse buttons (Linux BTN_*). These are what lets the mouse expose
 * its side/extra buttons through the same EV_KEY channel the keyboard
 * uses for its keys. */
#define BTN_LEFT    0x110u
#define BTN_RIGHT   0x111u
#define BTN_MIDDLE  0x112u
#define BTN_SIDE    0x113u
#define BTN_EXTRA   0x114u
#define BTN_FORWARD 0x115u
#define BTN_BACK    0x116u

/* Fixed 8-byte event packet (virtio spec section 5.12.5.3). */
struct virtio_input_event {
    uint16_t type;    /* EV_* */
    uint16_t code;    /* KEY_* / REL_* / ABS_* / SYN_* */
    uint32_t value;   /* key state (0/1), relative delta, or absolute */
} __attribute__((packed));

typedef struct virtio_input {
    int valid;
    uint8_t index;

    struct virtio_device vdev;
    char name[VIRTIO_INPUT_MAX_NAME];

    /* Event buffers: page(s) of physical memory the device DMA-writes
     * into. Descriptor i permanently points at events[i]. */
    struct virtio_input_event *events;
    uintptr_t events_phys;

    int use_irq;
    uint8_t irq;
    irq_node_t irq_node;

    /* Classification (only ever used for nicer logs). */
    int saw_keys;
    int saw_buttons;

    /* Pointer-device accumulator, drained by virtio_mouse_get_delta().
     * Written in IRQ context (cli active); read with interrupts
     * disabled in the getter. */
    volatile int32_t mouse_dx;
    volatile int32_t mouse_dy;
    volatile uint8_t mouse_buttons;

    volatile uint64_t events_total;
    volatile uint64_t events_key;
    volatile uint64_t events_rel;
    volatile uint64_t events_abs;
    volatile uint64_t events_msc;
    volatile uint64_t events_syn;
    volatile uint64_t events_dropped;
    volatile uint64_t irq_count;
} virtio_input_t;

static virtio_input_t g_inputs[VIRTIO_INPUT_MAX_DEVICES];

/* ---- key/button name helpers ---- */

static char keycode_to_char(uint16_t code) {
    static const char map[128] = {
        [1]  = 27,                                   /* ESC */
        [2]  = '1', [3] = '2', [4] = '3', [5] = '4', [6] = '5',
        [7]  = '6', [8] = '7', [9] = '8', [10] = '9', [11] = '0',
        [12] = '-', [13] = '=', [14] = 8, [15] = 9,   /* backspace, tab */
        [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
        [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
        [26] = '[', [27] = ']', [28] = 10,            /* enter */
        [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
        [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
        [39] = ';', [40] = 39, [41] = '`',            /* ; ' ` */
        [43] = 92,                                    /* backslash */
        [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
        [49] = 'n', [50] = 'm', [51] = ',', [52] = '.', [53] = '/',
        [57] = ' ',
        /* Navigation keys map to sentinels the terminal understands
         * (see virtio_keyboard_read_char in the header): */
        [103] = 0x05,                                 /* up */
        [104] = 0x01,                                 /* page up */
        [105] = 0x03,                                 /* left */
        [106] = 0x04,                                 /* right */
        [108] = 0x06,                                 /* down */
        [109] = 0x02,                                 /* page down */
    };
    if (code < 128) return map[code];
    return 0;
}

/* ---- keyboard character queue ---- */

#define VKEY_BUF_SIZE 256u

static volatile char vkey_buf[VKEY_BUF_SIZE];
static volatile uint32_t vkey_head;
static volatile uint32_t vkey_tail;

static void vkey_push(char c) {
    uint32_t next = (vkey_head + 1) % VKEY_BUF_SIZE;
    if (next == vkey_tail) return;   /* full — drop the keystroke */
    vkey_buf[vkey_head] = c;
    vkey_head = next;
}

bool virtio_keyboard_read_char(char *out) {
    __asm__ volatile ("cli");
    uint32_t t = vkey_tail;
    bool got = t != vkey_head;
    if (got) {
        *out = vkey_buf[t];
        vkey_tail = (t + 1) % VKEY_BUF_SIZE;
    }
    __asm__ volatile ("sti");
    return got;
}

static const char *button_name(uint16_t code) {
    switch (code) {
        case BTN_LEFT:    return "left";
        case BTN_RIGHT:   return "right";
        case BTN_MIDDLE:  return "middle";
        case BTN_SIDE:    return "side";
        case BTN_EXTRA:   return "extra";
        case BTN_FORWARD: return "forward";
        case BTN_BACK:    return "back";
        default:          return "?";
    }
}

/* Bit index in the mouse_buttons bitmask for a BTN_* code. -1 if the
 * code has no dedicated bit (then EV_KEY is logged but not tracked). */
static int button_bit(uint16_t code) {
    switch (code) {
        case BTN_LEFT:    return 0;
        case BTN_RIGHT:   return 1;
        case BTN_MIDDLE:  return 2;
        case BTN_SIDE:    return 3;
        case BTN_EXTRA:   return 4;
        case BTN_FORWARD: return 5;
        case BTN_BACK:    return 6;
        default:          return -1;
    }
}

static const char *rel_name(uint16_t code) {
    switch (code) {
        case REL_X:      return "x";
        case REL_Y:      return "y";
        case REL_WHEEL:  return "wheel";
        case REL_HWHEEL: return "hwheel";
        default:         return "?";
    }
}

static const char *abs_name(uint16_t code) {
    switch (code) {
        case ABS_X: return "x";
        case ABS_Y: return "y";
        default:    return "?";
    }
}

/* ---- event handling ---- */

static void virtio_input_handle_event(virtio_input_t *vi,
                                      const struct virtio_input_event *ev, uint32_t len) {
    if (len < sizeof(struct virtio_input_event)) {
        vi->events_dropped++;
        return;
    }

    switch (ev->type) {
    case EV_SYN:
        vi->events_syn++;
        break;

    case EV_KEY:
        vi->events_key++;
        if (ev->code >= 0x100u) {
            /* Mouse button (BTN_*), including side/extra. */
            vi->saw_buttons = 1;
            int bit = button_bit(ev->code);
            if (bit >= 0) {
                if (ev->value != 0) {
                    vi->mouse_buttons |= (uint8_t)(1u << bit);
                } else {
                    vi->mouse_buttons &= (uint8_t)~(1u << bit);
                }
            }
            if (ev->value != 0 || ev->code <= BTN_BACK) {
                klog("[vinput%d] button %s down=%d\n", vi->index, button_name(ev->code), (int)ev->value);
            }
        } else {
            char c = keycode_to_char(ev->code);
            if (ev->value != 0) {
                vi->saw_keys = 1;
                if (c >= 0x20 && c <= 0x7E) {
                    klog("[vinput%d] key code=%u '%c'\n", vi->index, (unsigned)ev->code, c);
                } else {
                    klog("[vinput%d] key code=%u down\n", vi->index, (unsigned)ev->code);
                }
                if (c != 0 && ev->value == 1) {
                    vkey_push(c);
                }
            }
        }
        break;

    case EV_REL:
        vi->events_rel++;
        if (ev->code == REL_X) {
            vi->mouse_dx += (int32_t)ev->value;
        } else if (ev->code == REL_Y) {
            vi->mouse_dy += (int32_t)ev->value;
        }
        klog("[vinput%d] rel %s=%d\n", vi->index, rel_name(ev->code), (int)(int32_t)ev->value);
        break;

    case EV_ABS:
        vi->events_abs++;
        klog("[vinput%d] abs %s=%u\n", vi->index, abs_name(ev->code), (unsigned)ev->value);
        break;

    case EV_MSC:
        vi->events_msc++;
        break;

    default:
        vi->events_dropped++;
        klog("[vinput%d] unknown event type=%u code=%u\n",
             vi->index, (unsigned)ev->type, (unsigned)ev->code);
        break;
    }
    vi->events_total++;
}

/* Drains every used descriptor and recycles it. Must be called with
 * interrupts disabled (IRQ context, or a cli/sti section). */
static void virtio_input_drain(virtio_input_t *vi) {
    struct virtio_device *d = &vi->vdev;
    uint16_t id;
    uint32_t len;
    int recycled = 0;

    while (virtio_queue_pop_used(d, 0, &id, &len)) {
        if (id < VIRTIO_INPUT_MAX_QUEUE) {
            virtio_input_handle_event(vi, &vi->events[id], len);
        } else {
            vi->events_dropped++;
        }
        virtio_queue_recycle(d, 0, id);
        recycled++;
    }
    if (recycled > 0) virtio_queue_kick(d, 0);
}

static void virtio_input_irq(void *ctx) {
    virtio_input_t *vi = ctx;
    uint8_t isr = *(volatile uint8_t *)vi->vdev.isr;
    if (isr == 0) return;          /* spurious / other device on this line */
    vi->irq_count++;
    if (isr & 0x01u) virtio_input_drain(vi);   /* used-buffer notification */
    /* bit 0x02 = config change: nothing to handle here */
}

/* ---- init ---- */

static void virtio_input_read_name(virtio_input_t *vi) {
    if (vi->vdev.device_cfg == NULL) return;
    /* select = VIRTIO_INPUT_CFG_ID_NAME (1), subsel = 0 */
    virtio_device_cfg_write8(&vi->vdev, 0, 1);
    virtio_device_cfg_write8(&vi->vdev, 1, 0);
    uint8_t size = virtio_device_cfg_read8(&vi->vdev, 2);
    if (size > VIRTIO_INPUT_MAX_NAME - 1) size = VIRTIO_INPUT_MAX_NAME - 1;
    for (uint8_t i = 0; i < size; i++) {
        vi->name[i] = (char)virtio_device_cfg_read8(&vi->vdev, (uint32_t)(8 + i));
    }
    vi->name[size] = 0;
}

uint32_t virtio_input_init(uint64_t hhdm) {
    uint32_t found = 0;

    for (uint32_t i = 0; i < pci_device_count() && found < VIRTIO_INPUT_MAX_DEVICES; i++) {
        const pci_device_t *p = pci_device_at(i);
        if (p == NULL) continue;
        if (p->vendor != 0x1AF4u) continue;
        if (p->device_id != VIRTIO_PCI_MODERN_ID && p->device_id != VIRTIO_PCI_LEGACY_ID) continue;

        virtio_input_t *vi = &g_inputs[found];
        vi->index = (uint8_t)found;

        klog("virtio-input: found %x:%x.%u (id %x)\n", p->bus, p->dev, p->func, p->device_id);

        if (p->device_id != VIRTIO_PCI_MODERN_ID) {
            klog("virtio-input: legacy-transitional device %x unsupported (modern-only)\n",
                 p->device_id);
            continue;
        }

        if (virtio_device_init(&vi->vdev, p, hhdm) != 0) {
            klog("virtio-input: transport init failed\n");
            continue;
        }

        virtio_device_set_feature(&vi->vdev, VIRTIO_INPUT_F_EVENTS);
        if (virtio_device_finish_features(&vi->vdev) != 0) {
            klog("virtio-input: feature negotiation failed\n");
            continue;
        }
        klog("virtio-input: features ok (dev %x:%x)\n",
             vi->vdev.device_features_lo, vi->vdev.device_features_hi);

        uint16_t size = virtio_queue_size(&vi->vdev, 0);
        if (size > VIRTIO_INPUT_MAX_QUEUE) size = VIRTIO_INPUT_MAX_QUEUE;
        if (virtio_queue_init(&vi->vdev, 0, size) != 0) {
            klog("virtio-input: queue setup failed\n");
            continue;
        }

        /* One page of event buffers (256 * 8 bytes = 2 KiB). */
        vi->events_phys = pmm_alloc_page();
        if (vi->events_phys == 0) {
            klog("virtio-input: no memory for event buffers\n");
            continue;
        }
        vi->events = (struct virtio_input_event *)(hhdm + vi->events_phys);

        for (uint16_t n = 0; n < size; n++) {
            if (virtio_queue_add_buffer(&vi->vdev, 0,
                                        vi->events_phys + (uintptr_t)n * sizeof(struct virtio_input_event),
                                        sizeof(struct virtio_input_event), 1) != 0) {
                break;   /* pool exhausted — not expected at setup */
            }
        }
        virtio_queue_kick(&vi->vdev, 0);

        virtio_input_read_name(vi);

        /* Hook the IRQ line the firmware programmed for this function. */
        uint8_t irq = pci_config_read8(p->bus, p->dev, p->func, 0x3Cu);
        if (irq != 0 && irq < 16) {
            if (idt_register_irq_handler(irq, virtio_input_irq, vi, &vi->irq_node)) {
                pic_unmask_irq(irq);
                vi->use_irq = 1;
                vi->irq = irq;
            }
        }
        if (vi->use_irq) {
            klog("virtio-input[%u]: '%s' irq=%u queue=%u\n",
                 found, vi->name[0] ? vi->name : "<unnamed>", irq, size);
        } else {
            klog("virtio-input[%u]: '%s' no IRQ (polling)\n",
                 found, vi->name[0] ? vi->name : "<unnamed>");
        }

        virtio_device_set_driver_ok(&vi->vdev);
        vi->valid = 1;
        found++;
    }

    klog("virtio-input: %u device(s) ready\n", found);
    return found;
}

void virtio_input_poll(void) {
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        virtio_input_t *vi = &g_inputs[i];
        if (!vi->valid) continue;
        __asm__ volatile ("cli");
        virtio_input_drain(vi);
        __asm__ volatile ("sti");
    }
}

bool virtio_mouse_get_delta(int32_t *dx, int32_t *dy, uint8_t *buttons) {
    __asm__ volatile ("cli");
    int32_t ddx = 0, ddy = 0;
    uint8_t btns = 0;
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        virtio_input_t *vi = &g_inputs[i];
        if (!vi->valid) continue;
        ddx += vi->mouse_dx;
        ddy += vi->mouse_dy;
        btns |= vi->mouse_buttons;
        vi->mouse_dx = 0;
        vi->mouse_dy = 0;
    }
    __asm__ volatile ("sti");

    *dx = ddx;
    *dy = ddy;
    *buttons = btns;
    return ddx != 0 || ddy != 0;
}

uint32_t virtio_input_device_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        if (g_inputs[i].valid) n++;
    }
    return n;
}

uint64_t virtio_input_irq_count(void) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        if (g_inputs[i].valid) total += g_inputs[i].irq_count;
    }
    return total;
}

uint64_t virtio_input_event_count(void) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        if (g_inputs[i].valid) total += g_inputs[i].events_total;
    }
    return total;
}

uint64_t virtio_input_key_count(void) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        if (g_inputs[i].valid) total += g_inputs[i].events_key;
    }
    return total;
}

uint64_t virtio_input_rel_count(void) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        if (g_inputs[i].valid) total += g_inputs[i].events_rel;
    }
    return total;
}

uint64_t virtio_input_dropped_count(void) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < VIRTIO_INPUT_MAX_DEVICES; i++) {
        if (g_inputs[i].valid) total += g_inputs[i].events_dropped;
    }
    return total;
}
