import json
import uuid
from datetime import datetime
from typing import Any, Dict, List, Optional

from app.storage.db import get_connection


def _now() -> str:
    return datetime.utcnow().isoformat()


def create_task(
    instruction: str,
    dod_command: str,
    workspace_path: str,
    max_iters: int,
    timeout_sec: int,
) -> str:
    task_id = str(uuid.uuid4())
    with get_connection() as conn:
        conn.execute(
            """
            INSERT INTO tasks (id, created_at, status, instruction, dod_command, workspace_path, max_iters, timeout_sec)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                task_id,
                _now(),
                "queued",
                instruction,
                dod_command,
                workspace_path,
                max_iters,
                timeout_sec,
            ),
        )
        conn.commit()
    return task_id


def update_task_status(task_id: str, status: str) -> None:
    with get_connection() as conn:
        conn.execute("UPDATE tasks SET status = ? WHERE id = ?", (status, task_id))
        conn.commit()


def get_task(task_id: str) -> Optional[Dict[str, Any]]:
    with get_connection() as conn:
        row = conn.execute("SELECT * FROM tasks WHERE id = ?", (task_id,)).fetchone()
    return dict(row) if row else None


def list_artifacts(task_id: str) -> List[Dict[str, Any]]:
    with get_connection() as conn:
        rows = conn.execute("SELECT * FROM artifacts WHERE task_id = ?", (task_id,)).fetchall()
    return [dict(row) for row in rows]


def create_run(task_id: str, iteration: int) -> str:
    run_id = str(uuid.uuid4())
    with get_connection() as conn:
        conn.execute(
            """
            INSERT INTO runs (id, task_id, iter, started_at, result, summary)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (run_id, task_id, iteration, _now(), "running", ""),
        )
        conn.commit()
    return run_id


def finish_run(run_id: str, result: str, summary: str) -> None:
    with get_connection() as conn:
        conn.execute(
            "UPDATE runs SET ended_at = ?, result = ?, summary = ? WHERE id = ?",
            (_now(), result, summary, run_id),
        )
        conn.commit()


def record_tool_call(
    run_id: str,
    tool_name: str,
    input_json: Dict[str, Any],
    output_json: Dict[str, Any],
    ok: bool,
    started_at: str,
    ended_at: str,
) -> None:
    call_id = str(uuid.uuid4())
    with get_connection() as conn:
        conn.execute(
            """
            INSERT INTO tool_calls (id, run_id, tool_name, input_json, output_json, started_at, ended_at, ok)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                call_id,
                run_id,
                tool_name,
                json.dumps(input_json, ensure_ascii=False),
                json.dumps(output_json, ensure_ascii=False),
                started_at,
                ended_at,
                1 if ok else 0,
            ),
        )
        conn.commit()


def record_artifact(task_id: str, artifact_type: str, path: str) -> None:
    with get_connection() as conn:
        conn.execute(
            """
            INSERT INTO artifacts (id, task_id, type, path, created_at)
            VALUES (?, ?, ?, ?, ?)
            """,
            (str(uuid.uuid4()), task_id, artifact_type, path, _now()),
        )
        conn.commit()
