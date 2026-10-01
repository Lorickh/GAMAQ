# CPU/NPU 数据搬运中间件：演进记录

## 定位与当前边界

候选首个应用是推理 KV 块批量搬运：需要可靠地描述每个块的目的范围、
控制在途资源并给出清楚的完成契约。现阶段只有 CPU 发起的 URMA WRITE
benchmark，不是可投入业务的中间件，尚未实现 KV Get/Put 或推理引擎适配。
URMA 单边通信不等于 CPU 可 load/store 的透明共享内存。

渐进路线：可信微基准 → 有界异步传输内核 → 内存/队列复用 → KV batch
接口 → 一个推理引擎端到端验证。CPU indexer 卸载和分离式推理 activation
搬运作为后续候选，待本条数据路径形成可测收益后再比较投入价值。

## 2026-10-01：先建立目的范围隔离

基线：`a8b020e41267cf3dc22356fd9a1f235f78b07890`，`codex/urmacpu`。
读取完整分支文件树，未发现适用的 `AGENTS.md`。

**问题与决定**：旧实现遇到 `id * size + size > hbm_len` 时回退 offset 0，
让多个 worker 覆盖同一范围；也没有用 `remote_seg.len` 限制访问。
同样的填充值掩盖了覆盖，难以作为离散 KV 块搬运的正确性基线。
本轮先移除这种回退，再扩展 outstanding；不通过增加并发掩盖地址问题。

**实现**：

- 抽出无 SDK 依赖的 `examples/transfer_layout.h`，检查总长度溢出、HBM
  长度、segment 长度、UBVA 最后一字节溢出和 worker 索引。
- benchmark 在创建 URMA 资源及发出任何 WR 前验证参数矩阵最大 footprint；
  case 内再次检查，每线程地址在计时区外计算。容量不足明确失败，不输出
  部分矩阵数据，不再静默改变测试的目的地址布局。
- 增加 CTest 与 `BUILD_URMA_BENCH=OFF`，使无 SDK 环境能执行同一布局逻辑。
  原 benchmark 默认构建开关、有效配置地址和 CSV 格式保留。

**验证证据**（Linux x86_64，GCC 13.3.0，CMake 3.31.6）：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
  examples/tests/transfer_layout_test.cpp -o /tmp/transfer_layout_test
/tmp/transfer_layout_test
# transfer_layout: 11494 checks passed

cmake -S examples -B /tmp/gamaq-cpu-tests -DBUILD_URMA_BENCH=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gamaq-cpu-tests -j2
ctest --test-dir /tmp/gamaq-cpu-tests --output-on-failure
# 1/1 test passed; checks remain active with NDEBUG

g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iexamples examples/tests/transfer_layout_test.cpp -o /tmp/transfer_layout_sanitized
ASAN_OPTIONS=detect_leaks=0 /tmp/transfer_layout_sanitized
# transfer_layout: 11494 checks passed
git diff --check
```

最初 sanitizer 运行被 LeakSanitizer 读取 `/proc` 的环境限制阻止；关闭
泄漏检测后 ASan/UBSan 运行通过。因此没有获得泄漏检查结论。
断言覆盖 8 KiB 回退回归、注册范围短于 HBM、零参数、乘法和地址上界、
精确容纳、非法 worker，以及小域容量扫描与 1–32 线程/512–4096 B 参数矩阵。
这些是 11,494 次断言，不是 11,494 个实机测试。

**未验证**：环境无匹配 URMA/CANN SDK、NPU/UB 设备；完整 benchmark 的目标 SDK
编译、控制面 EOF 处理、HBM 内容校验与性能均未执行。新增 segment `len` 字段已
核对公开 API Guide（链接见基准说明），不据此宣称目标驱动 ABI 兼容。
本轮证明软件布局边界，不宣称吞吐、CPU 或尾延迟改善。

## 下一步：优先级与验收条件

1. **完成与资源生命期**：核对匹配 SDK 的初始化、EID 选择、completion
   `user_ctx`、错误/超时及 drain/destroy 契约。当前固定 poll 次数超时可能仍有
   WR 在途，原清理逻辑不能据此视为安全；先明确如何停止并确认回收，
   再实施窗口化。验收包含错序/错误完成、超时和部分失败的故障注入。
2. **有界窗口**：每线程 W=1/2/4/8/16/32/64，逐项 completion 核对；
   必须证明在途不超过上限、缓冲区不会过早复用、失败不丢失归属。
   首先形成可测试的状态机，再绑定已核实 URMA API，保留 W=1 对照。
3. **业务数据真实性**：不同块/线程采用可识别 pattern，逐块回读与错误计数；
   现有 NPU 端打印不等于自动完整校验。没有实机时保留明确待测脚本/步骤。
4. **竞争力实验**：同环境比较 W=1 与有界窗口/批量方案的有效吞吐、CPU
   开销、p50/p99 和资源占用，保留原始数据及 SDK/驱动/NUMA 配置。
   用户观察到的 2048/4096 在途差异作为待检验假设，不外推到所有系统。

长期不变式：提交、源缓冲可复用、传输完成、对端可消费是不同事件；
任何布局预检都不替代实际设备权限、完成语义或 HBM 映射验证。
