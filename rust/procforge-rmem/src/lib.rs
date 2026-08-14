//! Núcleo perigoso de acesso à memória de OUTRO processo, em Rust.
//! Exposto via FFI ("C") e linkado no helper privilegiado `procforged`.
//! `no_std` + `panic = abort`: staticlib mínima, sem runtime Rust, linka direto
//! no binário C++. As chamadas `process_vm_*` vêm da libc já linkada pelo C++.
#![no_std]

use core::ffi::{c_int, c_ulong, c_void};

#[panic_handler]
fn on_panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

#[repr(C)]
struct IoVec {
    base: *mut c_void,
    len: usize,
}

extern "C" {
    fn process_vm_readv(pid: c_int, local: *const IoVec, liovcnt: c_ulong,
                        remote: *const IoVec, riovcnt: c_ulong, flags: c_ulong) -> isize;
    fn process_vm_writev(pid: c_int, local: *const IoVec, liovcnt: c_ulong,
                         remote: *const IoVec, riovcnt: c_ulong, flags: c_ulong) -> isize;
}

/// Lê `len` bytes a partir de `addr` do processo `pid` para `buf`.
/// Retorna o número de bytes lidos, ou -1 em erro/argumentos inválidos.
#[no_mangle]
pub extern "C" fn pf_read_mem(pid: c_int, addr: u64, buf: *mut u8, len: usize) -> isize {
    if buf.is_null() || len == 0 {
        return -1;
    }
    let local = IoVec { base: buf as *mut c_void, len };
    let remote = IoVec { base: addr as *mut c_void, len };
    // SAFETY: iovecs válidos por `buf`/`len`; process_vm_readv não escreve além de `len`.
    unsafe { process_vm_readv(pid, &local, 1, &remote, 1, 0) }
}

/// Escreve `len` bytes de `buf` em `addr` do processo `pid`.
/// Retorna o número de bytes escritos, ou -1 em erro/argumentos inválidos.
#[no_mangle]
pub extern "C" fn pf_write_mem(pid: c_int, addr: u64, buf: *const u8, len: usize) -> isize {
    if buf.is_null() || len == 0 {
        return -1;
    }
    let local = IoVec { base: buf as *mut c_void, len };
    let remote = IoVec { base: addr as *mut c_void, len };
    // SAFETY: origem `buf`/`len` válida; escreve na memória do alvo via syscall.
    unsafe { process_vm_writev(pid, &local, 1, &remote, 1, 0) }
}
