from typing import Dict, List

from app.rag.indexer import build_index
from app.rag.retriever import query_index


def rag_rebuild(root: str, include_glob: List[str], exclude_glob: List[str]) -> Dict:
    indexed_files, chunks = build_index(root, include_glob, exclude_glob)
    return {"indexed_files": indexed_files, "chunks": chunks}


def rag_query(query: str, top_k: int) -> Dict:
    hits = query_index(query, top_k)
    return {"hits": hits}
