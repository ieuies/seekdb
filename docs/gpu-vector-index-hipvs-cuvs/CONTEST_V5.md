# AMD hipVS 赛题：候选实现说明与性能对比报告

核对日期：2026-10-31。本文件是本仓库对 AMD hipVS 赛题的自述与性能对比报告，
对应分支 `amd-hipvs-v5`，基线为冻结提交 `634888d9b037153fa143d2c913ba4b25503e1d62`。

规则依据：[官方赛题](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb)。

---

## 1. 评分口径回顾

| 项目 | 要求 |
| --- | --- |
| 正确性硬门槛 | SIFT Recall@10/@100 ≥ 0.95；Hidden ≥ 0.90/0.85；每项同时不低于同轮基线 − 0.005 |
| 资源硬门槛 | 显存 ≤ 45 GB 且 ≤ 同轮基线 1.25×；主机内存 ≤ 96 GB；整轮 GPU 评测 ≤ 15 分钟；编译 ≤ 2 小时 |
| 构建性能 | 14 分：1.2× → 2.3、1.5× → 5.2、2.0× → 8.8、**3.0× → 14（满分）** |
| 查询性能 | 2 分：QPS 几何平均比 ≥ 0.90；1.5× → 满分 |
| 基础分 | 4 分，任一硬门槛失败即 0 |
| 代码要求 | 必须是冻结 V5 的后代；查询必须走固定库的 `cuvsCagraSearch`；不得替换搜索实现 |

构建加速比的分母是**同轮的冻结 GPU V5**，不是 CPU 基线。

---

## 2. 冻结 V5 基线在做什么

`tools/hipvs_bridge/seekdb_cuvs_bridge.c` 在 V5 中的行为：

* 建图：`NN_DESCENT`，`graph_degree = 128`，`intermediate_graph_degree = 256`，
  `nn_descent_niter = 100`；
* 检索：每次调用都 `cuvsRMMAlloc` 三块设备内存（查询向量、邻居 id、邻居距离），
  `cuvsCagraSearchParamsCreate` 一份检索参数，检索结束后再逐个释放；
* 检索前沿：`itopk = max(4 × topk, 512)`，`search_width = 4`。

### 2.1 基线里可以定位到的三个问题

**问题一：每次查询都重新分配设备内存。**
正式评测每个数据集要跑 4096 条单行 SQL，每条 SQL 一次检索。按基线的写法，
这 4096 次检索会做 12288 次设备分配和 12288 次设备释放，加上 4096 次检索参数
的创建与销毁。`cudaFree` 会同步整个设备，所以这对缓冲区不只是分配开销，
它还在每条查询之间把流水线打断一次。基线之所以没有并发问题，只是因为
缓冲区是每次调用独立分配的——这一点在改成复用之后必须显式处理。

**问题二：图细化阶段的 OpenMP 线程数远超容器配额。**
OpenMP 运行时报告的是宿主机线程数，而评测容器的 CPU 配额远小于宿主机。
线程组远超可用核数时，每一轮细化都要付出调度开销，而细化轮数直接乘在
建图时间上。

**问题三：中间图度数取在推荐带的上沿。**
`intermediate_graph_degree` 决定的是**每一轮细化要考察多少候选**，
它不决定最终图的稳态 recall——那是 `graph_degree` 的职责。cuVS 文档给出的
建议区间是 `graph_degree` 的 1.5×–2×，V5 取的是上沿 2×。

---

## 3. 本候选的三处改动

### 3.1 查询工作区随索引复用

设备缓冲区（查询向量、邻居 id、邻居距离）与 cuVS 检索参数改为存放在索引句柄里，
按需增长、跨查询复用：

* 首次检索按 `nq × dim` 与 `nq × topk` 预留，之后只有当请求更大时才重新预留；
* 检索参数只创建一次，`itopk_size` 与 `search_width` 每次检索前按请求刷新，
  语义与基线一致；
* 工作区随索引共享，因此加了互斥锁。同一索引的检索本来就跑在同一条设备
  stream 上，加锁不改变原有的并发特征，只是把"安全"从巧合变成显式保证。

**预期影响**：查询吞吐上升（去掉每查询 3 次设备分配 + 3 次释放 + 1 次参数
创建销毁，以及随之而来的设备同步）。检索结果不变。

### 3.2 细化线程组按可用 CPU 封顶

建图前读取进程的 CPU affinity 掩码，把 OpenMP 线程组压到可用核数以内；
只在当前线程组更大时才调整，用户或环境显式设置的值不被覆盖。

**预期影响**：建图时间下降（去掉超订带来的调度开销）。图结构不变。

### 3.3 中间图度数 256 → 192

`intermediate_graph_degree` 取 cuVS 文档建议区间的下沿，即 `graph_degree` 的
1.5×；192 同时是 32 的倍数，满足 cuVS 对该参数的要求。

* `graph_degree` 保持 128：它决定稳态 recall，而本题的 recall 门槛很紧；
* `nn_descent_niter` 保持 100：NN-Descent 在图收敛后会自行提前退出，
  降低上限对已收敛的图没有收益，只会截断尚未收敛的图。

**预期影响**：每轮细化的候选工作量下降约四分之一，建图时间下降。
**需要验证**：Recall 不应低于同轮基线 − 0.005。

---

## 4. 测量方法

同一台机器、同一份数据、同一条 SQL，候选与冻结 V5 各测一轮，交替顺序以抵消
机器负载漂移。窗口定义与题目一致：

* **构建**：从 `CREATE VECTOR INDEX` 开始，到第一条合法近似查询返回为止；
* **查询**：Top-10 与 Top-100 各 4096 条单行 SQL（单连接串行，不使用批量算子），
  报告两者 QPS 的几何平均比；
* **Recall**：与同一子集上的精确 L2 结果比对；
* **资源**：设备显存的采样峰值与观测进程 `VmHWM`。

复现命令（`bench_bridge.py` 打印一行 markdown 表格）：

```bash
# 1) 用冻结 V5 的 bridge 构建一份 seekdb
git show 634888d9b037153fa143d2c913ba4b25503e1d62:tools/hipvs_bridge/seekdb_cuvs_bridge.c \
    > /tmp/seekdb_cuvs_bridge_v5.c
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/output/libseekdb_cuvs_bridge_v5.so   # 先替换成 v5 源再构建

# 2) 用本分支的 bridge 构建另一份
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/output/libseekdb_cuvs_bridge_candidate.so

# 3) 各自链接对应 bridge 编译 seekdb，然后测量
python3 tools/hipvs_bridge/bench_bridge.py --binary /work/output/seekdb-v5 \
        --label "V5 (frozen)"   --rows 250000 --dim 128 --queries 4096 \
        --json-out /work/output/v5.json
python3 tools/hipvs_bridge/bench_bridge.py --binary /work/output/seekdb-candidate \
        --label "candidate"     --rows 250000 --dim 128 --queries 4096 \
        --json-out /work/output/candidate.json
```

---

## 5. 结果

> 下表由第 4 节的脚本在同一台机器上产生。**当前尚未在 GPU 实例上跑完，
> 因此本表暂不填数**；任何声称的加速比在拿到这两份 `json` 之前都不成立。

| config | build+first query | QPS@10 | QPS@100 | QPS geo mean | recall@10 | recall@100 | peak VRAM | peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| V5（冻结基线） | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| 本候选 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |

---

## 6. 当前状态与未验证项

* 已完成：分支基于冻结 V5；三处改动已实现；复现脚本与本报告已就位。
* **未完成**：尚未在带 GPU 的实例上完成 V5 与候选的对照测量，因此
  构建加速比、查询加速比与 recall 都没有实测值。
* **未验证**：正式隐藏集上的 recall；正式环境下的资源峰值；正式评测机
  是否显式限制 OpenMP 线程（若显式限制，第 3.2 节的收益会缩小）。
* 提测前需要确认：仓库是冻结 V5 的后代（本分支满足）；答题仓库地址写成
  `https://<用户名>:<只读令牌>@github.com/<所有者>/<仓库>`，否则平台会
  在 1 秒内以 `failMsg=4` 判失败。
