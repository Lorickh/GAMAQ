import os
from typing import List

def _safe_prefixes() -> List[str]:
    return [
        os.getenv("WORKSPACE_ROOT", "/workspace"),
        os.getenv("AGENT_DATA_DIR", "/agent_data"),
    ]

BLOCKED_KEYWORDS = ["rm -rf /", "sudo", "chmod 777", "curl | sh", "mkfs", "dd if="]

ALLOWED_COMMAND_PREFIXES = [
    "pytest",
    "python",
    "python -m",
    "npm",
    "mvn",
    "gradle",
    "make",
    "go",
    "ls",
    "cat",
    "rg",
    "echo",
]


def check_path(path: str) -> None:
    if not any(path.startswith(prefix) for prefix in _safe_prefixes()):
        raise ValueError(f"Access denied for path: {path}")


def check_cmd(cmd: str) -> None:
    lowered = cmd.lower()
    if any(keyword in lowered for keyword in BLOCKED_KEYWORDS):
        raise ValueError(f"Blocked command: {cmd}")
    if not any(lowered.startswith(prefix) for prefix in ALLOWED_COMMAND_PREFIXES):
        raise ValueError(f"Command not allowed: {cmd}")
