#pragma once
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

// Leitura/escrita da memória viva de outro processo via process_vm_readv/writev.
// Não para o alvo (bom para ler e para "congelar valor" em loop). Para alvos do
// mesmo uid com ptrace_scope=0 funciona direto; cross-uid vai pelo helper.
namespace mem {

// Retorna bytes transferidos (>=0) ou -1 com errno setado.
ssize_t readv(int pid, uint64_t addr, void *buf, size_t len);
ssize_t writev(int pid, uint64_t addr, const void *buf, size_t len);

} // namespace mem
