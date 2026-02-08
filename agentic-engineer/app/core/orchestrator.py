import json
import os
from datetime import datetime
from typing import Any, Callable, Dict, Optional

from app.core.agent import build_context, exec_plan, extract_evidence
from app.core.schemas import LLMPlan
from app.llm.gateway import make_plan
from app.storage import models
from app.tools.cmd_tools import run_cmd
from app.tools.git_tools import git_diff
from app.telemetry.logger import log_event

EventEmitter = Optional[Callable[[str, Dict[str, Any]], None]]


def run_task(task_id: str, emit: EventEmitter = None, planner: Optional[Callable[[Dict[str, Any], str], LLMPlan]] = None) -> None:
    task = models.get_task(task_id)
    if not task:
        return
    models.update_task_status(task_id, "running")
    if emit:
        emit("task_started", {"task_id": task_id})
    log_event("task_started", {"task_id": task_id})

    workspace = task["workspace_path"]
    max_iters = task["max_iters"]
    timeout_sec = task["timeout_sec"]
    dod_cmd = task["dod_command"]
    planner = planner or make_plan

    last_verify = None
    for iteration in range(max_iters):
        if emit:
            emit("iter_started", {"task_id": task_id, "iter": iteration})
        run_id = models.create_run(task_id, iteration)
        verify = run_cmd(dod_cmd, cwd=workspace, timeout_sec=timeout_sec, env={"PYTHONUNBUFFERED": "1"})
        last_verify = verify
        if verify["exit_code"] == 0:
            models.finish_run(run_id, "ok", "DoD passed")
            models.update_task_status(task_id, "succeeded")
            _write_artifacts(task_id, workspace, verify)
            if emit:
                emit("task_finished", {"task_id": task_id, "status": "succeeded"})
            return

        evidence = extract_evidence(verify)
        context = build_context(task, workspace)
        plan = planner(context, evidence)
        tool_results = exec_plan(
            plan,
            run_id,
            lambda name, args, output, ok, started_at, ended_at: models.record_tool_call(
                run_id, name, args, output, ok, started_at, ended_at
            ),
        )
        summary = json.dumps({"plan": plan.model_dump(), "results": tool_results}, ensure_ascii=False)
        models.finish_run(run_id, "fail", summary)
        if emit:
            emit("iter_finished", {"task_id": task_id, "iter": iteration, "status": "fail"})

    models.update_task_status(task_id, "failed")
    if last_verify:
        _write_artifacts(task_id, workspace, last_verify)
    if emit:
        emit("task_finished", {"task_id": task_id, "status": "failed"})


def _write_artifacts(task_id: str, workspace: str, verify: Dict[str, Any]) -> None:
    base_dir = os.path.join(os.getenv("AGENT_DATA_DIR", "/agent_data"), "artifacts", task_id)
    os.makedirs(base_dir, exist_ok=True)

    verify_path = os.path.join(base_dir, "verify.log")
    with open(verify_path, "w", encoding="utf-8") as handle:
        handle.write(verify.get("stdout", ""))
        handle.write("\n")
        handle.write(verify.get("stderr", ""))
    models.record_artifact(task_id, "log", verify_path)

    diff = git_diff(workspace).get("diff", "")
    patch_path = os.path.join(base_dir, "final.patch")
    with open(patch_path, "w", encoding="utf-8") as handle:
        handle.write(diff)
    if diff.strip():
        models.record_artifact(task_id, "patch", patch_path)

    report_path = os.path.join(base_dir, "report.md")
    report = _format_report(task_id, diff, verify)
    with open(report_path, "w", encoding="utf-8") as handle:
        handle.write(report)
    models.record_artifact(task_id, "report", report_path)


def _format_report(task_id: str, diff: str, verify: Dict[str, Any]) -> str:
    status = "succeeded" if verify.get("exit_code") == 0 else "failed"
    return "\n".join(
        [
            f"# Task Report ({task_id})",
            "",
            f"- Status: {status}",
            "",
            "## Changes",
            diff if diff.strip() else "No changes.",
            "",
            "## Verification",
            f"Exit code: {verify.get('exit_code')}",
        ]
    )
