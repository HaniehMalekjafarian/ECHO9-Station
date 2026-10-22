import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="agentos-pcb-unit-") as td:
    cmd = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g",
           "-Iinclude", "src/process_manager.c", "tests/process_manager_unit.c",
           "-o", str(Path(td) / "pcb_tests")]
    subprocess.run(cmd, cwd=ROOT, check=True)
    subprocess.run([str(Path(td) / "pcb_tests")], cwd=ROOT, check=True)
