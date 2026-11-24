#!/usr/bin/env bash
# Disassemble the stock application from a flash dump (stage 0 research, R-3).
#
#   tools/re/disasm_stock.sh DUMP.bin [OUTDIR]     default OUTDIR: materials/re/v012
#
# Cuts app.bin out of the dump (flash 0x1120, 0x21DCC bytes on V012, see docs/hardware.md), wraps
# it in an ELF at the link address 0x1E00120 and disassembles it with the JieLi objdump in the
# build container. The output is vendor code: it stays under materials/ (not published, SPEC C-7).
# Navigate the listing with tools/re/listing.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUMP=${1:?usage: disasm_stock.sh DUMP.bin [OUTDIR]}
OUT=${2:-$ROOT/materials/re/v012}
APP_OFFSET=0x1120
APP_SIZE=0x21DCC
mkdir -p "$OUT"
case "$(cd "$OUT" && pwd)/" in "$ROOT"/*) ;; *) echo "disasm_stock: OUTDIR must be inside the repository (it is mounted at /src)" >&2; exit 1 ;; esac

python3 -I - "$DUMP" "$OUT/app.bin" "$APP_OFFSET" "$APP_SIZE" <<'EOF'
import hashlib, sys
d = open(sys.argv[1], "rb").read()
off, size = int(sys.argv[3], 16), int(sys.argv[4], 16)
app = d[off:off + size]
open(sys.argv[2], "wb").write(app)
print("app.bin: %d bytes, SHA-256 %s" % (len(app), hashlib.sha256(app).hexdigest()))
EOF
printf '.section .text,"ax"\n.incbin "app.bin"\n' > "$OUT/wrap.S"
REL=${OUT#"$ROOT"/}
"$ROOT/tools/env/run.sh" bash -c "set -e; cd '/src/$REL'; T=/opt/jieli/pi32v2/bin
    \$T/clang -target pi32v2 -c wrap.S -o wrap.o
    \$T/ld -Ttext=0x1E00120 -e 0x1E00120 -o app.elf wrap.o
    \$T/objdump -d -address-mask=0x1ffffff app.elf > app.lst
    echo \"app.lst: \$(wc -l < app.lst) lines\""
