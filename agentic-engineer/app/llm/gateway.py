import json
import os
from typing import Any, Dict

import requests

from app.core.schemas import LLMPlan


def make_plan(context: Dict[str, Any], evidence: str) -> LLMPlan:
    if not os.getenv("LLM_API_KEY"):
        return LLMPlan(
            plan_summary="LLM 未配置，返回空计划",
            steps=[],
            risk_notes=["LLM API 未配置"],
            done_when="DoD 通过",
        )
    base_url = os.getenv("LLM_BASE_URL", "http://127.0.0.1:8000")
    endpoint = os.getenv("LLM_ENDPOINT", "/v1/chat/completions")
    model = os.getenv("LLM_MODEL", "gpt-4o-mini")
    timeout_sec = float(os.getenv("LLM_TIMEOUT_SEC", "30"))
    url = f"{base_url.rstrip('/')}{endpoint}"

    system_prompt = os.getenv(
        "LLM_SYSTEM_PROMPT",
        "You are an engineer agent that outputs strictly JSON plans.",
    )
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {
                "role": "user",
                "content": json.dumps({"context": context, "evidence": evidence}, ensure_ascii=False),
            },
        ],
        "temperature": 0.2,
    }
    headers = {
        "Authorization": f"Bearer {os.getenv('LLM_API_KEY')}",
        "Content-Type": "application/json",
    }

    response = requests.post(url, json=payload, headers=headers, timeout=timeout_sec)
    response.raise_for_status()
    data = response.json()

    content = (
        data.get("choices", [{}])[0]
        .get("message", {})
        .get("content")
    )
    if not content:
        raise ValueError("LLM response missing message content")
    return parse_plan(content)


def parse_plan(payload: str) -> LLMPlan:
    data = json.loads(payload)
    return LLMPlan(**data)
