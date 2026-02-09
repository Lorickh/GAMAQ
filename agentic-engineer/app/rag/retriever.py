import os
from typing import List

from app.rag.embedder import embed
from app.rag.vector_store import VectorStore


def _index_path() -> str:
    base = os.getenv("AGENT_DATA_DIR", "/agent_data")
    return os.path.join(base, "rag_index.json")


def query_index(query: str, top_k: int) -> List[dict]:
    store = VectorStore(_index_path())
    store.load()
    vector = embed([query])[0]
    hits = []
    for score, entry in store.query(vector, top_k):
        hits.append(
            {
                "path": entry.path,
                "start_line": entry.start_line,
                "end_line": entry.end_line,
                "score": score,
                "snippet": entry.text,
            }
        )
    return hits
