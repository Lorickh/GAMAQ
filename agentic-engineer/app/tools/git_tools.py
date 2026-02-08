import subprocess
from typing import Dict


def git_diff(cwd: str) -> Dict:
    result = subprocess.run(["git", "diff"], cwd=cwd, capture_output=True, text=True)
    return {"diff": result.stdout}
