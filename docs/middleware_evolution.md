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

## 2026-10-02：建立 completion 所有权状态机

基线：`445c24856ebbdb7371ec46918e8d68d1bef50fa9`，`codex/urmacpu`。
同步远端并检查文件树，未发现新的 `AGENTS.md`。

**问题与决定**：基线收到任意 CR 后只检查 `status`。它不核对 `user_ctx`，
也不核对 `completion_len`；错序、陈旧或错误归属的完成可能替当前 WR “结账”。
固定一百万次 poll 也不是时间单位，超时后仍普通销毁 context，无法证明在途 WR
已经 drain。先建立可测试的完成所有权，再开放多 outstanding。

**实现**：

- 新增 SDK-independent `CompletionTracker`：post 前登记请求，拒绝零/重复 ID；
  按 `user_ctx` 精确退休，验证状态与长度，支持未来窗口的乱序完成。
- 单 WR post 失败会撤销登记；成功 post 后的 poll 错误、未知 CR 或墙钟超时会
  quarantine context。参数矩阵 fail-fast，失败路径不发送成功控制命令。
- 用默认 5 秒的 `--completion-timeout-ms` 替换固定 poll 次数，范围限制为
  1 ms–1 h；正常路径仍 busy-poll，每 256 次空 poll 检查一次截止时间。
- quarantine 不执行普通销毁链，资源保留到失败进程退出。它避免在未证明 drain
  时主动释放被设备引用的缓冲，但不是常驻中间件的恢复机制。
- 修复官方头文件语法检查发现的 const 不匹配：复制 segment 描述后传给
  `urma_import_seg`，保留控制面原始 export 不变。

**验证证据**（Linux x86_64，GCC 13.3.0，CMake 3.31.6）：

```bash
cmake -S examples -B /tmp/gamaq-tests -DBUILD_URMA_BENCH=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gamaq-tests -j2
ctest --test-dir /tmp/gamaq-tests --output-on-failure
# 2/2 tests passed

g++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iexamples \
  examples/tests/completion_tracker_test.cpp -o /tmp/completion_tracker_test
ASAN_OPTIONS=detect_leaks=0 /tmp/completion_tracker_test
# completion_tracker: 147 checks passed

g++ -std=c++17 -Wall -Wextra -Werror -pthread \
  -isystem /tmp/umdk/src/urma/lib/urma/core/include -Iexamples \
  -fsyntax-only examples/urma_cpu_to_npu_bench.cpp
# passed against openEuler UMDK mirror e720dbead0b6 fetched 2026-10-02
```

Completion 测试覆盖非法/重复提交、post 撤销、未知和重复 CR、状态错误、长度错误、
64 项窗口逆序完成。加上原布局测试，本地两套测试共执行 11,641 次断言。
官方镜像头文件编译只是 API 形状检查，不等于用户目标 CANN/URMA 版本兼容。

**未验证**：目标 `/usr/include/ub/umdk/urma` 与 `/usr/lib64/liburma.so` 编译链接、
设备 completion_len 行为、quarantine 后 provider/kernel 的退出回收、NPU 控制面 EOF
处理以及任何性能指标均未实测。本轮不宣称性能收益。

**下一步**：在用户目标 SDK/设备上先验证 W=1 的 user_ctx、completion_len 与错误
CR；同时核实 Jetty suspend/flush/drain API。随后把同一 tracker 接到 W=2–64 的
有界窗口，逐批 post、批量 poll，并测量有效带宽、CPU 开销和尾延迟。

## 2026-10-03：有界滑动窗口与独立数据槽

基线：`7a999d981b5e0a64c96d56ec0adebc9483037978`，`codex/urmacpu`。

**场景假设**：KV cache prefetch/offload 会连续搬运多个离散块。W=1 把每次 post
和 completion 串行化，无法重叠设备/链路延迟；但只增加在途数而复用同一源缓冲会
破坏源缓冲可复用契约。最高优先级增量是先建立 W=1–64 的有界窗口和独立数据槽，
保持 W=1 对照，再由实机决定收益与最优窗口。

**架构决定与实现**：

- `CompletionTracker` 同时管理请求归属和固定容量 source slot；窗口满则背压，
  乱序 CR 只释放匹配 `user_ctx` 的槽，post 失败仅撤销未提交请求。
- benchmark 新增 `--windows`。每线程注册 `window * payload` 的 Host buffer，NPU
  目标布局扩展为 `threads * window * payload`，避免并发写共享目的地址。
- 热路径逐 WR post、最多批量 poll 16 条 CR，完成后立即补满窗口。故障后停止新
  post；其他 worker drain 已提交请求，出错 worker 的未知在途资源继续进入
  quarantine。尚未实现 WR 链式批量 post。
- JFS depth=`window`，JFC depth=`window+1`，JFR depth=1；能力不足显式拒绝。
  CSV 增加 window，并把易误解的 latency 列改名为 completion interval。

**验证证据**（Linux x86_64，GCC 13.3.0）：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
  examples/tests/completion_tracker_test.cpp -o /tmp/completion && /tmp/completion
# completion_tracker: 36134 checks passed

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
  examples/tests/transfer_layout_test.cpp -o /tmp/layout && /tmp/layout
# transfer_layout: 19565 checks passed

# 两个测试同样以 ASan/UBSan 运行通过（ASAN_OPTIONS=detect_leaks=0）
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
  -isystem /tmp/umdk-day3/src/urma/lib/urma/core/include -Iexamples \
  -fsyntax-only examples/urma_cpu_to_npu_bench.cpp
# passed against openEuler UMDK mirror e720dbead0b6 fetched 2026-10-03
```

两套测试合计 55,699 次断言；其中 reference scheduler 对七档窗口各执行 1,024 次
非 FIFO 完成。这里的断言数不是实机测试数。当前环境没有 CMake，因而改用等价的
直接编译/运行；sanitizer 未启用 leak 检查。

**未验证与下一步**：没有目标 CANN 9.1、NPU/UB 设备，未验证真实 SDK 编译链接、
completion 行为、数据一致性、吞吐或 CPU 收益。下一轮优先补充可注入 post/poll
部分失败的 backend seam 与窗口调度测试，随后在实机采集 W=1–64、512–4096 B、
线程 1–32 的 Gb/s、Mops/s、CPU core-seconds/GB 和尾延迟，依据数据决定是否做
链式 post、自适应窗口或公平调度。
