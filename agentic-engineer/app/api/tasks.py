import asyncio
import threading
from typing import Dict

from fastapi import APIRouter, HTTPException

from app.api.events import bus, sse_events
from app.core.orchestrator import run_task
from app.core.schemas import ArtifactInfo, TaskCreate, TaskResponse, TaskStatus
from app.storage import models

router = APIRouter()


def _emit_sync(task_id: str, event_type: str, payload: Dict) -> None:
    asyncio.run(bus.publish(task_id, event_type, payload))


@router.post("/tasks", response_model=TaskResponse)
async def create_task(payload: TaskCreate) -> TaskResponse:
    task_id = models.create_task(
        instruction=payload.instruction,
        dod_command=payload.dod_command,
        workspace_path=payload.workspace_path,
        max_iters=payload.max_iters,
        timeout_sec=payload.timeout_sec,
    )

    thread = threading.Thread(target=run_task, args=(task_id, lambda et, pl: _emit_sync(task_id, et, pl)))
    thread.daemon = True
    thread.start()

    return TaskResponse(id=task_id, status="queued")


@router.get("/tasks/{task_id}", response_model=TaskStatus)
async def get_task(task_id: str) -> TaskStatus:
    task = models.get_task(task_id)
    if not task:
        raise HTTPException(status_code=404, detail="Task not found")
    return TaskStatus(
        id=task["id"],
        status=task["status"],
        instruction=task["instruction"],
        dod_command=task["dod_command"],
    )


@router.get("/tasks/{task_id}/artifacts", response_model=list[ArtifactInfo])
async def get_artifacts(task_id: str) -> list[ArtifactInfo]:
    return [ArtifactInfo(**artifact) for artifact in models.list_artifacts(task_id)]


@router.get("/tasks/{task_id}/events")
async def events(task_id: str):
    return sse_events(task_id)
