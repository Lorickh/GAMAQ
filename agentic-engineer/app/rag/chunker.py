from dataclasses import dataclass
from typing import List


@dataclass
class Chunk:
    path: str
    start_line: int
    end_line: int
    text: str
    language: str


def chunk_text(path: str, text: str, max_lines: int = 200) -> List[Chunk]:
    lines = text.splitlines()
    chunks = []
    for i in range(0, len(lines), max_lines):
        start = i + 1
        end = min(i + max_lines, len(lines))
        chunk_text = "\n".join(lines[i:end])
        language = path.rsplit(".", 1)[-1] if "." in path else "text"
        chunks.append(Chunk(path=path, start_line=start, end_line=end, text=chunk_text, language=language))
    return chunks
