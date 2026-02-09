import fnmatch
import os
from pathlib import Path
from typing import List, Tuple

from app.rag.chunker import chunk_text
from app.rag.embedder import embed
from app.rag.vector_store import VectorEntry, VectorStore


def _index_path() -> str:
    base = os.getenv("AGENT_DATA_DIR", "/agent_data")
    return os.path.join(base, "rag_index.json")


def build_index(root: str, include_glob: List[str], exclude_glob: List[str]) -> Tuple[int, int]:
    root_path = Path(root)
    files: List[Path] = []
    for path in root_path.rglob("*"):
        if not path.is_file():
            continue
        rel = str(path.relative_to(root_path))
        if exclude_glob and any(fnmatch.fnmatch(rel, pattern) for pattern in exclude_glob):
            continue
        if include_glob and not any(fnmatch.fnmatch(rel, pattern) for pattern in include_glob):
            continue
        files.append(path)

    entries: List[VectorEntry] = []
    for file_path in files:
        try:
            content = file_path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        chunks = chunk_text(str(file_path), content)
        vectors = embed([chunk.text for chunk in chunks])
        for chunk, vector in zip(chunks, vectors):
            entries.append(
                VectorEntry(
                    path=chunk.path,
                    start_line=chunk.start_line,
                    end_line=chunk.end_line,
                    language=chunk.language,
                    text=chunk.text,
                    vector=vector,
                )
            )
    store = VectorStore(_index_path())
    store.upsert(entries)
    return len(files), len(entries)
