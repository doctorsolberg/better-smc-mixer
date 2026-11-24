"""Patch the SDK's generated linker script (cpu/br25/sdk_ld.c after cpp) for our image.

Usage: python3 -I sdk_ld_patch.py IN.ld OUT.ld

- INCLUDE smclog.ld at the top of the first SECTIONS block: the log call sites, not loaded
  (firmware/lib/smclog/smclog.ld);
- the GNU build-id note in flash, in .text right after the startup code, as __build_id_note
  (read by the smclog port for the stream header);
- .smc_keep, NOLOAD, between the end of the RAM code (data_code_limit_end, where the SDK's CPU
  write limit ends) and the heap: the RAM a reset keeps (firmware/platform/br25/diag_br25.c). The
  startup code clears only .bss and the heap starts after it.
Fails if a pattern is not found exactly once, so an SDK update cannot silently skip a patch.
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
ld = open(src).read()


def patch(text, pattern, repl, what):
    new, n = re.subn(pattern, repl, text, count=1)
    if n != 1 or len(re.findall(pattern, text)) != 1:
        sys.exit("sdk_ld_patch: %s: expected one match of %r" % (what, pattern))
    return new


ld = patch(ld, r"(\nSECTIONS\n\{\n)(?= \. = ORIGIN\(code0\);)",
           r"\1    INCLUDE smclog.ld\n", "smclog section")
ld = patch(ld, r"(\n([ \t]*)\*\(\.startup\.text\)\n)",
           r"\1\2. = ALIGN(4);\n\2__build_id_note = .;\n\2KEEP(*(.note.gnu.build-id))\n", "build-id note")
ld = patch(ld, r"(\n([ \t]*)data_code_limit_end = \.;\n)(?=\s*_HEAP_BEGIN = \.;)",
           r"\1\n\2.smc_keep (NOLOAD) : ALIGN(32)\n\2{\n\2    smc_keep_begin = .;\n\2    *(.smc_keep)\n"
           r"\2    . = ALIGN(32);\n\2    smc_keep_end = .;\n\2} > ram0\n", "keep section")
with open(dst, "w") as f:
    f.write("/* Generated from the SDK's sdk_ld.c and patched by firmware/app/sdk_ld_patch.py */\n" + ld)
