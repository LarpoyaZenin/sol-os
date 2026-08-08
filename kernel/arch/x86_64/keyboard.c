#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include <stdint.h>
#include <stddef.h>

#define PS2_DATA_PORT 0x60

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Scancode set 1, unshifted, make codes only (key-down). Release
 * (break) codes are the make code with the high bit set (+0x80) and
 * are currently just dropped — no held-key tracking yet, so no
 * shift/ctrl/alt modifiers. That's the natural next step once this
 * is wired into something that needs them (a shell, Phase 4). Index
 * 0 is unused (scancode 0 is not a valid make code). Entries left
 * as 0 are unmapped keys (function keys, etc) for this first pass. */
static const char scancode_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ', /* rest defaults to 0 via static zero-init */
};

#define KBD_BUF_SIZE 128
static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static void kbd_buf_push(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) return;   /* buffer full — drop the keystroke */
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

bool keyboard_read_char(char *out) {
    if (kbd_tail == kbd_head) return false;   /* empty */
    *out = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return true;
}

static void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (!(scancode & 0x80)) {         /* make code (key-down), not a release */
        char c = scancode_ascii[scancode & 0x7F];
        if (c != 0) {
            kbd_buf_push(c);
        }
    }

    pic_send_eoi(1);
}

void keyboard_init(void) {
    idt_register_handler(33, keyboard_handler);  /* IRQ1 -> vector 33 */
    pic_unmask_irq(1);
}
