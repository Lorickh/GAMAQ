from typing import Any, Dict, List, Optional

from pydantic import BaseModel, Field


class TaskCreate(BaseModel):
    instruction: str
    dod_command: str
    workspace_path: str = "/workspace"
    max_iters: int = 8
    timeout_sec: int = 1800


class TaskResponse(BaseModel):
    id: str
    status: str


class ToolStep(BaseModel):
    tool: str
    args: Dict[str, Any]
    why: str


class LLMPlan(BaseModel):
    intent: str = Field(default="fix_code")
    plan_summary: str
    steps: List[ToolStep]
    risk_notes: List[str]
    done_when: str


class TaskStatus(BaseModel):
    id: str
    status: str
    instruction: str
    dod_command: str


class ArtifactInfo(BaseModel):
    id: str
    type: str
    path: str
    created_at: str


class IndexQuery(BaseModel):
    query: str
    top_k: int = 8


class IndexRebuild(BaseModel):
    path: Optional[str] = None
    include_glob: List[str] = ["**/*.py", "**/*.md", "**/*.txt"]
    exclude_glob: List[str] = ["**/.git/**", "**/dist/**"]
