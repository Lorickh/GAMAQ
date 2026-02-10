import json

from app.llm.gateway import parse_plan


def test_parse_plan_with_code_fence() -> None:
    payload = {
        "plan_summary": "test plan",
        "steps": [{"tool": "noop", "args": {}, "why": "testing"}],
        "risk_notes": ["none"],
        "done_when": "done",
    }
    message = f"Here is the plan:\\n```json\\n{json.dumps(payload)}\\n```"
    plan = parse_plan(message)
    assert plan.plan_summary == "test plan"
    assert plan.steps[0].tool == "noop"


def test_parse_plan_with_leading_text() -> None:
    payload = {
        "plan_summary": "another plan",
        "steps": [{"tool": "noop", "args": {}, "why": "testing"}],
        "risk_notes": ["none"],
        "done_when": "done",
    }
    message = f"原始payload内容: {json.dumps(payload)}"
    plan = parse_plan(message)
    assert plan.plan_summary == "another plan"



def test_parse_plan_with_legacy_plan_shape_cmd_steps() -> None:
    payload = {
        "plan": [
            {"id": "1", "action": "inspect", "cmd": "pwd", "cwd": "/workspace/GAMAQ", "timeout_sec": 10},
        ]
    }
    plan = parse_plan(json.dumps(payload))
    assert plan.plan_summary == "Generated execution plan"
    assert plan.steps[0].tool == "run_cmd"
    assert plan.steps[0].args["cmd"] == "pwd"
    assert plan.risk_notes == ["Legacy plan format received from LLM"]


def test_parse_plan_with_legacy_plan_shape_without_cmd() -> None:
    payload = {
        "plan": [
            {"id": "1", "action": "analyze repository"},
        ]
    }
    plan = parse_plan(json.dumps(payload))
    assert plan.steps[0].tool == "run_cmd"
    assert plan.steps[0].args["cmd"] == "echo legacy step"
