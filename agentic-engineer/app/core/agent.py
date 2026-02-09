import json
from datetime import datetime
from typing import Any, Callable, Dict, List, Optional

from app.core.schemas import LLMPlan
from app.tools.registry import get_tool


def build_context(task: Dict[str, Any], workspace: str) -> Dict[str, Any]:
    return {
        "task": task,
        "workspace": workspace,
    }


def extract_evidence(verify_result: Dict[str, Any], max_lines: int = 40) -> str:
    stdout = (verify_result.get("stdout") or "").splitlines()
    stderr = (verify_result.get("stderr") or "").splitlines()
    tail_stdout = "\n".join(stdout[-max_lines:])
    tail_stderr = "\n".join(stderr[-max_lines:])
    return "\n".join([tail_stdout, tail_stderr]).strip()


def exec_plan(
    plan: LLMPlan,
    run_id: str,
    tool_recorder: Callable[[str, Dict[str, Any], Dict[str, Any], bool, str, str], None],
) -> List[Dict[str, Any]]:
    results = []
    for step in plan.steps:
        tool = get_tool(step.tool)
        started_at = datetime.utcnow().isoformat()
        try:
            output = tool(**step.args)
            ok = True
        except Exception as exc:  # pragma: no cover - defensive
            output = {"error": str(exc)}
            ok = False
        ended_at = datetime.utcnow().isoformat()
        tool_recorder(step.tool, step.args, output, ok, started_at, ended_at)
        results.append({"tool": step.tool, "output": output, "ok": ok})
    return results


def serialize_plan(plan: LLMPlan) -> str:
    return json.dumps(plan.model_dump(), ensure_ascii=False, indent=2)
