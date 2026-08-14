# procforged — helper privilegiado

> Fase 1 IMPLEMENTADA: `Ping`, `SendSignal`, `Renice`, `SetAffinity` já rodam
> (ver `Helper.cpp`/`Polkit.cpp`). Os demais métodos abaixo são o esboço das
> próximas fases.

Serviço D-Bus de sistema, ativado sob demanda, `org.procforge.Helper1` em
`/org/procforge/Helper1`. Roda como root e **reduz imediatamente para caps
mínimas** (CAP_SYS_PTRACE, CAP_KILL, CAP_SYS_NICE, CAP_SYS_RESOURCE,
CAP_DAC_OVERRIDE conforme a ação) + seccomp allowlist. Idle-exit.

## Regras invioláveis
- Cada método verifica autorização polkit da AÇÃO correspondente
  (`data/org.procforge.helper.policy`) usando o `system-bus-name` do chamador.
- Valida identidade do alvo por `(pid, starttime)` ou pidfd (anti-reuso de PID).
- Recusa PID 1, kernel threads e a si mesmo.
- Audita no journald: uid do chamador, ação, pid+starttime do alvo, resultado.

## Interface (esboço)
```
SendSignal(u pid, t starttime, i signo)
Freeze(u pid, b on)                          # cgroup.freeze; fallback SIGSTOP
SuspendThread(u pid, u tid, b on)            # ptrace por-TID
Renice/SetSched/SetAffinity/SetRlimit(...)
ReadFds(u pid) -> a(...)   DupFd/CloseForeignFd(u pid, i fd)
ReadMem(u pid, t addr, u len) -> ay
WriteMem(u pid, t addr, ay bytes)
InjectLibrary(u pid, s path)
CgroupMove/CgroupLimit(...)   NsEnterSpawn(...)
```

## Recomendação de linguagem
O núcleo perigoso (ReadMem/WriteMem/Inject/ptrace) é candidato natural a **Rust**
(`zbus` + `nix` + `libc`), **sem nenhum binding de Qt** — o helper não tem GUI.
É onde a segurança de memória paga. O resto do helper pode ficar em C++ ou Rust.
