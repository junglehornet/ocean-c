"""Synthetic enum literals use eight bytes even with Windows' 32-bit long."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

clang = sys.argv[1]
source_root = Path(__file__).resolve().parents[1]
source = (source_root / "semantic_enum.inc").read_text()
helper = re.search(r"static expr enumInteger\([^)]*\) \{.*?^\}", source, re.S | re.M).group()
# Replace standalone long with its Windows width, preserving long long itself.
helper = re.sub(r"\blong\s+long\b|\blong\b", lambda match: "int32_t" if match[0] == "long" else match[0], helper)
with tempfile.TemporaryDirectory(prefix="ocean-enum-storage-") as directory:
    root = Path(directory)
    source = root / "probe.c"
    source.write_text('''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
static size_t allocated;
static void *record_malloc(size_t size) { allocated = size; return malloc(size); }
#define malloc record_malloc
''' + helper + '''
#undef malloc
int main(void) {
    const int64_t values[] = {0, 1, 9, INT64_C(4294967297), -1};
    for (size_t i = 0; i < sizeof(values)/sizeof(*values); i++) {
        expr value = enumInteger(values[i], (token){0});
        struct expr_literal *literal = value.underlying;
        assert(allocated == sizeof(int64_t));
        int64_t actual;
        memcpy(&actual, literal->value, sizeof(actual));
        assert(actual == values[i]);
        free(literal->value); free(literal);
    }
}
''')
    binary = root / ("probe.exe" if os.name == "nt" else "probe")
    subprocess.run([clang, "-std=c11", "-I" + str(source_root), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Enum literal storage passed with emulated 32-bit long.")
