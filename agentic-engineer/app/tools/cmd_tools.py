import os
import subprocess
import time
from typing import Dict, Optional

from app.tools.policy import check_cmd


def run_cmd(cmd: str, cwd: str, timeout_sec: int, env: Optional[Dict[str, str]] = None) -> Dict:
    check_cmd(cmd)
    start = time.time()
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            shell=True,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            env=merged_env,
        )
        duration_ms = int((time.time() - start) * 1000)
        return {
            "exit_code": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "duration_ms": duration_ms,
        }
    except subprocess.TimeoutExpired as exc:
        duration_ms = int((time.time() - start) * 1000)
        return {
            "exit_code": 124,
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "timeout",
            "duration_ms": duration_ms,
        }
