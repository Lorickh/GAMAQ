import json
import os
from datetime import datetime
from typing import Any, Dict


def log_event(event_type: str, payload: Dict[str, Any]) -> None:
    log_dir = os.getenv("AGENT_DATA_DIR", "/agent_data")
    os.makedirs(log_dir, exist_ok=True)
    path = os.path.join(log_dir, "events.log")
    record = {
        "timestamp": datetime.utcnow().isoformat(),
        "type": event_type,
        "payload": payload,
    }
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, ensure_ascii=False) + "\n")
