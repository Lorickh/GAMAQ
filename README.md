# Agentic Engineer MVP

这是一个在容器中运行的“工程师型 Agent”最小实现，能够对挂载到 `/workspace` 的目标项目进行读取/修改、RAG 检索、执行命令与测试，并在多轮迭代中尝试满足 Definition of Done (DoD)。



## 功能概览

- **任务系统**：通过 HTTP API 提交任务（指令 + DoD），Agent 自动执行并产出进度事件与产物。
- **工作区读写**：在容器内对 `/workspace` 进行读取、修改，并输出补丁。
- **命令执行**：执行构建/测试命令，捕获 stdout/stderr/exit code。
- **RAG 检索**：对目标项目建立索引并提供检索接口。
- **多轮闭环**：失败后自动迭代（DoD 验证 → 证据提取 → 计划 → 执行）。
- **产物输出**：记录 patch、report、verify.log 并落库审计。

## 目录结构

```
agentic-engineer/
  SPEC.md
  docker/
    Dockerfile.agent
    docker-compose.yml
  app/
    main.py
    api/
      tasks.py
      index.py
      events.py
    core/
      orchestrator.py
      agent.py
      schemas.py
    tools/
      registry.py
      fs_tools.py
      cmd_tools.py
      git_tools.py
      rag_tools.py
      policy.py
    rag/
      indexer.py
      chunker.py
      vector_store.py
      embedder.py
      retriever.py
    llm/
      gateway.py
      prompts.py
    storage/
      db.py
      models.py
    telemetry/
      logger.py
      metrics.py
  tests/
  scripts/
    agentctl.py
```

## 快速开始

### 1) 本地运行（Python）

```bash
cd agentic-engineer
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

### 2) Docker 运行

```bash
cd agentic-engineer/docker
docker-compose up --build
```

默认会将宿主机的 `./target_project` 挂载到容器内 `/workspace`，Agent 数据写入 `./agent_data`。

## 核心 API

- `POST /tasks`：创建任务
- `GET /tasks/{id}`：查询任务状态
- `GET /tasks/{id}/events`：SSE 事件流
- `GET /tasks/{id}/artifacts`：产物列表
- `POST /index/rebuild`：重建 RAG 索引
- `POST /index/query`：检索

### 示例：创建任务

```bash
curl -X POST http://localhost:8000/tasks \
  -H 'Content-Type: application/json' \
  -d '{
    "instruction": "修复单测失败",
    "dod_command": "pytest -q",
    "workspace_path": "/workspace"
  }'
```

## CLI

`agentctl` 提供最小 CLI：

```bash
python scripts/agentctl.py task "修复单测失败" --dod "pytest -q"
python scripts/agentctl.py logs <task_id>
python scripts/agentctl.py patch <task_id> > fix.patch
```

## 产物说明

任务结束后会在 `${AGENT_DATA_DIR}/artifacts/<task_id>/` 下生成：

- `final.patch`：最终 diff
- `report.md`：变更摘要与验证结果
- `verify.log`：DoD 命令的输出日志

## 关键实现说明

- **任务编排**：`app/core/orchestrator.py` 中每轮都会先跑 DoD，失败后调用 planner 生成计划并执行。
- **工具层**：`app/tools/*` 封装读写文件、执行命令、git diff、RAG 操作。
- **RAG**：`app/rag/*` 提供简单的文本分块、向量化和本地存储检索。
- **审计**：`app/storage/*` 使用 SQLite 保存任务、运行、工具调用和产物记录。

## 测试

```bash
cd agentic-engineer
pytest -q
```

> 注意：测试需要安装 `requirements.txt` 中的依赖。

## 环境变量

- `WORKSPACE_ROOT`：默认 `/workspace`
- `AGENT_DATA_DIR`：默认 `/agent_data`
- `LLM_API_KEY` / `LLM_MODEL`：LLM 网关配置（MVP 可为空）
- `LLM_BASE_URL`：LLM 网关基础地址（默认 `http://127.0.0.1:8000`）
- `LLM_ENDPOINT`：LLM 网关路径（默认 `/v1/chat/completions`）
- `LLM_TIMEOUT_SEC`：LLM 请求超时时间（秒，默认 `30`）
- `LLM_SYSTEM_PROMPT`：系统提示语（默认要求输出 JSON 计划）
- `LLM_EXTRA_HEADERS`：JSON 字符串，附加请求头（例如自定义鉴权）
- `LLM_DEBUG_LOG`：启用 LLM 请求/响应日志（写入 `${AGENT_DATA_DIR}/events.log`）
- `LLM_LOG_MAX_CHARS`：LLM 日志单条最大长度（默认 `2000`）

## LLM 网关日志查看

开启 `LLM_DEBUG_LOG=1` 后，LLM 请求与响应会写入 `${AGENT_DATA_DIR}/events.log`，可用以下方式查看：

```bash
tail -f /agent_data/events.log
```

## License

此仓库为 MVP 代码基线，后续可根据实际需求补充 License。
