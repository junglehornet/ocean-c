"""Check both version aliases without needing an Ocean project or library."""
import re
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    outputs = []
    for flag in ("-v", "--version"):
        result = subprocess.run(
            [sys.argv[1], flag], cwd=directory, capture_output=True, text=True
        )
        assert result.returncode == 0, result
        assert not result.stderr, result.stderr
        assert re.fullmatch(
            r"Ocean compiler \(ocean-c\) .+\n"
            r"Commit ([0-9a-f]{40,64}|unknown) "
            r"\((\d{4}-\d{2}-\d{2}|unknown)\), built "
            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z\n"
            r"Copyright Connor Egan (\d{4}|unknown)\n",
            result.stdout,
        ), result.stdout
        outputs.append(result.stdout)
    assert outputs[0] == outputs[1], outputs
