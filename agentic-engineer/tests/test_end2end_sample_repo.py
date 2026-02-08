import os
import shutil
from pathlib import Path

from app.core.orchestrator import run_task
from app.core.schemas import LLMPlan, ToolStep
from app.storage import models
from app.storage.db import init_db


def _planner(context, evidence):
    workspace = context["workspace"]
    patch = """diff --git a/src/calculator.py b/src/calculator.py
index e69de29..f77b5c4 100644
--- a/src/calculator.py
+++ b/src/calculator.py
@@ -1,2 +1,2 @@
 def add(a: int, b: int) -> int:
-    return a - b
+    return a + b
"""
    return LLMPlan(
        plan_summary="Fix add implementation",
        steps=[
            ToolStep(
                tool="apply_patch",
                args={"root": workspace, "patch": patch},
                why="Fix arithmetic bug",
            )
        ],
        risk_notes=[],
        done_when="pytest passes",
    )


def test_end2end_sample_repo(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    fixture = Path(__file__).resolve().parents[1] / "fixtures" / "sample_py_repo"
    shutil.copytree(fixture, workspace)

    monkeypatch.setenv("WORKSPACE_ROOT", str(workspace))
    monkeypatch.setenv("AGENT_DATA_DIR", str(tmp_path / "agent_data"))

    init_db()
    task_id = models.create_task(
        instruction="修复失败测试",
        dod_command="pytest -q",
        workspace_path=str(workspace),
        max_iters=3,
        timeout_sec=300,
    )

    run_task(task_id, planner=_planner)
    task = models.get_task(task_id)

    assert task["status"] == "succeeded"
    artifacts = models.list_artifacts(task_id)
    assert any(artifact["type"] == "patch" for artifact in artifacts)
    verify_log = next(artifact for artifact in artifacts if artifact["type"] == "log")
    assert os.path.exists(verify_log["path"])
