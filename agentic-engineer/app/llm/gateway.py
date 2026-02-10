import json
import os
import re
from typing import Any, Dict, Optional

import requests

from app.core.schemas import LLMPlan
from app.telemetry.logger import log_event


def make_plan(context: Dict[str, Any], evidence: str) -> LLMPlan:
    if not os.getenv("LLM_API_KEY"):
        _log_llm_event(
            "llm_skipped",
            {
                "reason": "LLM_API_KEY missing",
            },
        )
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
    extra_headers = _load_extra_headers()
    if extra_headers:
        headers.update(extra_headers)

    _log_llm_event(
        "llm_request",
        {
            "url": url,
            "model": model,
            "timeout_sec": timeout_sec,
            "payload": payload,
            "headers": _redact_headers(headers),
        },
    )
    try:
        response = requests.post(url, json=payload, headers=headers, timeout=timeout_sec)
    except Exception as exc:
        _log_llm_event(
            "llm_error",
            {
                "stage": "request",
                "error": str(exc),
                "url": url,
            },
        )
        raise
    response.raise_for_status()
    data = response.json()
    _log_llm_event(
        "llm_response",
        {
            "status_code": response.status_code,
            "response": data,
        },
    )

    content = (
        data.get("choices", [{}])[0]
        .get("message", {})
        .get("content")
    )
    if not content:
        raise ValueError("LLM response missing message content")
    return parse_plan(content)


def parse_plan(payload: str) -> LLMPlan:
    data = _parse_json_payload(payload)
    data = _normalize_plan_payload(data)
    return LLMPlan(**data)


def _normalize_plan_payload(data: Dict[str, Any]) -> Dict[str, Any]:
    """Normalize legacy planner schemas to current ``LLMPlan`` shape."""
    if _is_current_plan_schema(data):
        return data

    legacy_steps = data.get("plan")
    if not isinstance(legacy_steps, list):
        return data

    steps = []
    for raw_step in legacy_steps:
        if not isinstance(raw_step, dict):
            continue
        if {"tool", "args", "why"}.issubset(raw_step.keys()):
            steps.append(raw_step)
            continue
        steps.append(_legacy_step_to_tool_step(raw_step))

    plan_summary = str(data.get("plan_summary") or data.get("summary") or "Generated execution plan")
    risk_notes = data.get("risk_notes")
    if not isinstance(risk_notes, list):
        risk_notes = ["Legacy plan format received from LLM"]

    done_when = str(data.get("done_when") or "All planned steps are completed")
    intent = str(data.get("intent") or "fix_code")

    return {
        "intent": intent,
        "plan_summary": plan_summary,
        "steps": steps,
        "risk_notes": risk_notes,
        "done_when": done_when,
    }


def _is_current_plan_schema(data: Dict[str, Any]) -> bool:
    required_keys = {"plan_summary", "steps", "risk_notes", "done_when"}
    return required_keys.issubset(data.keys())


def _legacy_step_to_tool_step(raw_step: Dict[str, Any]) -> Dict[str, Any]:
    cmd = raw_step.get("cmd") or raw_step.get("command")
    step_why = raw_step.get("why") or raw_step.get("description") or raw_step.get("action") or "legacy step"

    if cmd:
        return {
            "tool": "run_cmd",
            "args": {
                "cmd": str(cmd),
                "cwd": str(raw_step.get("cwd") or "/workspace"),
                "timeout_sec": int(raw_step.get("timeout_sec") or 120),
            },
            "why": str(step_why),
        }

    return {
        "tool": "run_cmd",
        "args": {
            "cmd": "echo legacy step",
            "cwd": str(raw_step.get("cwd") or "/workspace"),
            "timeout_sec": int(raw_step.get("timeout_sec") or 30),
        },
        "why": str(step_why),
    }


def _parse_json_payload(payload: str) -> Dict[str, Any]:
    try:
        return json.loads(payload)
    except json.JSONDecodeError:
        pass
    fenced = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", payload, re.DOTALL)
    if fenced:
        return json.loads(fenced.group(1))
    first_brace = payload.find("{")
    last_brace = payload.rfind("}")
    if first_brace != -1 and last_brace != -1 and last_brace > first_brace:
        return json.loads(payload[first_brace:last_brace + 1])
    raise ValueError("Unable to parse JSON payload from LLM response")


def _log_llm_event(event_type: str, payload: Dict[str, Any]) -> None:
    if not _is_debug_enabled():
        return
    max_chars = int(os.getenv("LLM_LOG_MAX_CHARS", "2000"))
    log_event(event_type, _truncate_payload(payload, max_chars))


def _is_debug_enabled() -> bool:
    return os.getenv("LLM_DEBUG_LOG", "").lower() in {"1", "true", "yes", "on"}


def _truncate_payload(payload: Dict[str, Any], max_chars: int) -> Dict[str, Any]:
    serialized = json.dumps(payload, ensure_ascii=False)
    if len(serialized) <= max_chars:
        return payload
    return {"truncated": serialized[:max_chars] + "..."}


def _redact_headers(headers: Dict[str, str]) -> Dict[str, str]:
    redacted = dict(headers)
    if "Authorization" in redacted:
        redacted["Authorization"] = "Bearer ***"
    return redacted


def _load_extra_headers() -> Optional[Dict[str, str]]:
    extra_headers = os.getenv("LLM_EXTRA_HEADERS")
    if not extra_headers:
        return None
    return json.loads(extra_headers)
