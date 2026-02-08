import json
import os
from typing import Any, Dict

from app.core.schemas import LLMPlan


def make_plan(context: Dict[str, Any], evidence: str) -> LLMPlan:
    if not os.getenv("LLM_API_KEY"):
        return LLMPlan(
            plan_summary="LLM 未配置，返回空计划",
            steps=[],
            risk_notes=["LLM API 未配置"],
            done_when="DoD 通过",
        )
    raise NotImplementedError("LLM gateway integration not configured")


def parse_plan(payload: str) -> LLMPlan:
    data = json.loads(payload)
    return LLMPlan(**data)
