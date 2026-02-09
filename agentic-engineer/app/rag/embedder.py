import math
import re
from typing import List

VECTOR_SIZE = 256
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]+")


def _tokenize(text: str) -> List[str]:
    return [token.lower() for token in TOKEN_RE.findall(text)]


def embed(texts: List[str]) -> List[List[float]]:
    vectors = []
    for text in texts:
        counts = [0.0] * VECTOR_SIZE
        for token in _tokenize(text):
            idx = hash(token) % VECTOR_SIZE
            counts[idx] += 1.0
        norm = math.sqrt(sum(value * value for value in counts)) or 1.0
        vectors.append([value / norm for value in counts])
    return vectors
