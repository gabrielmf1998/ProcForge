<div align="center">

# ⚒️ ProcForge

**Um clone do [Process Hacker / System Informer](https://systeminformer.sourceforge.io/) para Linux — nativo, respeitando as primitivas do sistema.**

Não é um visualizador. É uma ferramenta de **manipulação e controle ativos** do sistema em tempo real: matar, suspender, congelar, reescrever memória, injetar bibliotecas, estrangular CPU/RAM/disco, fechar descritores alheios, rastrear com eBPF e mais — tudo sobre APIs estáveis do Linux (`procfs`, `pidfd`, `ptrace`, `process_vm_*`, `cgroups v2`, `netlink`, `eBPF`), com GUI Qt6 / KDE Frameworks 6 em **Wayland puro**.

![Tema Clássico (Process Hacker)](docs/img/procforge-classic.png)

</div>

> ⚠️ **Aviso.** O ProcForge dá a você poder real sobre processos em execução: escrever na memória de outro programa, injetar código, fechar travas, limitar recursos e matar árvores inteiras. Use **na sua própria máquina** e com responsabilidade. Toda ação privilegiada (uid alheio / root) passa por um diálogo do **polkit**; a GUI **nunca** roda como root.

---

## Índice

- [Por que existe](#por-que-existe)
- [Capturas de tela](#capturas-de-tela)
- [Funcionalidades](#funcionalidades)
- [Instalação](#instalação)
  - [Instalação num comando (curl)](#instalação-num-comando-curl)
  - [Pacote RPM (Fedora / Nobara / Bazzite)](#pacote-rpm-fedora--nobara--bazzite)
  - [Pacote .deb (Debian / Ubuntu)](#pacote-deb-debian--ubuntu)
  - [Pacote Arch Linux (PKGBUILD)](#pacote-arch-linux-pkgbuild)
  - [AppImage (qualquer distro com KDE Plasma)](#appimage-qualquer-distro-com-kde-plasma)
  - [A partir do código-fonte](#a-partir-do-código-fonte)
  - [Atualizações](#atualizações)
- [Requisitos](#requisitos)
- [Arquitetura e segurança](#arquitetura-e-segurança)
- [Como usar](#como-usar)
- [Idiomas e temas](#idiomas-e-temas)
- [Estado do projeto](#estado-do-projeto)
- [Licença e créditos](#licença-e-créditos)

---

## Por que existe

No Windows, o Process Hacker precisa de um **driver em kernel mode** porque o SO nega ao usuário esse nível de manipulação. No Linux, esse poder **já é do usuário no espaço de usuário** — está só espalhado e cru entre `kill`, `gdb`, `scanmem`, `renice`, `ss`, `nsenter`, `bpftrace`. O ProcForge não conquista acesso: ele dá **uma interface gráfica coerente e ação direta** a esse poder, e em vários pontos **vai além** do que o Process Hacker jamais conseguiu no Windows (cgroups como bisturi de recursos, eBPF por processo, namespaces/containers, janelas via KWin scripting).

## Capturas de tela

| Tema Clássico (cara do Process Hacker) | Tema escuro (Breeze) |
|---|---|
| ![clássico](docs/img/procforge-classic.png) | ![escuro](docs/img/procforge-dark.png) |

| Inspetor de propriedades (7 abas) | Aba de Serviços (systemd) |
|---|---|
| ![propriedades](docs/img/procforge-properties.png) | ![serviços](docs/img/procforge-services.png) |

## Funcionalidades

**Lista de processos**
- Árvore ao vivo com atualização **quase instantânea** via eventos `cn_proc` (fork/exec/exit em push, sem polling perceptível).
- Colunas do Process Hacker (Nome, PID, CPU, I/O total, Bytes privados, Usuário, Descrição), com **cores por categoria** (verde=serviço, rosa=root, ciano=seu, cinza=suspenso, amarelo=kernel, flash verde=novo) e **ícone por processo** (ícone real do app via `.desktop`, com fallback de sistema).
- **Colunas opcionais** (PID pai, threads, estado, nice, memória virtual, tempo de CPU, iniciado em, leitura e escrita de disco): clique direito no cabeçalho para escolher; ordem e larguras ficam salvas. Processos que acabaram de sair ficam **em vermelho por 1,5 s**.
- **Busca** por nome, linha de comando, usuário ou PID, com `user:nome` (só um usuário) e `pid:N` (um PID exato). **Salvar lista** (Ctrl+S) em TXT indentado ou CSV.
- Busca/filtro, ordenação, e **inspetor de propriedades** (duplo-clique): abas **Geral, Estatísticas** (ao vivo), **Threads** (suspender por thread), **Token** (uid/gid, *capabilities* decodificadas, seccomp, SELinux), **Ambiente**, **Handles** (fds), **Memória** (maps).

**Manipulação de processos**
- Sinais graduados (TERM/KILL/STOP/CONT/…) via **pidfd** (imune a reuso de PID).
- Suspender/retomar, **matar árvore inteira**, renice, afinidade de CPU.
- Submenu **Prioridade** (Alta, Acima do normal, Normal, Abaixo do normal, Ociosa, Personalizada), **ir para o processo pai**, **abrir local do arquivo** e **copiar** PID/nome/linha de comando/caminho/linha inteira. Confirmação antes de terminar ou matar (processos do sistema sempre perguntam).
- **Escalonamento** (SCHED_OTHER/BATCH/IDLE/FIFO/RR), **ionice**, **prlimit** ao vivo.
- **Suspender/retomar uma thread individual** (ptrace por TID).
- **Executar como…** (Run as): **cria um novo processo** como outro usuário, com diretório de trabalho, *nice* e afinidade — o ProcForge não só manipula o que existe, ele **cria e controla** processos.

**Memória (estilo Cheat Engine / scanmem)**
- Scanner de valor com busca exata, **refinamento** (mudou/não mudou/aumentou/diminuiu/…), **congelamento de valor** (reescrita em loop) e **editor hexadecimal**.
- Leitura/escrita via `process_vm_readv/writev`.
- **Manipulação de páginas** (o "Protect…/Free" do PH): **alocar** (`mmap`), **mudar proteção R/W/X** (`mprotect`) e **liberar** (`munmap`) regiões do alvo — executado *dentro* do processo por chamada remota de função libc via ptrace.

**Injeção e descritores**
- **Injeção de biblioteca** (`dlopen` forçado via ptrace) — a "injeção de DLL" do Linux.
- Aba de **descritores**: fechar um fd de outro processo (injeta `close()`), o truque do *singleton*, e **salvar uma cópia do arquivo** aberto via `pidfd_getfd` (recupera até arquivo apagado que o processo ainda segura).
- **Localizar handles ou DLLs** (Hacker › Localizar handles ou DLLs): procura um texto nos fds e nos mapas de memória de todos os processos, em segundo plano.

**cgroup-bisturi (além do Windows)**
- Limite **ao vivo** de **CPU %, RAM, PIDs e I/O de disco** de um processo, sem reiniciá-lo. Restaura ao scope de origem ao remover.
- **Cortar/restabelecer toda a rede de um processo** (menu de contexto): útil pra diagnosticar se um app se comporta diferente sem internet, sem precisar desconectar a máquina inteira. DROP via `iptables`/`ip6tables` casando pelo cgroup do processo — reaproveita o mesmo cgroup do limite de recursos quando os dois estão ativos.

**eBPF, namespaces e janelas**
- **eBPF por processo** (via bpftrace): contagem de syscalls, arquivos abertos, conexões TCP.
- **Namespaces/containers**: inspeção de `/proc/PID/ns` e **rodar um comando dentro** dos namespaces do alvo (`setns`).
- **Janelas** (Wayland): trazer para frente, minimizar, restaurar, fechar via **KWin scripting**.

**Abas de sistema**
- **Serviços** (systemd) do **sistema ou do usuário**: iniciar/parar/reiniciar/recarregar, **habilitar/desabilitar**, **mascarar/desmascarar**, status/journal, filtro e "Só ativos" (falhos em vermelho).
- **Rede** (`ss`), com filtro e "Só portas abertas": matar o dono da conexão, **fechar a conexão** (`ss -K`), ir para o processo. Seção **"Processos com a rede cortada"**: lista com ícone/nome/PID/caminho de cada processo com a internet cortada agora, com opção de restabelecer — some sozinha quando não há nenhum.
- **Disco**: throughput por dispositivo.
- **Informação do sistema** (Ctrl+I): gráficos ao vivo de CPU (total e por núcleo), RAM e swap, disco e rede.

**Interface**
- Qt6 / KDE Frameworks 6, **Wayland puro**, **pt-BR e en_US**, temas **Sistema / Claro / Escuro / Clássico (Process Hacker)**, tabelas ordenáveis e estáveis (sem "pulo" ao atualizar).
- **Ícone na bandeja do sistema**: reabrir a janela (fechar só minimiza), **verificar atualizações** e alternar **iniciar com o sistema** (entrada XDG autostart).
- **Atualização dentro do app**: veja [Atualizações](#atualizações).
- **Intervalo de atualização** com opção **Parar (manual)** — desliga o polling e os rescans por evento em tempo real, só atualiza no F5. Com o intervalo ativo, eventos em tempo real (cn_proc) respeitam o valor escolhido como piso, em vez de disparar rescans soltos toda hora num sistema ocupado.

## Instalação

### Instalação num comando (curl)

Cola num terminal Linux (Fedora / Nobara, Arch e derivados, Debian 13+ / Ubuntu 24.10+). O script instala as dependências que faltam, compila e instala a GUI e o atualizador (no seu usuário) e o helper privilegiado (via `sudo` ou `pkexec`):

```bash
curl -fsSL https://raw.githubusercontent.com/gabrielmf1998/ProcForge/main/install.sh | bash
```

Alternativa pelo GitLab:

```bash
curl -fsSL https://gitlab.com/gabriel17166/ProcForge/-/raw/main/install.sh | bash
```

Depois é só abrir **ProcForge** no menu, ou rodar `procforge`.

### Pacote RPM (Fedora / Nobara / Bazzite)

Baixe o `.rpm` da página de [Releases](https://github.com/gabrielmf1998/ProcForge/releases) e:

```bash
# Fedora / Nobara (tradicional)
sudo dnf install ./procforge-*.rpm

# Bazzite e outros atômicos (rpm-ostree) — requer reboot
rpm-ostree install ./procforge-*.rpm && systemctl reboot
```

A spec fica em [`packaging/procforge.spec`](packaging/procforge.spec); para gerar o `.rpm` você mesmo:

```bash
rpmbuild -ba packaging/procforge.spec   # com o tarball procforge-<versão>.tar.gz em ~/rpmbuild/SOURCES
```

### Pacote .deb (Debian / Ubuntu)

Baixe o `.deb` da página de [Releases](https://github.com/gabrielmf1998/ProcForge/releases) e:

```bash
sudo apt install ./procforge_*_amd64.deb
```

Empacotamento em [`debian/`](debian/) (padrão debhelper/dpkg-buildpackage). Requer uma versão do
Debian/Ubuntu com **KDE Frameworks 6** nos repositórios (Debian 13 "trixie"+, Ubuntu 24.10+); em
versões mais antigas (sem KF6) use o AppImage. Para gerar o `.deb` você mesmo:

```bash
sudo apt build-dep .   # ou instale as Build-Depends listadas em debian/control
dpkg-buildpackage -b -uc -us
```

### Pacote Arch Linux (PKGBUILD)

`PKGBUILD` em [`packaging/PKGBUILD`](packaging/PKGBUILD):

```bash
cd packaging
makepkg -si   # compila e instala; pede sudo só para os pacotes de dependência
```

### AppImage (qualquer distro com KDE Plasma)

Baixe o `.AppImage` da página de [Releases](https://github.com/gabrielmf1998/ProcForge/releases):

```bash
chmod +x ProcForge-*.AppImage
./ProcForge-*.AppImage
```

Não precisa instalar nada — roda em qualquer distro Linux com KDE Plasma 6 / Wayland. **Ressalva
importante:** o AppImage empacota só a GUI (`procforge`). O helper privilegiado (`procforged`) —
que faz sinais/renice/afinidade em processo alheio, leitura/escrita de memória, injeção, cgroups,
namespaces e eBPF — precisa de arquivos instalados em caminhos fixos do sistema (unit systemd,
policy do polkit, config do D-Bus) e **não cabe dentro de um AppImage portátil**. Rodar o AppImage
sem o helper instalado dá acesso completo aos **seus próprios processos** (mesmo uid); ações sobre
processos de outro usuário/root pedem `./install-helper.sh` (ou um dos pacotes acima) à parte.

### A partir do código-fonte

```bash
git clone https://github.com/gabrielmf1998/ProcForge.git
cd ProcForge
./install.sh            # instala deps, compila e instala tudo
# ou manualmente:
cmake -B build -G Ninja && cmake --build build
QT_QPA_PLATFORM=wayland ./build/procforge          # rodar sem instalar
pkexec ./install-helper.sh                          # instalar só o helper privilegiado
```

### Atualizações

**Ajuda › Verificar atualizações** mostra as notas da versão nova e instala com um clique. Por baixo, o `procforge-update` (instalado junto) descobre como o ProcForge foi instalado e atualiza do jeito certo: troca o AppImage, usa `pacman`/`dnf`/`apt` para os pacotes, ou recompila o código-fonte da release no mesmo lugar. Todo arquivo é conferido contra o `SHA256SUMS` publicado na release antes de qualquer troca; sem ele, nada é instalado. A checagem automática (ao abrir e a cada 12 h) só avisa, e pode ser desligada em Hacker › Opções.

Pelo terminal:

```bash
procforge-update --check     # mostra a versão instalada e a última
procforge-update             # atualiza (pergunta antes)
procforge-update --version 0.5.0   # instala uma versão específica
```

## Requisitos

**Para compilar** (o `install.sh` instala tudo via `dnf`, `pacman` ou `apt`; nomes abaixo do Fedora):

- `cmake` (≥ 3.20), `ninja-build`, `gcc-c++` (C++20), `extra-cmake-modules`, `gettext`
- Qt6: `qt6-qtbase-devel`
- KF6: `kf6-kcoreaddons-devel`, `kf6-ki18n-devel`, `kf6-kwidgetsaddons-devel`

**Em execução:**

- **KDE Plasma 6 em Wayland** (o controle de janelas usa o KWin scripting) e Qt6/KF6.
- `polkit` (autorização das ações privilegiadas) e `systemd` (ativação do helper).
- Para funcionalidades específicas: `bpftrace` (eBPF), `iproute` (`ss`, aba Rede), `util-linux` (`nsenter`), `libcap` (`capsh`, aba Token).
- **Opcional:** `rust` + `cargo` — se presentes na hora de compilar, o núcleo de leitura/escrita de memória do helper é construído em **Rust** (memória segura). Sem eles, usa o caminho em C++ (idêntico em função).

## Arquitetura e segurança

Dois binários, um barramento, **política por poder concedido**:

- **`procforge`** — a GUI, roda **como você, sem privilégio**. Faz o caminho rápido direto (mesmo uid); no `EPERM` ou ação intrinsecamente privilegiada, chama o helper por D-Bus (**assíncrono** — a janela não congela no diálogo de senha).
- **`procforged`** — helper D-Bus de sistema, **ativado sob demanda**, roda como root e **reduz imediatamente para capabilities mínimas + seccomp** (sandbox da unit systemd). Idle-exit. **Cada método = uma action polkit distinta** (matar-alheio, escrever-memória, fechar-fd, injetar-lib… são autorizações separadas). Valida identidade do alvo por `(pid, starttime)` (anti-reuso) e audita tudo no journald.

O split resolve a segurança de memória de forma limpa: quem escreve na memória alheia é o **helper**, que não tem Qt — pode ser (e é, quando há toolchain) **Rust puro**, exatamente onde a garantia paga.

## Como usar

- **Botão direito** num processo: sinais, suspender, matar árvore, prioridade/afinidade/escalonamento, **Scanner de memória**, **Injetar biblioteca**, **Limitar recursos (cgroup)**, **eBPF**, **Namespaces**, **Janela**, **Propriedades**.
- **Duplo-clique** ou **Enter**: abre o inspetor de **Propriedades** (7 abas).
- **Atalhos**: Del termina, Shift+Del mata a árvore, Ctrl+C copia a linha, Ctrl+P vai para o pai, Ctrl+K ou Ctrl+F vai para a busca, Ctrl+I abre a informação do sistema, Ctrl+S salva a lista, F5 atualiza.
- **Abas** no topo: Processos, Serviços, Rede, Disco.
- Ações em processos de **outro usuário/root** disparam o **diálogo do polkit**.
- **Fechar a janela** minimiza para a **bandeja** (não encerra o ProcForge); **Sair** é que fecha de verdade — pelo menu Hacker ou pelo menu do ícone na bandeja.

## Idiomas e temas

- **Ver › Idioma**: Automático / Português (Brasil) / English (US) — reinicia para aplicar.
- **Ver › Tema**: Sistema (Breeze) / Claro / Escuro / **Clássico (Process Hacker)** — ao vivo. O tema Clássico reproduz o visual do Process Hacker do Windows.
- Também em **Hacker › Opções**.

## Estado do projeto

Projeto pessoal, funcional e em evolução. Fases 0→4 completas (lista de processos e manipulação, helper D-Bus + polkit, scanner de memória, injeção + cgroup + eBPF + namespaces + cn_proc + janelas), mais ícone de bandeja com verificação de atualização e autostart, e empacotamento para RPM/.deb/Arch/AppImage. Débitos técnicos e ideias em [`docs/STATUS.md`](docs/STATUS.md). Histórico em [`CHANGELOG.md`](CHANGELOG.md) e notas de cada versão em [`docs/releases/`](docs/releases/).

**Testes:** `ctest --test-dir build` roda testes de unidade, ações reais contra apps falsos (`tests/fake/fakeapp.cpp`, nunca processos seus), a GUI inteira em modo offscreen e o atualizador contra um servidor de releases falso. O CI roda tudo em Arch, Fedora e Debian a cada push. Como publicar uma versão: [`docs/RELEASING.md`](docs/RELEASING.md).

## Licença e créditos

- **Licença:** [GPL-3.0-or-later](LICENSE). Código **original**, escrito para Linux.
- **Inspiração:** [Process Hacker / System Informer](https://systeminformer.sourceforge.io/) (Wen Jia Liu / winsiderss), sob GPL — um projeto excelente. O ProcForge **não** é afiliado nem contém código dele; é uma reimplementação independente, traduzindo a filosofia da ferramenta para as primitivas nativas do Linux.
- Construído com **Qt6** e **KDE Frameworks 6**.
