# ProcForge — estado

Fases 0-4 completas. Clone funcional do Process Hacker, muito além do básico.

## Feito nesta rodada
- **Inspetor de propriedades** (`panels/ProcessProperties`, duplo-clique ou menu):
  abas Geral, Estatísticas (ao vivo), Threads (com suspend/resume), Token
  (uid/gid/**capabilities** decodificadas via capsh, seccomp, NoNewPrivs, contexto
  SELinux), Ambiente, Handles (fds, com fechar-no-dono), Memória (maps).
- **Async no eBPF e namespaces** (`bpfTraceAsync`/`nsRunAsync`) — não travam mais.
- **ss -K também para UDP** na aba Rede.
- **Núcleo de memória em Rust** (`rust/procforge-rmem`, staticlib no_std): read/write
  de memória de outro processo (process_vm_*) portado para Rust, roteado no helper
  via FFI. **Código completo, ativa sozinho** quando o `cargo` existir (o CMake
  detecta; sem cargo usa o C++). Aqui o `dnf install rust cargo` deu timeout no
  sandbox — o usuário instala e recompila para ativar.

## Dívida técnica restante
- [ ] **Restore de cgroup à origem**: cai na raiz (`0::/`) quando o systemd já
      coletou o scope de origem. É inerente à semântica de cgroup/systemd (só o
      scope-folha original — que some — poderia recebê-lo de volta; slices não
      seguram processos por causa da regra "no internal process"). Solução ideal:
      criar o leaf procforge DENTRO do scope de origem (nested) e devolver ao
      pai; depende de delegação de controladores. Documentado.
- [ ] R/W de memória cross-uid no scanner ainda é bloqueante (raro; same-uid é
      direto). Tornar async como no eBPF/namespaces.
- [ ] Ativar o núcleo Rust de fato (instalar toolchain) e estender a Rust o
      injetor (dlopen) e o fd-closer (as partes mais perigosas com lógica).
- [ ] Revisar a superfície do sandbox do helper.
