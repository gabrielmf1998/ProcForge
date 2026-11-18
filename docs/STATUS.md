# ProcForge — estado

Fases 0-4 completas. Clone funcional do Process Hacker, muito além do básico.

## Feito nesta rodada (0.4.0) — correção de bugs
- **Verificador de atualização dava falso positivo permanente.** A versão do
  app era `"0.3"` e as tags são `v0.3.0`; `QVersionNumber(0,3,0) > (0,3)` é
  verdadeiro, então o app SEMPRE notificava "atualização disponível 0.3.0"
  mesmo estando atualizado. Agora `applicationVersion` tem três segmentos
  (`0.4.0`), igual ao formato das tags. (CMake/spec/PKGBUILD/deb bumpados.)
- **Assinatura de eventos em tempo real duplicava ao religar.** `Ver ›
  Opções` liga/desliga "Atualização em tempo real"; o `subscribeProcEvents`
  conectava o sinal D-Bus `ProcEvent` de novo sem desconectar o anterior, e
  cada evento `cn_proc` passava a disparar DOIS rescans. Agora guarda
  receptor/slot e `bus.disconnect()` antes de reconectar (e no unsubscribe).
- **"Matar árvore inteira" não funcionava em processo de root/outro usuário.**
  `actions::killTree` usava `kill()` direto; sem privilégio ele falha em
  silêncio e a função ainda retornava "ok (0 encerrados)", sem cair no helper.
  Novo método privilegiado `KillTree(pid, starttime)` no helper faz
  STOP+coleta+KILL da árvore toda como root, com UM diálogo do polkit (reusa a
  action `org.procforge.signal.other`, sem nova policy). A GUI usa a via direta
  para alvos do próprio uid e o helper para os demais (`killTreeAsync`).
- **Limpeza:** trocado `invalidateFilter()` (deprecado no Qt 6.12) por
  `invalidateRowsFilter()` no filtro de busca; `Ping` reporta `0.4.0`.

## Rodada anterior (0.3.0)
- **Cortar/restabelecer rede de um processo** (menu de contexto): novo método
  `NetBlock` no helper — DROP via `iptables`/`ip6tables` casando pelo cgroup v2
  (`-m cgroup --path`), nas chains OUTPUT e INPUT. Reaproveita a MESMA
  infraestrutura de leaf do "Limitar recursos" (`ensureLeaf`/`releaseLeaf`
  extraídos de `CgroupThrottle`/`CgroupRelease`): cgroup v2 só permite um
  cgroup por processo, então throttle e bloqueio de rede precisam do mesmo
  leaf quando os dois estão ativos — `CgroupThrottle` agora reaproveita um
  leaf já existente em vez de tentar mover o processo de novo, e
  `CgroupRelease` não derruba o leaf se o bloqueio de rede ainda precisar
  dele (só zera os limites de recurso). Testado ao vivo (não só em
  container): `iptables -m cgroup --path` confirmado funcionando no kernel
  6.17/Fedora 43 antes de entrar no helper. Action polkit `net.block`
  (`auth_admin_keep`, mesmo nível de `cgroup.move`).
- **Corrigido — "Intervalo de atualização" sem stop e não respeitava os
  segundos**: eventos de processo em tempo real (`cn_proc`) disparavam um
  rescan a cada fork/exec/exit do SISTEMA INTEIRO com um debounce fixo de
  60ms, incondicional ao valor escolhido no menu — num desktop real e ativo
  isso significa rescans muito mais frequentes que "5 s", resetando a
  seleção da árvore e impedindo cliques. Agora o intervalo configurado é o
  piso: eventos em tempo real só disparam rescan imediato se já fazia
  tempo ocioso, senão esperam o que falta do intervalo. Adicionada opção
  **"Parar (manual)"** (0 ms) no menu Ver e nas Opções — desliga tanto o
  polling quanto os rescans por evento; só atualiza no F5/Atualizar manual.

## Rodada anterior (0.2.0)
- **Ícone na bandeja** (`core/TrayIcon`): reabrir a janela, checar atualização
  e alternar autostart. Fechar a janela (X) minimiza para a bandeja em vez de
  encerrar; "Sair" (menu Hacker ou tray) é que realmente fecha o app —
  `QApplication::setQuitOnLastWindowClosed(false)` evita que uma janela
  secundária (Propriedades, Scanner) fechando mate o app com a bandeja ainda
  ativa. Flag `--tray` (usado pela entrada de autostart) abre só na bandeja.
- **Verificar atualizações** (`core/UpdateChecker`): consulta assíncrona
  (Qt Network) à última release no GitHub e no GitLab em paralelo, compara com
  `QApplication::applicationVersion()` via `QVersionNumber`. Checagem
  silenciosa ao iniciar (só notifica se houver versão nova) + item de menu
  manual (sempre mostra o resultado). Clicar na notificação abre a release no
  navegador — **não baixa/instala nada sozinho** (decisão deliberada: o
  helper privilegiado e a unit systemd não devem ser trocados sem o usuário
  rodar o instalador conscientemente).
- **Iniciar com o sistema** (`core/Autostart`): entrada XDG em
  `~/.config/autostart`, fonte da verdade é o próprio arquivo (sem duplicar
  estado em QSettings).
- **Correção de segurança**: `Renice`/`SetAffinity`/`ReadMem`/`WriteMem` no
  helper eram os 4 únicos métodos privilegiados que não validavam
  `(pid, starttime)` contra reuso de PID (todo o resto do helper trata isso
  como invariante). `Renice`/`SetAffinity` também tinham descasamento de
  assinatura D-Bus com o cliente assíncrono (`reniceAsync`/`setAffinityAsync`
  já mandavam `starttime`, o helper não aceitava) — elevar prioridade ou
  mudar afinidade de processo de outro usuário sempre falhava. Corrigido nos
  4 métodos; removidos os wrappers síncronos mortos `helper::renice`/
  `helper::setAffinity` (não tinham chamador — só as versões `*Async` são
  usadas na UI).
- Versão bumpada para 0.2.0 (CMakeLists, `applicationVersion`, `.spec`) —
  necessário para o UpdateChecker comparar corretamente contra a próxima tag.

## Rodada anterior (0.1.1)
- **"Executar como…" (Run as)** — o ProcForge agora CRIA processos, não só
  manipula os existentes. Método privilegiado `LaunchProcess` (helper): novo
  processo como outro usuário (uid/gid + initgroups), com cwd, nice e afinidade,
  duplo-fork para desanexar. Diálogo no menu Hacker. Action polkit
  `process.launch`.
- **Manipulação de páginas de memória** (`mmap`/`mprotect`/`munmap`) — o que o
  PH faz em "Protect…"/"Free". Executado DENTRO do alvo via chamada remota de
  função libc por ptrace (`inject/RemoteMem`); métodos `AllocMem`/`ProtectMem`/
  `FreeMem` no helper, action `mem.map`. UI no scanner (aba Memória): alocar
  região, mudar proteção R/W/X, liberar.
- **Ícones por processo** na lista (`core/ProcessIcons`): ícone real do app via
  índice dos `.desktop` (Exec→Icon), depois tema, e fallback de sistema
  (engrenagem p/ kernel/serviço, terminal p/ shells, executável genérico).
- **Tema Clássico estilo Windows 7 Aero** (sem transparência): folha de estilo
  com gradientes, bordas beveladas, seleção azul degradê, abas arredondadas —
  preservando as cores por categoria dos itens.

## Rodada anterior
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
