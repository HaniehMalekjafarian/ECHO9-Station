#!/usr/bin/env python3
"""Verify incremental builds in an isolated copy of the C project.

Does not modify the instructor's source tree or running AgentOS instance.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def build(project):
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "all"],
        cwd=project, capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, (result.stdout, result.stderr)


def main():
    with tempfile.TemporaryDirectory(prefix="agentos-build-check-") as tmp:
        project = Path(tmp)
        shutil.copy2(ROOT / "Makefile", project / "Makefile")
        shutil.copytree(ROOT / "src", project / "src")
        shutil.copytree(ROOT / "include", project / "include")

        build(project)
        binary = project / "build" / "agentos"
        objects = {p.stem: p for p in (project / "build").glob("*.o")}
        assert binary.is_file()
        assert set(objects) == {"main", "protocol", "backend", "ollama_backend",
                                "request_dispatcher", "agentos_runtime", "process_manager", "control_plane"}, objects
        for name in objects:
            assert (project / "build" / f"{name}.d").is_file()
        print("PASS: clean build produces binary, objects and dependency files")

        original = {name: path.stat().st_mtime_ns for name, path in objects.items()}
        binary_before = binary.stat().st_mtime_ns
        build(project)
        assert {name: p.stat().st_mtime_ns for name, p in objects.items()} == original
        assert binary.stat().st_mtime_ns == binary_before
        print("PASS: unchanged sources do not rebuild")

        # Avoid touching any real source file; guarantee a detectable timestamp.
        time.sleep(1.1)
        with (project / "include" / "protocol.h").open("a", encoding="utf-8") as stream:
            stream.write("\n/* Verify generated header dependencies. */\n")
        build(project)
        updated = {name: path.stat().st_mtime_ns for name, path in objects.items()}
        assert updated["main"] > original["main"], updated
        assert updated["protocol"] > original["protocol"], updated
        assert updated["backend"] == original["backend"], updated
        assert updated["ollama_backend"] == original["ollama_backend"], updated
        assert updated["request_dispatcher"] == original["request_dispatcher"], updated
        assert binary.stat().st_mtime_ns > binary_before
        print("PASS: changed header rebuilds dependent objects and relinks")

        second = {name: path.stat().st_mtime_ns for name, path in objects.items()}
        build(project)
        assert {name: path.stat().st_mtime_ns for name, path in objects.items()} == second
        print("PASS: post-rebuild no-op build stays unchanged")

    print("\nAll build-dependency checks passed!")


if __name__ == "__main__":
    main()
