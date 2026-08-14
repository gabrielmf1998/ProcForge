Name:           procforge
Version:        0.1.1
Release:        1%{?dist}
Summary:        Clone do Process Hacker para Linux — manipulação ativa de processos, memória e sistema

License:        GPL-3.0-or-later
URL:            https://github.com/gabrielmf1998/ProcForge
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules
BuildRequires:  gettext
BuildRequires:  qt6-qtbase-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kwidgetsaddons-devel

Requires:       qt6-qtbase
Requires:       kf6-kcoreaddons
Requires:       kf6-ki18n
Requires:       kf6-kwidgetsaddons
Requires:       polkit
Requires:       hicolor-icon-theme
# Recomendados para funcionalidades específicas (eBPF, rede, namespaces, token, janelas):
Recommends:     bpftrace
Recommends:     iproute
Recommends:     util-linux
Recommends:     libcap
Recommends:     kwin-wayland

%description
ProcForge é uma reimplementação independente do Process Hacker / System Informer
para Linux, com GUI nativa Qt6 / KDE Frameworks 6 em Wayland. Diferente de um
monitor, ele MANIPULA o sistema em tempo real: sinais e suspend/resume, kill de
árvore, renice/afinidade/escalonamento/ionice/prlimit, scanner e editor de memória
(estilo Cheat Engine), injeção de biblioteca (dlopen via ptrace), fechar descritores
alheios, limitar CPU/RAM/PIDs/I/O ao vivo via cgroups v2, rastreamento eBPF por
processo, entrar em namespaces de containers, e manipular janelas via KWin scripting.

A GUI nunca roda como root: ações privilegiadas passam por um helper D-Bus de sistema
com polkit por AÇÃO e capabilities mínimas.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja
%cmake_build

%install
%cmake_install

%post
%systemd_post procforged.service
/usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :

%postun
%systemd_postun procforged.service
if [ $1 -eq 0 ] ; then
    touch --no-create %{_datadir}/icons/hicolor &>/dev/null
    gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :
fi

%posttrans
gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :

%files
%license LICENSE
%doc README.md docs/STATUS.md
%{_bindir}/procforge
%{_libexecdir}/procforged
%{_datadir}/polkit-1/actions/org.procforge.helper.policy
%{_datadir}/dbus-1/system.d/org.procforge.Helper1.conf
%{_datadir}/dbus-1/system-services/org.procforge.Helper1.service
%{_unitdir}/procforged.service
%{_datadir}/applications/org.procforge.ProcForge.desktop
%{_datadir}/icons/hicolor/*/apps/procforge.png
%{_datadir}/locale/en/LC_MESSAGES/procforge.mo

%changelog
* Fri Aug 14 2026 Gabriel (kxinha) <gabriel17166@gmail.com> - 0.1.1-1
- Ícone definitivo do app (monitor/EKG) em PNG nos tamanhos do tema hicolor;
  atualização do cache de ícones no pós-instalação.

* Fri Aug 14 2026 Gabriel (kxinha) <gabriel17166@gmail.com> - 0.1.0-1
- Primeira versão empacotada: lista de processos, manipulação, scanner de memória,
  injeção, cgroup-bisturi, eBPF, namespaces, cn_proc, janelas KWin, temas e i18n.
