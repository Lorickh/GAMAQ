from fastapi import FastAPI

from app.api import index, tasks
from app.storage.db import init_db

app = FastAPI(title="Agentic Engineer MVP")


@app.on_event("startup")
async def on_startup() -> None:
    init_db()


app.include_router(tasks.router)
app.include_router(index.router)
