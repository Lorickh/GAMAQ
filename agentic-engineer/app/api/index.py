from fastapi import APIRouter

from app.core.schemas import IndexQuery, IndexRebuild
from app.tools.rag_tools import rag_query, rag_rebuild

router = APIRouter()


@router.post("/index/rebuild")
async def rebuild_index(payload: IndexRebuild):
    root = payload.path or "/workspace"
    return rag_rebuild(root, payload.include_glob, payload.exclude_glob)


@router.post("/index/query")
async def query_index(payload: IndexQuery):
    return rag_query(payload.query, payload.top_k)
