import fnmatch
import os
import subprocess
import tempfile
from typing import Dict, List

from app.tools.policy import check_path


def list_tree(root: str, max_depth: int, include_glob: List[str], exclude_glob: List[str]) -> Dict:
    check_path(root)
    entries = []
    root = os.path.abspath(root)
    for dirpath, dirnames, filenames in os.walk(root):
        depth = os.path.relpath(dirpath, root).count(os.sep)
        if depth >= max_depth:
            dirnames[:] = []
        for filename in filenames:
            path = os.path.join(dirpath, filename)
            rel_path = os.path.relpath(path, root)
            if any(fnmatch.fnmatch(rel_path, pattern) for pattern in exclude_glob):
                continue
            if include_glob and not any(fnmatch.fnmatch(rel_path, pattern) for pattern in include_glob):
                continue
            entries.append(
                {
                    "path": rel_path,
                    "type": "file",
                    "size": os.path.getsize(path),
                }
            )
    return {"entries": entries}


def read_file(path: str, max_bytes: int) -> Dict:
    check_path(path)
    with open(path, "rb") as handle:
        content = handle.read(max_bytes + 1)
    truncated = len(content) > max_bytes
    return {
        "path": path,
        "content": content[:max_bytes].decode("utf-8", errors="replace"),
        "truncated": truncated,
    }


def write_file(path: str, content: str) -> Dict:
    check_path(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(content)
    return {"ok": True}


def apply_patch(root: str, patch: str) -> Dict:
    check_path(root)
    with tempfile.NamedTemporaryFile("w", delete=False, encoding="utf-8") as handle:
        handle.write(patch)
        temp_path = handle.name
    try:
        subprocess.run(
            ["git", "apply", "--whitespace=fix", temp_path],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as exc:
        return {"ok": False, "applied_files": [], "rejects": [exc.stderr.strip()]}
    finally:
        os.unlink(temp_path)
    return {"ok": True, "applied_files": [], "rejects": []}
