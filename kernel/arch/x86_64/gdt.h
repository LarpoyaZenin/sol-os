#ifndef SOL_GDT_H
#define SOL_GDT_H

/* Sets up and loads Sol OS's own GDT (null, kernel code, kernel
 * data). Must run before idt_init(), since interrupt handlers rely
 * on a known-good code segment selector — Limine's GDT is only
 * guaranteed valid up until the kernel takes over. No TSS yet; that
 * arrives with ring 3 / user mode in Phase 4. */
void gdt_init(void);

#endif /* SOL_GDT_H */
