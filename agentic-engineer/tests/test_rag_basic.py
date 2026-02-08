from pathlib import Path

from app.tools.rag_tools import rag_query, rag_rebuild


def test_rag_basic(tmp_path, monkeypatch):
    monkeypatch.setenv("AGENT_DATA_DIR", str(tmp_path / "agent_data"))
    repo = tmp_path / "repo"
    repo.mkdir()
    (repo / "sample.py").write_text("def hello():\n    return 'hi'\n", encoding="utf-8")

    result = rag_rebuild(str(repo), ["**/*.py"], ["**/.git/**"])
    assert result["indexed_files"] == 1
    hits = rag_query("hello", 3)["hits"]
    assert hits
    assert any("sample.py" in hit["path"] for hit in hits)
