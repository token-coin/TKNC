# TKNC 区块链攻击全面审计报告

**审计日期**: 2026-07-01  
**审计方法**: 逐行代码扫描 + 攻击向量搜索 + 源码对照CCX  
**审计范围**: `d:\TKNC\src\` 全部源码

---

## 🔴 严重漏洞（需立即修复）

### VULN-1: 外部矿工遗漏交易费用 — `miner/miner.cpp`

**严重度**: 🔴 HIGH (中等严重但影响经济模型)  
**文件**: `src/miner/miner.cpp:443-459`

```cpp
CAmount blockReward = GetTKNCBlockSubsidy(realHeight);  // 只有补贴
// ... 从未加入 nFees！
CAmount minerReward = blockReward * 90 / 100;
```

**问题**: `tknc-miner.exe` 包含交易进区块但不收取交易费。费用永久销毁（通缩），矿工每块损失收入。

**影响**: 所有GPU矿工无法收取交易费。只有内部矿工(`node/miner.cpp`)正确处理`nFees + subsidy`。

**修复**: 从`getblocktemplate` RPC响应中提取总费用并加入coinbase。

---

### VULN-2: `SIGPUSHONLY`不在MANDATORY验证标志中

**严重度**: 🔴 HIGH  
**文件**: `src/policy/policy.h:104-110`

```
MANDATORY_SCRIPT_VERIFY_FLAGS 不包含 SCRIPT_VERIFY_SIGPUSHONLY
STANDARD_SCRIPT_VERIFY_FLAGS  包含 SCRIPT_VERIFY_SIGPUSHONLY
```

**问题**: 非push-only的scriptSig可通过共识验证(mempool拒绝但区块接受)。这是Bitcoin Core的设计选择，但对于TKNC链可能被利用。

**影响**: 攻击者挖带恶意scriptSig的块（非push-only），该块被网络接受但mempool会拒绝。

---

## 🟡 中等风险

### VULN-3: CLTV/CSV未激活时退化为NOP

**严重度**: 🟡 MEDIUM  
**文件**: `src/script/interpreter.cpp:524-527, 563-565`

```cpp
case OP_CHECKLOCKTIMEVERIFY:
    if (!(flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY)) {
        break;  // 退化为NOP！
    }
```

**影响**: 链初始化时BIP65/BIP112可能未激活（需软分叉投票），在此期间时间锁操作码为空操作，攻击者可绕过时间锁。

**当前状态**: `BIP65Height=1, CSVHeight=1` → 从高度1就激活，**实际上已缓解**。但代码路径理论上存在绕过可能。


### VULN-4: `AccessCoin`返回空Coin不抛异常

**严重度**: 🟡 MEDIUM  
**文件**: `src/coins.cpp:158-165`

```cpp
const Coin& CCoinsViewCache::AccessCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) {
        return coinEmpty;  // 返回空对象而非抛异常！
    }
    return it->second.coin;
}
```

**影响**: 调用者若未检查`IsSpent()`，可能使用全零Coin数据做签名验证等操作。

### VULN-5: BIP68检查依赖CSV部署状态

**严重度**: 🟡 MEDIUM  
**文件**: `src/validation.cpp:2472-2474`

```cpp
if (DeploymentActiveAt(*pindex, m_chainman, Consensus::DEPLOYMENT_CSV)) {
    nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;
}
```

**当前状态**: `CSVHeight=1` → 已激活，**实际无风险**。

### VULN-6: 难度DAA时间戳仅用父块跨度

**严重度**: 🟡 MEDIUM  
**文件**: `src/pow/difficulty.cpp:33-35`

```cpp
// 使用 pindexLast->GetBlockTime() - pindexLast->pprev->GetBlockTime()
// 而不是 block自己的时间戳。与CCX不同，存在1块延迟。
```

**与CCX对比**: CCX用`CalcDifficulty(header.Time, parent)`即当前块的时间戳。TKNC用父块时间差作代理。1块延迟在稳态无影响，但在难度突变时会使收敛略微放缓。

**评估**: 非安全漏洞，但设计差异值得记录。

### VULN-7: 挖矿端 `GetMinimumTime()` 部分实现BIP94

**严重度**: 🟡 LOW  
**文件**: `src/node/miner.cpp:46-48`

```cpp
if (height % difficulty_adjustment_interval == 0) {  // 仅每100块
    min_time = std::max(min_time, pindexPrev->GetBlockTime() - MAX_TIMEWARP);
}
```

**评估**: 验证端(`validation.cpp:4118`)已每块检查timewarp，且`difficulty_adjustment_interval`对每块DAA意义不大。实际攻击面已被封堵。

---

## 🟢 已验证安全项

### ✅ PoW/难度

| 检查项 | 状态 | 文件 |
|--------|------|------|
| `fPowNoRetargeting=false` | ✅ | `chainparams.cpp:84` |
| `powLimit=0x7fff...ff` (标准值) | ✅ | `chainparams.cpp:78` |
| `CheckProofOfWork` 完整验证(nBits+nTime+hash) | ✅ | `pow.cpp:106-136` |
| `ContextualCheckBlockHeader` BIP94 每块检查 | ✅ | `validation.cpp:4118-4126` |
| MedianTimePast时间戳检查 | ✅ | `validation.cpp:4113` |
| `PermittedDifficultyTransition` 4x限制被headerssync使用 | ✅ | `pow.cpp:39-68` |
| 创世块POW豁免(标准做法) | ✅ | `pow.cpp:108-110` |

### ✅ 通胀/增发

| 检查项 | 状态 | 文件 |
|--------|------|------|
| `bad-cb-amount` 检查(coinbase不超补贴+费用) | ✅ | `validation.cpp:2603-2607` |
| `bad-cb-teamwallet` 共识强制(90/10+1%容差) | ✅ | `validation.cpp:2609-2634` |
| 总量上限10亿 TKNC | ✅ | `emission.h:20` |
| 9年排放截止 | ✅ | `emission.h:22-23` |
| `MAX_MONEY` 与 `int64_t` 边界安全(余量98倍) | ✅ | `amount.h` |
| COIN翻倍Bug已修复 | ✅ | `miner.cpp:452-457` |

### ✅ 双花/重org

| 检查项 | 状态 | 文件 |
|--------|------|------|
| `FindMostWorkChain` 基于`nChainWork`选链 | ✅ | `validation.cpp:3135-3185` |
| `InvalidateBlock` 强制断开+标记失败 | ✅ | `validation.cpp:3535-3704` |
| `MaybeUpdateMempoolForReorg` 重组后恢复交易 | ✅ | `validation.cpp:299-393` |
| `GetConflictTx` 通过`mapNextTx`检测双花 | ✅ | `txmempool.cpp:689-693` |
| RBF 5条规则(BIP125) | ✅ | `policy/rbf.cpp:58-140` |
| TRUC(BIP431)禁止兄弟冲突 | ✅ | `policy/truc_policy.cpp` |
| 钱包层`MarkConflicted`递归标记 | ✅ | `wallet.cpp:1365-1393` |
| 孤立交易DoS评分驱逐 | ✅ | `txorphanage.cpp:155-188` |

### ✅ 交易脚本

| 检查项 | 状态 | 文件 |
|--------|------|------|
| P2SH sigpush-only共识强制 | ✅ | `interpreter.cpp:2055` |
| Witness 可锻性检测 | ✅ | `interpreter.cpp:2036-2048` |
| CLEANSTACK+WITNESS | ✅ | `interpreter.cpp:2100-2108` |
| MAX_SCRIPT_SIZE / MAX_STACK_SIZE | ✅ | `script.h:28-43` |
| DERSIG 强制 | ✅ | `policy.h` MANDATORY flags |

---

## 📋 修复优先级

| 优先级 | 编号 | 问题 | 修复难度 |
|--------|------|------|----------|
| 🔴 P0 | VULN-1 | 外部矿工遗漏交易费 | 中 — 需要改getblocktemplate响应解析 |
| 🔴 P1 | VULN-2 | SIGPUSHONLY不在MANDATORY | 低 — 添加一个flag |
| 🟡 P2 | VULN-3~5 | CLTV/CSV/AccessCoin/BIP68 | 低 — 当前已通过参数缓解 |

---

## 🛡️ 总体安全评级

| 维度 | 评级 | 说明 |
|------|------|------|
| PoW/难度 | **A** | 多轮修复后到达稳定状态，动态DAA工作正常 |
| 通胀保护 | **B+** | 核心正确，矿工交易费遗漏需修复 |
| 双花防护 | **A** | 继承Bitcoin Core 28.x成熟方案 |
| 脚本验证 | **A-** | MANDATORY flags少一个SIGPUSHONLY |
| 重组保护 | **A** | FindMostWorkChain+InvalidateBlock完整 |
| 网络层 | **B+** | 继承BTC网络栈，矿工签名TODO未完成 |

**总体**: **B+ 级别** — 共识核心安全，2个需要修复的问题，无致命漏洞。

---

*审计基于Bitcoin Core 28.x安全最佳实践，覆盖OWASP Blockchain Top 10攻击向量*
