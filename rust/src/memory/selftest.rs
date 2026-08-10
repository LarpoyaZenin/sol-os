//! Memory self-tests, ported from `pmm_selftest()` and
//! `heap_selftest()` in `kernel/kmain.c` (C). Runs after
//! `pmm::init` and `kheap::init`.

use crate::memory::kheap;
use crate::memory::pmm;

pub fn run() {
    pmm_selftest();
    heap_selftest();
}

fn pmm_selftest() {
    let p1 = pmm::alloc_page();
    let p2 = pmm::alloc_page();
    let p3 = pmm::alloc_page();
    crate::kprintln!("[rust] PMM selftest: pages {:#x} {:#x} {:#x}", p1, p2, p3);

    pmm::free_page(p2);
    let p4 = pmm::alloc_page();
    crate::kprintln!(
        "[rust] PMM selftest: freed p2, realloc got {:#x} ({})",
        p4,
        if p4 == p2 {
            "OK, reuse"
        } else {
            "not reused"
        }
    );
    pmm::free_page(p1);
    pmm::free_page(p3);
    pmm::free_page(p4);
}

fn heap_selftest() {
    let a = kheap::kmalloc(64);
    let b = kheap::kmalloc(512);
    crate::kprintln!("[rust] Heap selftest: a={:p} b={:p}", a, b);

    if !a.is_null() {
        unsafe {
            core::ptr::write_bytes(a, 0xAB, 64);
        }
    }
    if !b.is_null() {
        unsafe {
            core::ptr::write_bytes(b, 0xCD, 512);
        }
    }

    kheap::kfree(a);
    let c = kheap::kmalloc(64); /* should reuse a's block */
    crate::kprintln!(
        "[rust] Heap selftest: realloc after free -> {:p} ({})",
        c,
        if c == a { "OK, reused" } else { "different block" }
    );

    kheap::kfree(b);
    kheap::kfree(c);

    crate::kprintln!(
        "[rust] Heap stats: used={} free={}",
        kheap::used_bytes(),
        kheap::free_bytes()
    );
}
