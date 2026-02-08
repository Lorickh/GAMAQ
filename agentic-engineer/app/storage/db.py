import os
import sqlite3
from contextlib import contextmanager

DB_FILENAME = "agent.db"


def _db_path() -> str:
    base_dir = os.getenv("AGENT_DATA_DIR", "/agent_data")
    os.makedirs(base_dir, exist_ok=True)
    return os.path.join(base_dir, DB_FILENAME)


def init_db() -> None:
    conn = sqlite3.connect(_db_path())
    try:
        cursor = conn.cursor()
        cursor.execute(
            """
            CREATE TABLE IF NOT EXISTS tasks (
                id TEXT PRIMARY KEY,
                created_at TEXT,
                status TEXT,
                instruction TEXT,
                dod_command TEXT,
                workspace_path TEXT,
                max_iters INTEGER,
                timeout_sec INTEGER
            )
            """
        )
        cursor.execute(
            """
            CREATE TABLE IF NOT EXISTS runs (
                id TEXT PRIMARY KEY,
                task_id TEXT,
                iter INTEGER,
                started_at TEXT,
                ended_at TEXT,
                result TEXT,
                summary TEXT,
                FOREIGN KEY(task_id) REFERENCES tasks(id)
            )
            """
        )
        cursor.execute(
            """
            CREATE TABLE IF NOT EXISTS tool_calls (
                id TEXT PRIMARY KEY,
                run_id TEXT,
                tool_name TEXT,
                input_json TEXT,
                output_json TEXT,
                started_at TEXT,
                ended_at TEXT,
                ok INTEGER,
                FOREIGN KEY(run_id) REFERENCES runs(id)
            )
            """
        )
        cursor.execute(
            """
            CREATE TABLE IF NOT EXISTS artifacts (
                id TEXT PRIMARY KEY,
                task_id TEXT,
                type TEXT,
                path TEXT,
                created_at TEXT,
                FOREIGN KEY(task_id) REFERENCES tasks(id)
            )
            """
        )
        conn.commit()
    finally:
        conn.close()


@contextmanager
def get_connection():
    conn = sqlite3.connect(_db_path())
    conn.row_factory = sqlite3.Row
    try:
        yield conn
    finally:
        conn.close()
