import json
import os
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
    data = json.loads(payload)
    return LLMPlan(**data)


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
