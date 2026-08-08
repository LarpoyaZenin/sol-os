#ifndef SOL_KEYBOARD_H
#define SOL_KEYBOARD_H

#include <stdbool.h>

/* Registers the IRQ1 handler and unmasks it. Call after idt_init()
 * and pic_remap(). Uses scancode set 1 (the PS/2 controller's power-
 * on default), decoding a basic US QWERTY layout — no scancode set
 * switching, no extended (0xE0-prefixed) keys like arrows yet. */
void keyboard_init(void);

/* Pops the oldest buffered ASCII character, or returns false if
 * the buffer is empty. Non-blocking by design so kmain's idle loop
 * (or later, a shell) can poll it without stalling. */
bool keyboard_read_char(char *out);

#endif /* SOL_KEYBOARD_H */
