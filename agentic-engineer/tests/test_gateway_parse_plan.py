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
