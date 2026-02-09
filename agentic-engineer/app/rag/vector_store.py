import json
import math
import os
from dataclasses import asdict, dataclass
from typing import List


@dataclass
class VectorEntry:
    path: str
    start_line: int
    end_line: int
    language: str
    text: str
    vector: List[float]


class VectorStore:
    def __init__(self, path: str):
        self.path = path
        self.entries: List[VectorEntry] = []

    def load(self) -> None:
        if not os.path.exists(self.path):
            return
        with open(self.path, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
        self.entries = [VectorEntry(**item) for item in raw]

    def save(self) -> None:
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as handle:
            json.dump([asdict(entry) for entry in self.entries], handle, ensure_ascii=False)

    def upsert(self, entries: List[VectorEntry]) -> None:
        self.entries = entries
        self.save()

    def query(self, vector: List[float], top_k: int) -> List[tuple[float, VectorEntry]]:
        scored = []
        for entry in self.entries:
            score = self._cosine(vector, entry.vector)
            scored.append((score, entry))
        scored.sort(key=lambda item: item[0], reverse=True)
        return scored[:top_k]

    @staticmethod
    def _cosine(a: List[float], b: List[float]) -> float:
        return sum(x * y for x, y in zip(a, b)) / ((math.sqrt(sum(x * x for x in a)) or 1.0) * (math.sqrt(sum(y * y for y in b)) or 1.0))
