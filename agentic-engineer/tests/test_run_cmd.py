from app.tools.cmd_tools import run_cmd


def test_run_cmd_echo(tmp_path):
    result = run_cmd("echo hello", cwd=str(tmp_path), timeout_sec=5)
    assert result["exit_code"] == 0
    assert "hello" in result["stdout"]
