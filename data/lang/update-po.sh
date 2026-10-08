#!/bin/sh
# data/lang/update-po.sh [LANG...]
#
# Extracts the overlay's strings into data/lang/sm2-emu.pot and merges the
# template into every catalog in data/lang (or just the named ones), keeping
# existing translations. A new language starts with
#   msginit -i data/lang/sm2-emu.pot -l <code> -o data/lang/<code>.po --no-translator
# then add X-Language-Name (its own name) and X-Language-Name-English headers.
#
# Strings are marked in the source with tr(), tr_id(), trf(), trnf(), N_() and
# text_wrapped(). Only trf() and trnf() strings are printf formats.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LANG_DIR=$ROOT/data/lang
POT=$LANG_DIR/sm2-emu.pot

cd "$ROOT"
xgettext --language=C++ --from-code=UTF-8 --add-comments=TRANSLATORS: \
    --package-name=sm2-emu --msgid-bugs-address=https://github.com/dmanlfc/sm2-emu/issues \
    --keyword= --keyword=tr --keyword=tr_id --keyword=trf --keyword=trnf:1,2 \
    --keyword=N_ --keyword=text_wrapped \
    --flag=tr:1:no-c-format --flag=tr_id:1:no-c-format --flag=N_:1:no-c-format \
    --flag=text_wrapped:1:no-c-format \
    --flag=trf:1:c-format --flag=trnf:1:c-format --flag=trnf:2:c-format \
    --add-location=file --sort-by-file -o "$POT" src/osd/gui.cpp src/main.cpp

if [ $# -eq 0 ]; then
    set -- $(cd "$LANG_DIR" && ls *.po 2>/dev/null | sed 's/\.po$//')
fi
for code in "$@"; do
    msgmerge --quiet --update --backup=none --no-fuzzy-matching "$LANG_DIR/$code.po" "$POT"
    printf '%s: ' "$code"
    msgfmt --check-format --statistics -o /dev/null "$LANG_DIR/$code.po"
done
