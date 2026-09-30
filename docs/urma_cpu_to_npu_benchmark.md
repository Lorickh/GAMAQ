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
- 每个线程只保持 1 个 outstanding WR，完成轮询不 sleep。因此结果代表
  CPU post + doorbell + completion 的同步性能。
- 预热不计时；所有线程经 gate 同时开始；吞吐按最慢线程的 wall time 计算。
- 尽量为线程分配互不重叠的 HBM offset。若 NPU 仍只分配原来的 8 KiB，则回退到
  offset 0；做 4/8 线程测试时建议把 NPU `MEM_SIZE` 至少改为
  `max_threads * 4096`，最好使用 64 MiB 以上并按 cache/page 做间隔。
- CSV 中 `avg_thread_latency_us` 是所有线程执行时间之和除以完成操作总数；它不是
  p50/p99。若需要分位数，应抽样记录单次延迟，避免每次记录本身扰动热路径。

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
  --warmup 1000 --iterations 100000
```

输出列为线程数、payload、完成次数、Gb/s、Mops/s、平均线程内延迟和状态码。每组
至少重复 5 次，报告中位数以及最差值；同时记录 CPU 型号、NUMA、NPU/固件/驱动/
CANN/URMA 版本、EID、MTU 和 CPU governor。

## 4. 值得尝试的配置矩阵

以下是实验变量，不是对所有固件都有效的推荐值：

1. **线程与队列拓扑**：1/2/4/8/16 个线程；一线程一 Jetty/JFC，对比共享 JFC
   （仅在对应版本明确保证线程安全时）。线程绑到 UB/NPU 所在 NUMA node 的物理核，
   避免 SMT sibling。
2. **同步深度**：当前 depth=1。下一阶段应实现滑动窗口 1/2/4/8/16/32/64：先 post
   N 个 WR，再批量 poll。小包带宽通常更依赖 outstanding 数；每个 WR 请求 completion
   的模式也可与间隔 signaled completion 比较，但必须确保 SDK 允许且正确回收 SQ。
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
