from typing import Any, Callable, Dict

from app.tools import cmd_tools, fs_tools, git_tools, rag_tools

ToolFunc = Callable[..., Dict[str, Any]]

TOOLS: Dict[str, ToolFunc] = {
    "list_tree": fs_tools.list_tree,
    "read_file": fs_tools.read_file,
    "write_file": fs_tools.write_file,
    "apply_patch": fs_tools.apply_patch,
    "run_cmd": cmd_tools.run_cmd,
    "git_diff": git_tools.git_diff,
    "rag_rebuild": rag_tools.rag_rebuild,
    "rag_query": rag_tools.rag_query,
}


def get_tool(name: str) -> ToolFunc:
    if name not in TOOLS:
        raise KeyError(f"Unknown tool: {name}")
    return TOOLS[name]
