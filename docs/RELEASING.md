# Como publicar uma versão

Este é o roteiro completo, para não depender de memória. Os remotos são
`origin` (GitHub, gabrielmf1998/ProcForge) e `gitlab` (GitLab, gabriel17166/ProcForge).
O `procforge-update` dos usuários depende de três coisas desta lista estarem
certas, que são os **nomes dos arquivos**, o **SHA256SUMS** e a **tag `vX.Y.Z`**.

## 1. Preparar a versão (num branch de trabalho)

Nunca dê ao branch o mesmo nome da tag (use algo como `melhorias-0.6`).

1. Versão em `CMakeLists.txt` (`project(procforge VERSION X.Y.Z ...)`). É a única
   fonte da versão do app, do helper (`Ping`) e do `--version`.
2. `packaging/PKGBUILD` (`pkgver`), `packaging/procforge.spec` (`Version` e entrada
   no `%changelog`, com o dia da semana correto) e `debian/changelog` (nova entrada
   no topo; confira com `dpkg-parsechangelog -l debian/changelog`).
3. Notas em `docs/releases/vX.Y.Z.md` (o CI recusa a tag sem esse arquivo) e um
   resumo em `CHANGELOG.md`.
4. Traduções. Para cada texto novo em `i18n(...)`, adicione a versão em inglês no
   dicionário `T` de `po/gen_en.py` e rode
   ```bash
   xgettext --c++ --from-code=UTF-8 --keyword=i18n:1 --keyword=i18nc:1c,2 --keyword=i18np:1,2 \
     -o po/procforge.pot $(find src -name '*.cpp' -o -name '*.h')
   python3 po/gen_en.py      # tem que terminar com "faltando: 0"
   ```
5. Compilar e testar localmente
   ```bash
   cmake -S . -B build -G Ninja && cmake --build build
   ctest --test-dir build --output-on-failure
   ```
   Os testes usam só apps falsos (`tests/fake/fakeapp.cpp`), nunca processos reais.
6. Commit e push do branch para o GitHub. O workflow **CI** compila com `-Werror` e
   roda os testes em Arch, Fedora e Debian. Só siga com tudo verde
   (`gh run list -R gabrielmf1998/ProcForge`).

## 2. Publicar

```bash
git checkout main && git merge --ff-only melhorias-X.Y
git tag -a vX.Y.Z -m "ProcForge X.Y.Z"
git push origin main vX.Y.Z
git push gitlab main vX.Y.Z
```

A tag dispara o workflow **Release** no GitHub, que gera e publica

| arquivo | uso |
|---|---|
| `procforge-X.Y.Z.tar.gz` | código-fonte (`git archive`, pasta `procforge-X.Y.Z/`) |
| `procforge-X.Y.Z-1-x86_64.pkg.tar.zst` | Arch |
| `procforge-X.Y.Z-1.fcNN.x86_64.rpm` | Fedora / Nobara / Bazzite |
| `procforge_X.Y.Z-1_amd64.deb` | Debian 13+ / Ubuntu 24.10+ |
| `ProcForge-X.Y.Z-x86_64.AppImage` | qualquer distro (só a GUI) |
| `SHA256SUMS` | conferido pelo `procforge-update` antes de instalar |

As notas da release vêm de `docs/releases/vX.Y.Z.md`. Se algum job falhar, corrija
num commit novo, mova a tag e rode de novo pelo `workflow_dispatch` (campo `tag`).

## 3. Espelhar no GitLab

O CI não tem token do GitLab; este passo é feito à mão depois que a release do
GitHub ficou completa.

```bash
V=X.Y.Z; D=$(mktemp -d)
gh release download v$V -R gabrielmf1998/ProcForge -D "$D"
(cd "$D" && sha256sum -c SHA256SUMS)          # tem que dar OK em tudo
glab release create v$V "$D"/* -R gabriel17166/ProcForge \
  --name "ProcForge $V" --notes-file docs/releases/v$V.md --use-package-registry
```

O `--use-package-registry` guarda os arquivos no registro de pacotes do projeto e
cria os links com `direct_asset_url`, que é o que o `procforge-update` usa quando o
GitHub está fora do ar.

## 4. Conferir

```bash
procforge-update --check                       # numa máquina com a versão anterior
curl -s https://api.github.com/repos/gabrielmf1998/ProcForge/releases/latest | grep tag_name
glab release view v$V -R gabriel17166/ProcForge
```

## 5. Instalar nesta máquina (Arch, instalação por código-fonte)

```bash
bash scripts/install-local.sh --prefix ~/.local --build build   # GUI e procforge-update
pkexec bash install-helper.sh                                    # helper em /usr
procforge --version
gdbus call --system --dest org.procforge.Helper1 --object-path /org/procforge/Helper1 \
  --method org.procforge.Helper1.Ping
```

Depois disso, as próximas versões chegam por Ajuda › Verificar atualizações.

## Como o atualizador decide o que fazer

`scripts/procforge-update` (instalado como `procforge-update` ao lado do binário)

1. descobre o tipo de instalação (`--detect`), sendo AppImage (`$APPIMAGE`), pacote
   pacman, dpkg ou rpm dono do binário, ou código-fonte (binário em `.../bin/procforge`);
2. lê a release (`/releases/latest` ou `/releases/tags/vX.Y.Z` do GitHub, com o GitLab
   como reserva);
3. baixa o arquivo certo e o `SHA256SUMS` e recusa instalar se o hash não bate ou se a
   release não tem `SHA256SUMS`;
4. instala (troca o AppImage, `pkexec pacman -U`, `pkexec dnf install`,
   `pkexec apt-get install`, ou compila o tarball e roda `install-local.sh` e
   `pkexec install-helper.sh`);
5. imprime `PROCFORGE-UPDATE-OK X.Y.Z`, que é o que a janela de atualização espera.

O helper antigo percebe que o binário foi trocado (`/proc/self/exe` com `(deleted)`)
e sai sozinho; o D-Bus ativa o novo na próxima chamada.

Variáveis para testes: `PROCFORGE_RELEASES_API`, `PROCFORGE_GITLAB_API`,
`PROCFORGE_UPDATE_NO_HELPER=1`. O teste `tests/test_updater.sh` usa um servidor de
releases falso em `127.0.0.1`.
