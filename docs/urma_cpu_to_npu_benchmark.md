# CPU 下发 URMA 到 NPU HBM：代码分析与多线程基准

## 1. 原程序到底做了什么

原程序不是“把数据通过 Unix socket 发给 NPU”。Socket 只承担**控制面**：NPU
把 `urma_seg_t`、Jetty ID、长度等元数据交给 Host，并在 WRITE、填充、READ 三个
阶段之间做同步。真正的**数据面**由 Host CPU 调用 URMA API 下发：

1. NPU 侧用 ACL 选择设备并分配 HBM；HCCP/RA 创建 UB context、CQ、QP/Jetty，
   然后把 HBM 注册为可被 UB 访问的内存段。
2. Host 侧按 EID 找到 UB 设备，创建 context、JFC、JFR 和 Jetty，注册 Host DDR，
   再导入 NPU 的 segment 与 Jetty。
3. `URMA_OPC_WRITE` 的源 SGE 是 Host DDR，目的 SGE 是 NPU HBM 的 UBVA；
   `URMA_OPC_READ` 正好相反。`urma_post_jetty_send_wr` 是 CPU 的提交点，JFC
   completion 表示该 WR 的完成状态。
4. NPU 收到 `HOST_WRITE_DONE` 后，使用 `aclrtMemcpy(DEVICE_TO_HOST)` 回读 HBM，
   这是功能校验，不在 URMA 性能计时路径内。

对应关系可类比 RDMA verbs，但不要把名称机械等同：JFS 是发送侧工作队列，JFR 是
接收侧工作队列，JFC 是完成队列，Jetty 将发送/接收资源组合起来，segment 是已注册
的本地或已导入的远端内存。准确语义和限制始终以机器上匹配驱动版本的
`urma_api.h` 为准。

## 2. 原代码逐段说明

### 常量与数据结构

- `MEM_SIZE/MSG_SIZE=0x2000`：只分配和传输 8 KiB；这不是“包大小”。一次 WR 的
  payload 才是本基准中的 size。
- `CQ_DEPTH`、`JETTY_SIZE`、`RQ_DEPTH`：分别控制 completion、Jetty/JFR 与接收
  队列容量。原代码每次只允许一个 outstanding WR，巨大 CQ 不会提升性能。
- `ProgramOptions` 保存角色、EID 索引和填充值。EID 是 16 字节端点标识，不是
  IP 地址。
- `Context` 是 Host URMA 对象所有权集合；`NpuState` 是 NPU RA/HCCP 句柄集合；
  `NpuHbmExport` 是控制面的资源描述。
- `SOCKET_MAGIC` 和 `version` 只能发现明显的不匹配。直接发送含厂商结构体的
  native ABI 不具备跨版本、跨架构或跨字节序兼容性。

### 参数、打印、Socket

- `parse_u32/parse_int/parse_options` 解析参数，但原版在转换后没有完整检查数值是否
  超出目标整数类型。
- `hex_string_to_bytes` 将 32 个十六进制字符变为 16 字节 EID。
- `dump_bytes` 最多显示 1024 字节；`read_and_print_hbm` 先从设备复制到 Host，
  不能直接在 CPU 上解引用 HBM 指针。
- `send_all/recv_all` 处理 stream socket 的短读写和 `EINTR`，这一点是必要的。
- NPU 是 Unix socket server，Host 是 client；Host 最多等待约 60 秒。

### Host 资源路径

- `urma_get_device_by_eid` 选择 UB 设备；原 `get_eid_index` 总是采用列表第一个
  EID index，未验证它就是命令行指定的 EID。
- `urma_create_context` 后创建 JFC、JFR、Jetty。传输模式为 `URMA_TM_RM`，即
  reliable message/可靠传输语义。
- `memalign(4096, MSG_SIZE)` 创建页对齐 DDR，`urma_register_seg` 将其注册。
- `urma_import_seg(...URMA_SEG_NOMAP)` 导入远端 HBM segment；NOMAP 表明程序
  不要求把远端段映射成 CPU 可直接 load/store 的本地虚址。
- `urma_import_jetty` 建立目标 Jetty 句柄。之后 WR 的本地和远端 SGE 都必须引用
  对应的注册/导入 segment。
- `post_rw` 每提交一个 WR 就等待一个 completion，因此测到的是**同步往返延迟**，
  不是链路极限带宽。原轮询每次空轮询后 `usleep(10)`，会把调度器睡眠直接计入
  小包延迟。

### NPU 资源路径

- `aclrtSetDevice` 选择逻辑设备，`aclrtGetPhyDevIdByLogicDevId` 转为物理 ID。
- `rtOpenNetService`、`RaInit` 和 `RaCtxInit` 初始化 HCCP/UB 通路，并从 EID 列表
  中选择 NPU 端 EID。`ctx_eid_index` 与 `npu_eid_list_index` 是两个不同命名空间，
  默认硬编码为 1 和 9 很依赖部署。
- `RaCtxCqCreate` 创建完成资源；`RaCtxQpCreate` 创建 RM QP/Jetty。代码从
  `qp_info.key.value` 解释出 Jetty ID，这依赖当前 HCCP 头文件的 ABI。
- `aclrtMalloc` 分配 HBM；`RaCtxLmemRegister` 授予 READ/WRITE/ATOMIC 权限并导出
  key、token ID 和 target segment handle。
- `remote_seg.ubva.eid` 被补为 Jetty EID，再通过 socket 发给 Host。

### 生命周期和两个明显问题

资源必须按依赖关系逆序释放。原 NPU 清理只销毁 QP、注销内存和释放 HBM，未展示
CQ、RA context、Ra 反初始化和 ACL device reset；应按实际 SDK 所提供的对称 API
补齐。原 Host 在销毁 imported segment 前先删除本地 Jetty，通常可工作，但最稳妥的
规则仍是先停止流量，再销毁 imported target、Jetty、segment、JFR/JFC、context。

另外，原程序 `HOST_WRITE_DONE` 失败后仍继续执行并覆盖 `ret`，可能把第一次校验失败
隐藏掉；基准程序把任一 case 或控制命令失败都保留为最终失败。

## 3. 新基准的设计

`examples/urma_cpu_to_npu_bench.cpp` 是**纯 Host CPU 下发**的 WRITE 基准，兼容原
NPU server 的首包和控制命令：

- 默认 payload 为 512、1024、2048、4096 B；线程数为 1、2、4、8。
- 每个线程独占 context、JFC、JFR、Jetty、registered DDR 和 imported handle，
  避免在未知线程安全保证下共享队列，也能观察多队列扩展性。
- `--windows` 控制每线程 outstanding WR 上限，支持 1/2/4/8/16/32/64，默认 1
  以保留原同步基线。实现逐 WR post、最多 16 条 CR 一次 poll；本版还没有把多个
  WR 链成一次 post，因此可分离验证“并发在途”而不是混入 doorbell batching。
- 每个在途 WR 独占一块已注册 Host 源槽位和 NPU HBM 目标槽位。完成按 `user_ctx`
  乱序退休，只有对应完成成功后才释放该槽，避免窗口下过早复用源缓冲。
- 预热不计时；所有线程经 gate 同时开始；吞吐按最慢线程的 wall time 计算。
- 为每个线程分配严格不重叠的 HBM 范围。在创建 URMA 资源及发送 WR 前，先对
  整个参数矩阵的最大 footprint 做预检：
  `max(threads) * max(windows) * max(sizes)` 必须同时
  不超过 `hbm_len` 和 `remote_seg.len`，目标最后一字节的 UBVA 不得溢出。
  容量不足直接报错退出（状态码 2），不再回退 offset 0，也不运行部分测试矩阵。
  例如 W=1 时 8 KiB 只允许 2 线程×4 KiB；8 线程×W64×4 KiB 至少需要 2 MiB，
  且 NPU 端分配和注册的长度都必须满足要求。
  各 case 顺序复用同一范围；范围之间暂不增加页间隔。
- CSV 中 `avg_completion_interval_us` 是所有线程执行时间之和除以完成操作总数。
  W=1 时可作为线程内同步操作耗时；W>1 时是完成间隔，**不是**单 WR latency，
  也不是 p50/p99。后续应在不扰动热路径的前提下抽样 request latency。

构建（路径和库名需要按安装的 CANN/URMA SDK 调整）：

```bash
cmake -S examples -B build/urma \
  -DCMAKE_CXX_FLAGS="-I${URMA_HOME}/include" \
  -DCMAKE_EXE_LINKER_FLAGS="-L${URMA_HOME}/lib"
cmake --build build/urma -j
```

先启动原 NPU 进程，再运行：

```bash
taskset -c 0-7 ./build/urma/urma_cpu_to_npu_bench \
  --threads 1,2,4,8 --sizes 512,1024,2048,4096 \
  --windows 1,2,4,8,16,32,64 \
  --warmup 1000 --iterations 100000 --completion-timeout-ms 5000
```

输出列为线程数、payload、完成次数、Gb/s、Mops/s、平均线程内延迟和状态码。每组
至少重复 5 次，报告中位数以及最差值；同时记录 CPU 型号、NUMA、NPU/固件/驱动/
CANN/URMA 版本、EID、MTU 和 CPU governor。

## 4. 值得尝试的配置矩阵

以下是实验变量，不是对所有固件都有效的推荐值：

1. **线程与队列拓扑**：1/2/4/8/16 个线程；一线程一 Jetty/JFC，对比共享 JFC
   （仅在对应版本明确保证线程安全时）。线程绑到 UB/NPU 所在 NUMA node 的物理核，
   避免 SMT sibling。
2. **同步深度**：当前已有滑动窗口 1/2/4/8/16/32/64，逐 WR post 后批量 poll。
   下一阶段先在实机确认最优窗口与 CPU 开销，再单独比较 WR 链式批量 post；每个 WR
   请求 completion 的模式也可与间隔 signaled completion 比较，但必须确保 SDK
   允许且能正确回收发送队列。
3. **CQ/JFC**：CQ depth 至少覆盖窗口和线程使用方式，不要无条件设为设备最大值。
   比较 busy poll、事件通知；事件适合降低 CPU 占用，busy poll 适合测最低延迟。
4. **Jetty/QP**：RM、priority、`rnrRetry`、`errTimeout`、多路径与 error suspend
   都应在确认两端能力及语义后逐项改变。单边 WRITE 通常不消耗普通 receive WQE，
   但 Jetty/JFR 配置仍需符合 provider 要求。
5. **内存**：页对齐、huge page、NUMA-local DDR、cacheable/non-cacheable、注册一次
   重复使用；注册和建 QP 时间必须与 steady-state 数据面分开报告。
6. **payload/MTU**：512 B 到 4 KiB 是应用 payload，不保证等于线上的 UB 包。实际
   分片由 provider、链路 MTU 和协议头决定；不要仅凭应用长度称其为物理“包大小”。
7. **测量**：同时报告 Gb/s、Mops/s、CPU 利用率、每核 cycles/instructions、p50/p99
   延迟和错误 completion。用 `steady_clock`，禁止把日志、HBM 回读或 socket ACK
   放入计时区。

## 5. 正确性与安全边界

- 两端的 `token_policy/tokenValue/token_id_valid` 必须一致。样例一端使用 0xEFCD，
  另一端写 0xABCF，同时又配置 `TOKEN_NONE`；这可能在当前 provider 中被忽略，但
  一旦启用 token 校验就会失败。不要照抄到生产环境。
- 校验 `remote_seg.len`，确保 `offset + size` 不越界；远端 raw HBM pointer 只用于
  日志，Host 不应该解引用。
- 每个 case 后可以用独特 pattern 和 NPU D2H 回读做正确性检查；正式计时阶段不要
  回读打印。
- Unix socket 文件默认位于 `/tmp`，且协议接收后就导入远端能力句柄；共享机器上
  应设置受限目录/权限并验证 peer credentials。
- 此示例只测 CPU→NPU WRITE。CPU→CPU 时，远端进程也用 URMA 注册普通内存并导出
  segment/Jetty 即可，Host benchmark 热路径无需改变；但两端都必须有 UB-capable
  device/provider，并不是任意两颗 CPU 仅靠普通网卡就能运行 URMA。

## 6. 公开资料说明

华为公开材料将 Unified Bus 描述为面向多种计算、内存及外设资源互联的统一总线；
具体 URMA/HCCP API、结构体字段和可调范围与已安装驱动及 CANN 版本强绑定。本次环境
访问华为官网被网络代理拒绝（HTTP 403），因此本文没有伪造具体版本的官网链接或把
未核实字段写成稳定 ABI。落地时应优先对照目标机器 SDK 自带的 `urma_api.h`、
`network/hccp*.h`、样例和对应版本《API Reference》，尤其检查线程安全、token、
CQ depth、Jetty mode 和销毁 API。


## 7. 无 URMA SDK 的边界验证

`examples/transfer_layout.h` 是无设备依赖的布局检查，不是传输后端。
其数值契约为：每个线程拥有 `window` 个连续、互不重叠的 `size` 字节槽位，所有
线程的总 footprint 同时受 HBM 与注册 segment 的长度约束。V1 协议仍要求 HBM 起始字节对应
`remote_seg.ubva.va`；`hbm_ptr` 不作为 CPU 可解引用地址，也不能仅靠数值检查
证明两个地址空间的映射或对端元数据真实性。预检失败时 Host 关闭控制连接；
原 NPU server 应处理 EOF 并释放资源，不会收到 WRITE_DONE/DONE。

```bash
cmake -S examples -B /tmp/gamaq-cpu-tests -DBUILD_URMA_BENCH=OFF
cmake --build /tmp/gamaq-cpu-tests -j
ctest --test-dir /tmp/gamaq-cpu-tests --output-on-failure
```

无 CMake 时：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iexamples \
  examples/tests/transfer_layout_test.cpp -o /tmp/transfer_layout_test
/tmp/transfer_layout_test
```

这些测试验证数值边界、窗口上限、乱序槽位复用和布局隔离，不验证 SDK ABI、设备
传输、HBM 数据一致性或性能。
正常 benchmark 构建仍默认开启；W=1 保留原有效地址布局，但 CSV 新增 `window`，
并将最后一个计时列改为语义更准确的 `avg_completion_interval_us`。
默认线程/大小矩阵对 8 KiB NPU 分配现在会明确拒绝，需要扩充并重新注册 HBM，
或显式使用 `--threads 1,2`。

本轮新增使用的 `urma_seg_t::len` 已核对
[openEuler 24.03 LTS SP4 URMA API Guide §2.3.2.1.4](https://docs.openeuler.org/zh/docs/24.03_LTS_SP4/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)，
查阅日期 2026-10-01；该核对不能替代目标机器匹配驱动的 SDK 编译。
后续迭代记录见 [middleware_evolution.md](middleware_evolution.md)。

## 8. Completion 所有权与失败边界

每个 signaled WR 在 post 前登记 `{user_ctx, expected_bytes}`。post 失败时撤销登记；
post 成功后，只有 `user_ctx` 命中当前在途请求、`status == URMA_CR_SUCCESS` 且
`completion_len == expected_bytes`，才计为一次成功操作并允许复用源缓冲区。
错误状态和长度不符会退休命中的请求但将本次操作记为失败；未知、重复或错配的
`user_ctx` 不会替其他请求完成，context 会进入 quarantine。

`--completion-timeout-ms` 默认 5000，合法范围 1–3,600,000 ms。实现使用墙钟截止
时间而非与 CPU 速度相关的固定 poll 次数。为了避免每次空 poll 都读取时钟，
每 256 次空 poll 检查一次截止时间；因此这是故障边界，不是精确计时器。
poll 接口失败或超时后，已成功 post 的 WR 可能仍在设备内。公开 API 文档没有说明
普通 `urma_delete_jetty` 会同步 drain 这些 WR，因此程序停止后续参数矩阵，跳过该
context 的常规 unimport/delete/free 链，并且不发送 `HOST_WRITE_DONE`/`DONE` 成功
控制命令。资源暂留到进程退出，由 provider/kernel 做最终回收；这是一种故障隔离，
不是面向常驻服务的恢复方案。后续中间件必须基于目标 SDK 核实 suspend/flush/drain
流程，才能在同一进程内安全恢复。

完成字段语义已于 2026-10-02 核对
[openEuler 24.03 LTS SP4 URMA API Guide 的 `urma_poll_jfc`/`urma_cr_t`](https://docs.openeuler.org/zh/docs/24.03_LTS_SP4/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)：
`user_ctx` 对应 WR，`completion_len` 是传输字节数，状态枚举还包括 timeout、flush、
suspend 等失败。目标机器仍应以匹配驱动版本的头文件和实际行为为准。

## 9. 有界滑动窗口契约

窗口状态机把 `request_id -> {expected_bytes, source_slot}` 作为最小所有权单元。
窗口满时不再 post；CR 可以乱序到达，但只能释放其 `user_ctx` 对应的槽位。单 WR
post 失败会撤销尚未交给 provider 的当前槽位、停止继续下发，并继续 poll 此前已被
provider 接受的 WR；全部正常退休后允许释放上下文。drain 中再次 poll 失败、未知
CR 或超时仍进入 quarantine，且不会用“最初的 post 错误”掩盖 drain 的二次错误。
任一 worker 失败会发布全局 stop；其他 worker 不再 post，但会继续 poll，直到已提交
请求全部退休后返回取消，从而尽量减少跨线程失败留下的未知在途资源。

每个 case 的 JFS depth 等于窗口，JFC depth 等于 `window + 1`，额外一项用于一个
关联 Jetty 的异步错误 CR；若设备查询到的 `max_jfs_depth`、`max_jfc_depth` 或
`max_jfr_depth` 不满足要求，就显式拒绝该 case。这个配置依据 2026-10-03 查阅的
[openEuler 24.03 LTS SP3 URMA API Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)：
文档建议 JFC depth 至少覆盖关联发送队列产生的 CR，再为每个关联 Jetty 预留一项；
`urma_poll_jfc` 对 RDMA device 单次最多返回 16 条 CR。

SDK-independent reference 测试对 W=1–64 各运行 1024 次非 FIFO 完成，检查在途量不
超过窗口、忙槽不被复用、完成只释放匹配槽。真实 provider 是否保证所需 completion
字段、窗口对应的队列能力以及实际吞吐/CPU 收益仍须目标 CANN 9.1 与 NPU/UB 实机验证。

## 10. 可复用窗口调度与 backend 边界

`window_scheduler.h` 把窗口引擎从 URMA 结构体中分离。backend 只实现两个同步调用：

- `post(TransferRequest)`：接收 `{request_id, slot, bytes}`；返回 0 才表示 provider
  接受当前请求，非零状态不允许调度器假设请求在途或进行无依据重试。
- `poll(TransferCompletion*, max_count)`：返回标准化的 request ID、长度、传输状态；
  负数表示本次 poll 失败，返回数不能超过 `max_count`。

窗口容量、request ID、槽位生命周期、背压、全局 stop、completion 校验、进展超时
与 drain 均由公共调度器负责。`UrmaWriteBackend` 只构造 URMA SGE/WR、调用 post/poll
并标准化 CR。该边界使用 C++ 模板静态绑定，不在 post/poll 热路径引入虚函数或
`std::function` 间接调用；是否真正降低或保持 CPU 开销仍需实机 profile。

状态机保留 primary error 与 drain error。例如第 3 次 post 失败时，请求 1、2 如果
已经被接受，就停止补充窗口并等待它们完成；若随后 poll 又失败，结果同时保留 post
和 drain 状态，且上下文不可销毁。已知 request 的错误 CR 会退休对应槽并 drain 其余
请求；未知 request、poll 失败、非法 completion 数和超时无法证明 quiescence，继续
fail-closed quarantine。

mock backend 覆盖 W=1–64 的 7,168 次乱序成功完成，以及部分 post 失败、取消、传输
错误、短 completion、未知/重复归属、poll 错误、非法返回数、确定性超时和 request ID
耗尽。它验证调度契约而非 URMA ABI 或 NPU 数据可见性；“传输完成”仍不自动等于
“对端计算可消费”，后者需要目标 SDK/设备协议给出额外同步证据。

## 11. 首个应用适配器：离散 KV block 批量 put

`kv_put_batch.h` 定义首个窄业务接口：把调用者已经注册的 CPU region 中多个离散
KV block 写到已经注册/导入的 NPU region。每项使用 `{block_id, source_offset,
destination_offset, bytes}`；offset 只相对于两个 region，不是可由另一地址空间直接
解引用的指针。该层不负责 KV key 查询、内存分配/注册、地址空间转换或对端通知。
当前只定义 CPU→NPU put/offload，不据此声称支持 get、NPU→NPU、远端 CPU 或 SSD。

提交任何请求之前，adapter 对整个 batch 执行 fail-fast 验证：长度非零，源/目标地址
加法不溢出，范围不超过各自注册区，block ID 唯一，目标范围互不重叠。只读源范围
允许被多个 block 共享；重叠目标会产生不明确的最终内容，因此拒绝。验证失败不调用
backend，也不产生部分提交。

每个 `KvPutItemResult` 明确区分：

- `not_submitted`：未交给 backend，源仍可复用；
- `submitted`：backend 可能仍引用源，不能复用；
- `transfer_complete`：命名 completion 成功，源可复用且传输完成；
- `post_failed`：当前请求未被接受，源可复用，后续项不再提交；
- `completion_failed`：命名请求已退休但传输失败，源可复用，不算成功传输。

所有状态的 `peer_consumable_proven` 当前都保持 false。单边 WRITE completion 与 CPU
load/store 内存语义、NPU kernel 可消费事件严格分离；后续必须增加目标 SDK 支持的
可见性/通知协议，才能由应用显式推进该状态。API 不自动重试失败 block，因为当前
没有调用者提供的幂等性或版本条件。

reference-memory backend 验证离散 offset 实际复制、W=2 背压、乱序完成、共享只读
源、逐项状态、部分 post 失败、poll 失败、错误/短 completion 和全部输入边界。
这些结果验证 API 契约，不代表 URMA SGL、设备数据可见性或实机性能已经验证。

## 12. Registered region 与 URMA KV backend

`registered_region.h` 把“已注册区域中的 offset”解析为 transport address。它同时检查
请求长度、region 容量、`base + offset` 和最后一个字节的 `uint64_t` 溢出，并分别保留
源、目标侧的错误。该结构只是数值 view，不注册内存、不拥有 provider handle，也不
把 NPU UBVA 当作 CPU 可解引用指针。

`urma_kv_backend.h` 提供非 owning 的 `UrmaRegionBinding` 和 `UrmaKvPutBackend`：调用者
分别绑定 CPU 本地注册 segment、NPU 导入 segment、Jetty/JFC 与远端 Jetty；backend
按每个 `TransferRequest` 的 source/destination offset 构造两个 SGE 和 signaled
`URMA_OPC_WRITE`，并把 CR 标准化为公共 completion。上层 batch 已验证输入，但
backend 在 post 边界再次执行 region/地址检查，防止绕过 adapter 的调用把越界 WR
交给 provider。热路径没有堆分配或虚函数。

该 binding 不拥有 context、segment、Jetty 或 buffer。`UrmaSession` 保证：请求全部
命名退休之前资源持续有效；未知 CR、poll 错误或超时后不得据此普通释放资源。

`urma_kv_backend_test` 使用 openEuler UMDK 官方结构体并 stub 仅有的 post/poll 调用，
直接核对 WR opcode、completion flag、两端 SGE address/length/segment、target Jetty、
`user_ctx`、provider 错误透传和 CR 标准化。它没有访问设备，不能证明目标 CANN ABI、
segment 权限、数据可见性或性能。

## 13. 可复用 URMA session 与 fail-closed teardown

`urma_session.h` 把每个 worker 的 context、JFC、JFR、Jetty、本地注册 segment、远端
导入 segment/Jetty、源 buffer 和 completion tracker 收拢为一个不可复制、不可移动的
session。benchmark 现在通过该对象建立和复用资源，数据面仍保持一 worker 一 session，
不假设 provider 对跨线程共享对象是安全的。`source_binding()` 与
`destination_binding()` 可直接供 KV batch backend 使用，避免重新注册或复制数据。

open 过程先查询能力，再按 `JFC -> JFR -> Jetty -> buffer -> local segment -> remote
segment -> remote Jetty` 建立依赖。任一步失败都按逆序 rollback，并同时返回主要失败
和 rollback 失败；不让清理错误掩盖原始错误。设备能力小于 `JFC=window+1`、
`JFS=window`、`JFR>=1` 时显式拒绝，不把设备上限当作可用能力。

close 只有 completion tracker 证明零在途时才执行逆序 teardown。已知在途请求返回
`in_flight`，调用者 drain 后可以重试；未知 completion/poll/timeout 会先 quarantine，
之后不再调用 provider 销毁函数。任一 unimport/delete/unregister 返回失败时也立即停止
后续 teardown 并 quarantine，避免在依赖项可能仍存活时级联释放 owner。这里刻意不重试
销毁操作，因为公开契约没有证明失败调用是幂等的。析构函数只尝试同一套 close；常驻
服务必须显式检查 close 结果，不能把进程退出回收当作在线恢复。

stub-provider 测试覆盖完整建立/销毁顺序、八个 open 失败点、队列能力不足、drain 后
重试、显式 quarantine、teardown 失败停止级联，以及“主要 open 失败 + rollback 失败”
的双重证据。它验证所有权状态机和官方类型兼容性，不证明目标 provider 的 flush、
quiescence、HBM 可见性或真实资源回收行为。

## 14. Session-facing KV put 与运行后健康状态

`urma_kv_session.h` 提供首个实际应用入口 `run_urma_kv_put_batch()`。调用者先把 KV
数据写入 `UrmaSession::buffer()` 的预注册区域，再提交相对源/目标 offset；入口自动把
session 拥有的 Jetty、JFC、本地 segment、远端导入 segment/Jetty、completion tracker
和单调 request ID 绑定到 `UrmaKvPutBackend`。连续 batch 复用同一组 provider 资源，
但同一 session 不允许混入已有在途请求。多 worker 仍应使用各自 session。

运行结束后不能把“资源可销毁”与“连接可继续使用”混为一谈：

- 全部成功，或 batch 在 provider 调用前验证失败：session 保持 ready，可提交下一批；
- post 失败或已命名的错误 completion 已完整 drain：session 标记 retired，禁止隐式重试
  或继续提交，但允许按正常依赖顺序 close；
- poll 失败、未知 completion 或 timeout 导致所有权不明：session 标记 quarantined，
  禁止提交和 provider teardown，等待进程退出或未来经目标 SDK 核实的恢复流程。

外部 stop 导致的已 drain cancellation 不自动判定 provider 故障，调用者清除 stop 后
可以复用；这与 transport 错误的退役路径分离。所有 item 仍只把命名传输 completion
解释为源可复用/传输完成，不把它提升为 NPU kernel 可消费。

stub-provider 测试已经直接执行两批离散 KV put，核对两端地址、segment 和 request ID
跨批递增；同时覆盖输入预检、非法 timeout、已有在途拒绝、post/命名 completion 失败
后的退役，以及 poll 失败后的隔离。该入口没有加入数据复制或内存分配策略，业务数据
如何进入预注册 buffer、以及对端消费通知仍属于更上层协议。
