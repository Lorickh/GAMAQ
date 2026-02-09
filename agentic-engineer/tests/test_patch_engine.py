import os
import subprocess
from pathlib import Path

from app.tools.fs_tools import apply_patch


def test_apply_patch(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("WORKSPACE_ROOT", str(tmp_path))
    repo = tmp_path / "repo"
    repo.mkdir()
    subprocess.run(["git", "init"], cwd=repo, check=True, capture_output=True)
    subprocess.run(["git", "config", "user.email", "bot@example.com"], cwd=repo, check=True, capture_output=True)
    subprocess.run(["git", "config", "user.name", "Bot"], cwd=repo, check=True, capture_output=True)
    target = repo / "hello.txt"
    target.write_text("hello\n", encoding="utf-8")
    subprocess.run(["git", "add", "hello.txt"], cwd=repo, check=True, capture_output=True)
    subprocess.run(["git", "commit", "-m", "init"], cwd=repo, check=True, capture_output=True)

    patch = """diff --git a/hello.txt b/hello.txt
index 1f8770a..7a85d27 100644
--- a/hello.txt
+++ b/hello.txt
@@ -1 +1 @@
-hello
+hello world
"""
    result = apply_patch(str(repo), patch)
    assert result["ok"] is True
    assert target.read_text(encoding="utf-8").strip() == "hello world"
