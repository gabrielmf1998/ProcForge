#!/usr/bin/env bash
# Regenera o catálogo de traduções (pt-BR é o base; en é traduzido).
set -e
cd "$(dirname "$0")"
xgettext --c++ --from-code=UTF-8 --keyword=i18n:1 --keyword=i18nc:1c,2 --keyword=i18np:1,2 \
  -o po/procforge.pot $(find src -name '*.cpp' -o -name '*.h')
python3 po/gen_en.py
DEST="$HOME/.local/share/locale/en/LC_MESSAGES"
mkdir -p "$DEST"
msgfmt po/en.po -o "$DEST/procforge.mo"
echo "traduções instaladas em $DEST/procforge.mo"
