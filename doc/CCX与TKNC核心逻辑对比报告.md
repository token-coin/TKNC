# CyberChain(CCX) 与 TKNC 核心逻辑对比报告

> 日期：2026-06-30
> 对比范围：节点区块验证、矿工提交、难度调整（不含排放模型）
> CCX 源码来源：https://github.com/CyberChainXyz/go-cyberchain (geth fork, Go)
> CCX 哈希库：https://github.com/CyberChainXyz/fphash-go (Cryptonight, C++/cgo)
> TKNC 源码：d:\TKNC\src (比特币核心改造, C++)

---

## 一、架构差异总览

| 维度 | CCX (go-cyberchain) | TKNC |
|------|---------------------|------|
| 基础框架 | go-ethereum (geth) fork | Bitcoin Core fork |
| 语言 | Go + C++(cgo) | C++ |
| PoW 算法 | fphash = Cryptonight（完整内存硬） | tokenhash = Cryptonight 变种(cn_v4) |
| 哈希库来源 | fphash-go（自带 cn_slow_hash_*.cpp） | 内嵌 src/pow/cn_slow_hash_*.cpp（**同源**） |
| 难度调整模型 | 以太坊式（线性增量） | 比特币式（timespan 比例） |
| PoW 比较空间 | 完整 256 位 | 原 8 字节（已修复为 32 字节） |

**关键发现**：TKNC 的 `src/pow/cn_slow_hash_soft.cpp` 等文件与 fphash-go 的同名文件**同源**——TKNC 的 tokenhash 就是从 fphash-go 移植的 Cryptonight 实现。

---

## 二、节点区块验证逻辑对比

### 2.1 CCX 的区块验证（consensus/ethash/consensus.go → verifySeal）

```
验证流程：
1. verifyHeader: 检查 extra-data、时间戳、难度匹配 CalcDifficulty、gasLimit 等
2. verifySeal (Cyber 分叉后):
   - hashSource = SealHash(header) + seedHash(number) + Nonce
   - result = fpHash(hashSource)              ← 完整 Cryptonight，输出 32 字节
   - 验证: SetBytes(result) <= two256 / Difficulty   ← 完整 256 位比较
   - Cyber 分叉后 MixDigest 必须为零（不验证 mix digest）
```

**CCX 特点**：
- 矿工和验证节点**用同一个 fpHash 函数**（fphash-go），无快通道
- PoW 验证是完整 256 位比较：`result <= 2^256 / Difficulty`
- 输出 32 字节全部参与比较

### 2.2 TKNC 的区块验证（src/pow.cpp + src/validation.cpp）

```
验证流程（ContextualCheckBlockHeader, validation.cpp:4114）:
1. 检查 nBits == GetNextWorkRequired(...)        ← 难度匹配
2. [已修复] PermittedDifficultyTransition 检查    ← 4倍幅度限制
3. CheckProofOfWork (pow.cpp:106):
   - powHash = TKNCComputeHash(header)           ← 调 hash_tokenhash_compat
   - 验证: UintToArith256(powHash) <= bnTarget   ← 256 位比较
```

**TKNC 原版问题（已修复）**：
- `hash_tokenhash_compat`（cn_slow_hash_soft.cpp:635）**只做 1 次 keccakf**，输出 8 字节有效 + 24 字节全 0
- 等效比较空间从 2^256 缩水到 2^64
- GPU 矿工验证候选块时也调 `TKNCComputeHash`（real_gpu_miner.cpp:569），走同一快通道

**TKNC 修复后**：
- `hash_tokenhash_compat` 改为调用完整 `software_hash_3`/`hardware_hash_3`，输出 32 字节
- 与 CCX 一致：矿工和验证用同一完整哈希

### 2.3 验证逻辑对比结论

| 对比项 | CCX | TKNC 原版 | TKNC 修复后 |
|--------|-----|-----------|-------------|
| 哈希函数 | 完整 Cryptonight（32字节） | compat 仅 1 次 keccakf（8字节有效） | 完整 Cryptonight（32字节）✓ |
| 矿工与验证一致性 | 一致 | **不一致**（GPU 完整 vs CPU 快通道） | 一致 ✓ |
| PoW 比较空间 | 256 位 | **64 位**（后24字节恒0） | 256 位 ✓ |
| 难度跳变校验 | 以太坊式天然小幅 | 无（PermittedDifficultyTransition 未调用） | 4倍 clamp + PermittedDifficultyTransition ✓ |

---

## 三、矿工提交逻辑对比

### 3.1 CCX 矿工（consensus/ethash/sealer.go → mine_fphash）

```go
func mine_fphash(block, id, nonce, abort, found):
    header = block.Header()
    hash = SealHash(header).Bytes()
    target = two256 / header.Difficulty
    hashSource = hash + seedHash(number)

    for nonce++:
        tryHashSource = hashSource + nonceBytes
        result = fpHash(tryHashSource)              ← 完整 Cryptonight
        if SetBytes(result) <= target:
            header.Nonce = nonceBytes
            found <- block.WithSeal(header)         ← 提交
```

**CCX 特点**：
- 矿工和验证用**同一个 fpHash**（fphash-go 的 `Hash()` 函数）
- hashSource = sealHash + seedHash + nonce（拼接后整体哈希）
- 不需要 DAG，内存硬但计算路径单一

### 3.2 TKNC 矿工（src/pow/opencl_miner.cpp + real_gpu_miner.cpp）

```
GPU 矿工流程 (opencl_miner.cpp:790):
1. GPU 内核 (tokenhash.cl) 跑完整 Cryptonight 遍历 nonce
   - explode_scratchpad(2MB) → inner_hash(0x300轮) → implode → keccakf
   - if as_ulong(State[0]) <= Target: 记录候选 nonce
2. CPU 验证候选 (opencl_miner.cpp:797):
   - hash = TKNCComputeHash(verifyHeader)    ← 走 hash_tokenhash_compat
   - if hash <= target: 提交区块

real_gpu_miner.cpp:569 同理。
```

**TKNC 原版问题**：
- GPU **搜索**用完整 Cryptonight（tokenhash.cl，慢）
- CPU **验证**用 compat 快通道（1 次 keccakf，快）
- 两者计算量相差几个数量级 → generatetoaddress（走 compat）远快于 GPU 矿工

**TKNC 修复后**：
- compat 改为完整 Cryptonight → CPU 验证与 GPU 搜索计算量一致
- generatetoaddress 在 mainnet 被禁用（仅 regtest）

### 3.3 矿工提交对比结论

| 对比项 | CCX | TKNC 原版 | TKNC 修复后 |
|--------|-----|-----------|-------------|
| 搜索哈希 | fpHash（完整 Cryptonight） | GPU: 完整 Cryptonight | GPU: 完整 Cryptonight |
| 验证哈希 | fpHash（同一函数） | compat（1次keccakf，**快通道**） | 完整 Cryptonight ✓ |
| 搜索与验证一致性 | 一致 | **不一致** | 一致 ✓ |
| nonce 注入方式 | 拼接到 hashSource 末尾 | 写入 header.nNonce（state[8]位置） | 写入 header.nNonce ✓ |
| generatetoaddress | N/A（geth 无此RPC） | mainnet 可用（**漏洞入口**） | 仅 regtest ✓ |

---

## 四、难度调整逻辑对比

### 4.1 CCX 难度调整（consensus.go → calcDifficultyCyber）

```
公式（以太坊式）:
diff = parent_diff + parent_diff/2048 * max(
    (2 if parent有叔块 else 1) - (timestamp - parent_timestamp) // 9,
    -99
)
最小难度 = 131072 (2^17)
无难度炸弹（Cyber 分叉移除）
```

**CCX 难度调整特性**：
- 调整因子范围：[-99, 2]
- **单步最大涨幅**：factor=2（当 timestamp 差=0），即 `+2/2048 ≈ +0.098%`
- **单步最大跌幅**：factor=-99，即 `-99/2048 ≈ -4.8%`
- 天然防暴增：即使瞬间出块，每步难度最多涨 0.098%，需 ~7000 块才能翻倍
- RISE 分叉：除数改为 4（更敏感），但单步最大涨幅仍 `+2/2048 ≈ 0.098%`

### 4.2 TKNC 难度调整（src/pow/difficulty.cpp）

```
公式（比特币式 timespan 比例）:
new_target = parent_target * (total_timespan / expected_timespan)
  其中 total_timespan = 最近10块时间跨度之和（每块 timespan clamp 到 [30s, 1200s]）
       expected_timespan = 10 * 120 = 1200s

[已修复] 4倍幅度限制: ClampDifficulty(new_target, parent_target, pow_limit)
  max_target = parent_target * 4
  min_target = parent_target / 4
```

**TKNC 原版问题**：
- 无 4 倍限制 → 瞬间出块时 timespan≈9s，`new_target = parent * 9/1200 ≈ parent/133`
- 单步难度暴增 133 倍，累积指数增长
- timespan 下限仅 1 秒（已修复为 30 秒）

**TKNC 修复后**：
- 4 倍 clamp：单步最多涨/跌 4 倍
- timespan 下限 30 秒（TARGET/4）：即使 4 倍被绕过，难度最多按 1/4 时间算（即 4 倍涨幅）

### 4.3 难度调整对比结论

| 对比项 | CCX | TKNC 原版 | TKNC 修复后 |
|--------|-----|-----------|-------------|
| 调整模型 | 以太坊式（线性增量） | 比特币式（timespan比例） | 比特币式（timespan比例） |
| 单步最大涨幅 | +0.098%（天然安全） | **无限制**（可达133倍） | 4倍 ✓ |
| 单步最大跌幅 | -4.8% | 无限制 | 4倍 ✓ |
| timespan 下限 | 隐式（//9 除法） | 1秒 | 30秒 ✓ |
| 难度炸弹 | 无（Cyber移除） | 无 | 无 |
| 瞬间出块影响 | 几乎无（+0.098%/块） | **灾难性**（133倍/块） | 可控（4倍/块） ✓ |

---

## 五、安全性总结

### 5.1 CCX 为何不会出现 TKNC 的瘫痪问题

CCX 有**三重天然防护**（架构层面自带，非刻意设计）：

1. **哈希一致**：矿工和验证用同一个 fpHash，无快通道 → generatetoaddress 等价物即使存在也不会比矿工快
2. **256位完整比较**：fpHash 输出 32 字节全部参与 `<= target` 比较 → 无 8 字节缩水问题
3. **以太坊式难度**：单步最多 +0.098% → 即使瞬间出块也需 7000 块才翻倍，不会雪崩

### 5.2 TKNC 原版的三个致命缺陷

| 缺陷 | CCX是否有 | TKNC原版 | TKNC修复后 |
|------|-----------|----------|------------|
| 哈希快通道（1次keccakf） | 无 | 有 | 已消除 ✓ |
| 8字节有效哈希空间 | 无 | 有 | 已扩展到32字节 ✓ |
| 难度无4倍限制 | 无（天然0.098%） | 有 | 已加4倍clamp ✓ |

### 5.3 修复后的防御纵深

TKNC 修复后相比 CCX 的额外防御层：

| 层级 | 防御措施 | CCX是否有 | TKNC修复后 |
|------|---------|-----------|------------|
| L1 哈希统一 | 矿工=验证=完整Cryptonight | ✓（架构自带） | ✓（修复） |
| L2 PoW完整比较 | 256位 | ✓（架构自带） | ✓（修复） |
| L3 难度4倍clamp | 不需要（天然0.098%） | ✗ | ✓（新增，比CCX更严格） |
| L4 timespan下限30s | 不需要 | ✗ | ✓（新增，第二层） |
| L5 验证启用PermittedDifficultyTransition | 不需要 | ✗ | ✓（新增，纵深） |
| L6 generatetoaddress限regtest | N/A | ✗（mainnet可用） | ✓（新增） |

### 5.4 排放逻辑影响

本次对比和修复**未触及排放逻辑**：
- TKNC 的 emission.h（10亿总量/9年）未改动
- CCX 的区块奖励（Cyber: 832 ETH/4年减半）与 TKNC 无关
- 难度修复只影响出块速度的调整方式，不改变奖励数额
- 哈希修复改变共识规则但不改变奖励规则

---

## 六、结论

1. **CCX 不会出现 TKNC 的瘫痪问题**，因为其架构（geth + fphash）天然具备哈希一致、256位比较、以太坊式小幅难度调整三重防护。

2. **TKNC 的瘫痪是三个缺陷叠加**：哈希快通道 + 8字节空间 + 无4倍限制。单独修复任一项都不够——快通道让攻击者瞬间出块，8字节空间让低难度下2次命中，无4倍限制让难度雪崩。

3. **TKNC 修复后比 CCX 更严格**：CCX 依赖架构天然的 0.098% 小幅调整，TKNC 则额外加了 4 倍 clamp + timespan 下限 + PermittedDifficultyTransition 三层显式防御。即使有人篡改 TKNC 代码改回快通道，4 倍限制仍能防住雪崩。

4. **共识规则变更影响**：哈希修复（compat 改为完整 Cryptonight）是共识规则变更，用旧快通道挖出的块会被新代码拒绝。必须回退到 generatetoaddress 之前的高度重新同步。
