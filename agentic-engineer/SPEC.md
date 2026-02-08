# Agentic AI 本地工程师代理平台（MVP）工程实现规格书

版本：v0.1 (MVP)
目标读者：Codex / 工程实现者
语言：中文
运行形态：CPU 常驻服务 + 任务执行器（容器内）
核心能力：读取/修改目标项目代码 + RAG 检索 + 多轮执行直到满足 DoD（可验证成功条件）

> 该文件为 MVP 规格的摘要版本，详见任务说明中的完整规格。

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
    test_patch_engine.py
    test_run_cmd.py
    test_rag_basic.py
    test_end2end_sample_repo.py
  scripts/
    agentctl.py
```
