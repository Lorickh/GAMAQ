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

## 2026-10-04：抽出 backend seam，证明部分失败后的 drain

基线：`f131d1afffb3334fbdec718b0b04f4052387c5f9`，`codex/urmacpu`。

**场景假设**：KV 批量 prefetch/put 的窗口中，某次 post 失败不代表此前已接受的 WR
也失败。若立即销毁会破坏所有权，若一律 quarantine 又无法成为常驻中间件。最高
优先级是把窗口状态机与 URMA 调用解耦，明确“不重试当前请求、停止新 post、drain
已接受请求”的边界，并在无硬件环境注入部分失败。

**架构决定与实现**：

- 新增模板化 `window_scheduler.h`，公共层拥有窗口/槽位、request ID、背压、stop、
  completion 校验、超时和 drain；backend 只有 `post` 与 `poll`，无虚调用。
- `UrmaWriteBackend` 负责地址槽到 SGE/WR 的映射及 CR 标准化；benchmark 复用同一
  scheduler，不再内嵌调度策略。
- post 失败仅撤销当前未接受请求，保留原错误并 drain 之前的请求；drain 再失败时
  同时记录 secondary error。已知错误 CR 可精确退休，未知 CR/poll 错误/超时继续
  quarantine；不假设写操作可幂等重试。
- 新增 deterministic mock backend，能注入 post、poll、completion、长度、未知 ID、
  非法 count、超时、跨 worker cancel 和 request-ID exhaustion。

**验证证据**（Linux x86_64，GCC 13.3.0）：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
  examples/tests/window_scheduler_test.cpp -o /tmp/window && /tmp/window
# window_scheduler: 86 checks passed; W=1..64 共 7168 次成功完成

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
  examples/tests/completion_tracker_test.cpp -o /tmp/completion && /tmp/completion
# completion_tracker: 36134 checks passed

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
  examples/tests/transfer_layout_test.cpp -o /tmp/layout && /tmp/layout
# transfer_layout: 19565 checks passed

# 三套测试同样以 ASan/UBSan 运行；LeakSanitizer 关闭
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
  -isystem /tmp/umdk-day3/src/urma/lib/urma/core/include -Iexamples \
  -fsyntax-only examples/urma_cpu_to_npu_bench.cpp
# passed against openEuler UMDK mirror e720dbead0b6 fetched 2026-10-04
```

三套测试合计 55,785 次显式断言；操作数与断言数不是实机测试数。当前环境没有目标
CANN 9.1、NPU/UB，未验证链接、设备 quiescence、HBM 可见性、吞吐或 CPU 开销。

**下一步**：当前 scheduler 仍面向单段、固定大小 WRITE。下一轮优先定义最小异步
搬运请求/完成 API 和多块 KV batch adapter，用 reference backend 验证一批请求的
逐项成功/失败结果、提交完成与对端可消费边界；实机可用后再做 W/线程/payload 扫描。

## 2026-10-05：首个 KV batch put/offload API

基线：`bc10e2d5fd94b0cf332fb432d29a8c2b2e419168`，`codex/urmacpu`。

**场景假设**：KV offload/prefetch 的最小有用接口不是固定大小循环，而是一批离散
block；业务需要逐块知道源何时可复用、传输是否成功，且不能把 transport completion
虚推为 NPU kernel 已可消费。选择 CPU→NPU batch put 作为首个应用切口，暂不扩展
get、key index、SSD 或推理引擎集成。

候选比较：activation/中间结果通常先表现为少量连续大 tensor，现有 benchmark 已能
覆盖其基础 WRITE 路径；CPU index/offload 还依赖算子运行时与任务队列。KV batch 的
离散块、部分失败和预取窗口与当前 scheduler 能力直接相交，也能在无 NPU 时验证业务
契约，因此优先级最高。这个判断只说明工程切入点，不声称 KV 已有实机收益。

**架构决定与实现**：

- `TransferWork/TransferRequest` 新增 application tag、源/目标 region 相对 offset 和
  operation index；`CompletionTracker` 保存调用者 context，使乱序 CR 能回到原项。
- 新增 `kv_put_batch.h`：输入多个 `{block_id, source_offset, destination_offset,
  bytes}`，输出逐项生命周期与错误证据；全 batch 验证通过后才允许第一个 post。
- 拒绝零长度、地址溢出、注册区越界、重复 block ID 和重叠目标；允许共享只读源。
- 明确五个 item 状态及 `source_reusable/transfer_complete`；
  `peer_consumable_proven` 始终 false，等待独立可见性/通知协议。
- 保留非幂等失败语义：不自动重试；post 失败后的项保持 `not_submitted`，poll 失败
  的已提交项保持 backend ownership。

**验证证据**（Linux x86_64，GCC 13.3.0）：

```bash
for test in completion_tracker transfer_layout window_scheduler kv_put_batch; do
  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
    examples/tests/${test}_test.cpp -o /tmp/${test} && /tmp/${test}
done
# completion_tracker: 36134 checks passed
# transfer_layout: 19565 checks passed
# window_scheduler: 86 checks passed
# kv_put_batch: 51 checks passed

# 四套测试同样以 ASan/UBSan 运行；LeakSanitizer 关闭
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
  -isystem /tmp/umdk-day5/src/urma/lib/urma/core/include -Iexamples \
  -fsyntax-only examples/urma_cpu_to_npu_bench.cpp
# passed against openEuler UMDK mirror e720dbead0b6 fetched 2026-10-05
```

四套测试合计 55,836 次显式断言。KV reference backend 复制并逐字节核对 4 个离散
block，另覆盖 7 类输入拒绝、共享源、部分提交及 completion/poll 故障。这不是实机
测试；目标 CANN 9.1 链接、URMA 对离散块的实际提交方式、HBM 可见性和性能未验证。

**下一步**：当前 KV adapter 已形成业务语义，但尚无生产 `UrmaKvPutBackend` 的注册
region/session 生命周期。下一轮优先抽出可复用 registered-region/session 对象，把
adapter 接到 URMA 地址与 segment，同时仍以 W=1 保留基线；对端消费通知只有在核实
目标 SDK 支持后才实现。

## 2026-10-06：把 KV offset 绑定到真实 URMA WR

基线：`e541eaddf7c89db71d1981561a489b0e40dc26be`，`codex/urmacpu`。

**场景假设**：KV adapter 如果只在 reference memory backend 上运行，仍无法证明离散
offset 会正确进入 CPU 本地 segment 与 NPU 导入 segment。最高优先级是建立窄且可复用
的 registered-region binding，把 batch 请求映射到真实 `urma_sge_t/urma_jfs_wr_t`；
session ownership 与设备可见性继续保持独立，不借地址映射假装已经解决。

**架构决定与实现**：

- 新增 transport-independent `RegisteredRegionView` 与 range resolver；使用相对 offset，
  检查零长度、注册容量、起始地址及最后一字节溢出，源/目标错误分别保留。
- 新增非 owning `UrmaRegionBinding` 和 `UrmaKvPutBackend`；按请求离散 offset 构造本地/
  远端 SGE、signaled WRITE，并标准化 completion。backend 不拥有或隐式释放任何资源。
- 上层 KV batch 和 transport post 边界执行两层验证；非法 binding/range 在 provider
  调用前失败。provider post 状态原样交给 scheduler，不增加无依据重试。
- benchmark 包含新 backend 并通过官方头文件编译；原固定槽 benchmark 路径和输出不变。

**验证证据**（Linux x86_64，GCC 13.3.0）：

```bash
for test in completion_tracker transfer_layout window_scheduler kv_put_batch registered_region; do
  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
    examples/tests/${test}_test.cpp -o /tmp/${test} && /tmp/${test}
done

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -isystem /tmp/umdk-day6/src/urma/lib/urma/core/include -Iexamples \
  examples/tests/urma_kv_backend_test.cpp -o /tmp/urma_kv_backend_test
/tmp/urma_kv_backend_test
# urma_kv_backend: 15 checks passed

g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
  -isystem /tmp/umdk-day6/src/urma/lib/urma/core/include -Iexamples \
  -fsyntax-only examples/urma_cpu_to_npu_bench.cpp
```

五套无 SDK 测试与一套官方类型 stub-provider 测试合计 80,665 次显式断言；其中
registered-region 小域穷举 24,814 次，WR 测试直接捕获并核对两端 SGE、segment、
Jetty、`user_ctx` 和成功/失败 CR。全部测试同时以 ASan/UBSan 运行通过；当前环境没有
CMake，使用等价直接编译。头文件版本为 openEuler UMDK `e720dbead0b6`。

**未验证与下一步**：没有目标 CANN 9.1、NPU/UB，未验证链接、权限、HBM 可见性、
吞吐或 CPU 开销。当前 binding 刻意不拥有资源，最大瓶颈转为可复用 session 生命周期：
下一轮抽出 context/JFC/JFR/Jetty/segment 的有序创建与销毁，只有已知 drain 才释放，
未知在途继续 quarantine；随后再让实际 KV entry point 使用该 session。

## 2026-10-07：可复用、失败封闭的 URMA session

基线：`5b5794ba6e1f704cdba37c008bf446500ce90dab`，`codex/urmacpu`。

**场景假设**：KV offload/prefetch 是常驻数据面，若每批重新创建 context、队列、Jetty
和注册内存，建链与注册开销会进入业务路径；但把资源做成长生命周期后，只有证明全部
WR 已退休才能释放。最高优先级是把实际 benchmark 的 provider 资源抽成可复用 session，
并让所有部分建立、drain 和 teardown 失败都 fail-closed，而不是先做未经实机证明的
批量 post 优化。

**架构决定与实现**：

- 新增不可复制/移动的 `UrmaSession`，拥有一 worker 的 context、JFC/JFR/Jetty、本地
  buffer/segment、远端导入 segment/Jetty、completion tracker 和 request ID；同时导出
  KV backend 可直接使用的 registered-region bindings。
- open 查询并保留设备队列能力，逐层建立依赖；任一点失败逆序 rollback，同时保留
  primary error 与 rollback error。窗口溢出及 `JFC=window+1`、`JFS=window`、`JFR>=1`
  能力不足在创建数据面对象前拒绝。
- close 区分 `closed/already_closed/in_flight/quarantined/provider_error`。已知在途可在
  drain 后重试；未知在途永久 quarantine。任一 provider teardown 失败立即停止级联
  销毁且不自动重试，因为没有幂等保证。
- benchmark 删除手工 `ThreadContext` 创建/释放，改为按 worker 复用 session，并把
  open 能力错误、rollback 失败和 close 失败输出为可定位证据。

**验证证据**（Linux x86_64，GCC 13.3.0）：

```bash
for test in completion_tracker transfer_layout window_scheduler kv_put_batch registered_region; do
  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
    examples/tests/${test}_test.cpp -o /tmp/${test} && /tmp/${test}
done

for test in urma_kv_backend urma_session; do
  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -isystem /tmp/umdk-day7/src/urma/lib/urma/core/include -Iexamples \
    examples/tests/${test}_test.cpp -o /tmp/${test} && /tmp/${test}
done

g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
  -isystem /tmp/umdk-day7/src/urma/lib/urma/core/include -Iexamples \
  -fsyntax-only examples/urma_cpu_to_npu_bench.cpp
```

七套测试合计 80,713 次显式断言；新增 session 测试 48 项，覆盖完整依赖顺序、八个
open 失败点、能力拒绝、已知 drain 后重试、未知在途隔离、teardown 失败停止级联，
以及 open 主失败和 rollback 失败并存。全部测试另以 ASan/UBSan 运行。官方类型来自
openEuler UMDK `e720dbead0b6dba8742028e162afed5dfd58df95`。当前环境没有 CMake，
因此使用等价的直接编译和运行。

**未验证与下一步**：没有目标 CANN 9.1、NPU/UB，未验证真实链接、设备权限、provider
quiescence/flush、HBM 可见性、资源回收或性能；上述断言数不是设备实验数。当前最大
瓶颈是 KV API 尚未拥有明确的 session-facing entry point。下一轮优先把 `put_batch`
绑定到 session 并定义显式 drain/close 调用路径；同时核实目标 SDK 是否提供 suspend/
flush，只有获得语义证据后才实现在线故障恢复或对端可消费通知。

## 2026-10-08：把 KV put 接入长生命周期 session

基线：`381ae46c76fae3b4d6083f5200feea0c7e35cc84`，`codex/urmacpu`。

**场景假设**：仅有 session 和非 owning KV backend 仍要求业务手工拼接 handle、tracker
和错误状态，容易把“全部 WR 已 drain、资源可销毁”误当成“Jetty 可继续使用”。常驻
KV offload 数据面需要一个实际 session-facing 入口，并在每批结束后区分可复用、可关闭
但必须退役、所有权未知必须隔离三种状态。

**架构决定与实现**：

- 新增 `urma_kv_session.h`，以预注册 `UrmaSession::buffer()` 为源区域，直接组合离散
  KV batch adapter、URMA WR backend、session handles、completion tracker 和单调 ID。
- 连续成功 batch 复用同一 context/队列/Jetty/segment；发现已有在途请求、未打开/
  已退役/已隔离 session 或非正 timeout 时，在 provider 调用前明确拒绝。
- post 或命名错误 completion 已 drain 时标记 retired：禁止无依据恢复，但仍允许正常
  close。poll/未知 completion/timeout 导致所有权不明时 quarantine：既不继续提交，
  也不执行 provider teardown。仅输入预检失败不污染健康 session。
- benchmark 的固定 WRITE 路径同步采用相同退役判断，避免已知 transport 错误后把
  session 错误视为可复用。

**验证证据**（Linux x86_64，GCC 13.3.0）：

```bash
for test in completion_tracker transfer_layout window_scheduler kv_put_batch registered_region; do
  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
    examples/tests/${test}_test.cpp -o /tmp/${test} && /tmp/${test}
done

for test in urma_kv_backend urma_session; do
  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -isystem /tmp/umdk-day8/src/urma/lib/urma/core/include -Iexamples \
    examples/tests/${test}_test.cpp -o /tmp/${test} && /tmp/${test}
done
```

七套测试合计 80,741 次显式断言；session/provider 测试由 48 增至 76 项。新增覆盖两批
离散 KV 的真实 WR 地址/segment 绑定、资源跨批复用、request ID 递增、输入和 timeout
预检、已有在途拒绝、drained failure 退役以及 unknown ownership 隔离。全部测试另以
ASan/UBSan 运行，benchmark 以 `-Werror` 通过同一头文件语法编译。2026-10-08 获取的
openEuler UMDK 版本为 `4eab3e4ad170b06bfe5d5c1014341e81edb9bf58`。

**未验证与下一步**：缺少目标 CANN 9.1、NPU/UB，未验证链接、真实 flush/quiescence、
HBM 可见性、资源回收和性能。当前最大瓶颈转为调用者如何低开销填充预注册 buffer，
以及传输完成后如何证明对端可消费。下一轮优先设计不复制的 staging/region 注册复用
边界和最小可见性通知接口；只有目标 SDK 证据支持时才实现 flush/suspend 在线恢复。
