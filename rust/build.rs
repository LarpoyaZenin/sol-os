fn main() {
    // Link with our higher-half linker script (absolute path, so the
    // linker invocation succeeds regardless of cargo's working dir).
    let ld = concat!(env!("CARGO_MANIFEST_DIR"), "/linker.ld");
    println!("cargo:rustc-link-arg=-T{ld}");
    // Keep the kernel a static ET_EXEC at fixed higher-half addresses
    // (the builtin bare-metal triple defaults to PIE otherwise).
    println!("cargo:rustc-link-arg=-no-pie");
    println!("cargo:rerun-if-changed=linker.ld");
}
