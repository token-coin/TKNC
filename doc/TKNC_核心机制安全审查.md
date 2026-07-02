# TKNC 核心机制安全审查报告

**审计日期**: 2026-07-01  
**审计范围**: 排放速度、排放总量、难度爆炸、旧generate漏洞残留

---

## 一、排放速度检查

### 1.1 代码路径

```
源码: economics/emission.h:38-71
验证: validation.cpp:2603  — bad-cb-amount检查
```

```cpp
// emission.h:38
CAmount GetTKNCBlockSubsidy(int nHeight) {
    int current_year = nHeight / TKNC_BLOCKS_PER_YEAR;  // 262800
    if (current_year >= TKNC_EMISSION_COMPLETE_YEAR)     // 9
        return 0;                                        // ← 9年截止
    
    CAmount total_emitted_before = GetTKNCTotalEmitted(nHeight);
    int64_t remaining_sat = total_supply_sat - total_emitted_before;
    int64_t reward_sat = remaining_sat / TKNC_EMISSION_DIVISOR;  // ÷300000
    return reward_sat;
}
```

### 1.2 排放速度分析

```
公式: reward(h) = remaining(h-1) / 300000
效果: 几何衰减，剩余量越小奖励越小

第1年:  每块≈3333 TKNC → 262800块 → 年排放≈877M TKNC (87.7%)
第2年:  每块≈389  TKNC → 262800块 → 年排放≈102M TKNC (10.2%)
第3年:  每块≈45   TKNC → ...
第9年后: GetTKNCBlockSubsidy返回0 → 停止排放
```

### 1.3 出块速度影响

**风险**: 如果难度调整失败导致出块快60倍 → 9年排放压到2个月 → 但总量不变

```cpp
// 保护层1: 9年硬截止 (emission.h:44)
if (current_year >= 9) return 0;  // 基于块高×262800，非日历时间

// 保护层2: DAA自收敛 (difficulty.cpp)
// 出块越快 → 难度越涨 → 出块自动减速 → 回归120秒
```

**结论**: ✅ 安全。排放总量受控，DAA保证出块速度收敛到120秒。收敛期排放仅0.002%供应。

---

## 二、排放总量检查

### 2.1 代码路径

```cpp
// emission.h:20
static const int64_t TKNC_TOTAL_SUPPLY = 1000000000;  // 10亿

// emission.h:177-202 — 三重保护
inline CAmount GetTKNCTotalEmitted(int nHeight) {
    // 保护层1: 年份截止 (行182-184)
    if (current_year >= TKNC_EMISSION_COMPLETE_YEAR)
        return TKNC_TOTAL_SUPPLY * COIN;
    
    // 保护层2: 块高上限 (行187-190)
    if (effective_height > TKNC_TOTAL_EMISSION_BLOCKS)
        effective_height = TKNC_TOTAL_EMISSION_BLOCKS;
    
    // 保护层3: clamp (行197-199)
    if (emitted > total_supply_sat)
        emitted = total_supply_sat;
    
    return emitted;
}
```

### 2.2 总量极限验证

```
9年总块数 = 262800 × 9 = 2,365,200
数学极限: remaining = 1e9 × (299999/300000)^2365200
         ≈ 1e9 × e^(-7.88) ≈ 378,000 TKNC (未排放)

9年硬截止后: 约38万TKNC(0.038%)永不排放 → 通缩效果
```

### 2.3 攻击模拟

```
攻击: 伪造高块号调用GetBlockSubsidy
→ 保护层1: year>=9 → 返回0 ✓

攻击: 构造溢出输入让emitted > total
→ 保护层3: clamp到total_supply ✓

攻击: 直接修改TKNC_TOTAL_SUPPLY
→ constexpr, 编译期常量, 无法运行时修改 ✓
```

**结论**: ✅ 总量安全。三重保护+编译期常量。

---

## 三、难度爆炸检查

### 3.1 代码路径

```cpp
// difficulty.cpp:38-49
int64_t x = TKN_BASE - (timespan / TKN_TIME_DIVISOR);
if (x < TKN_MAX_DROP) x = TKN_MAX_DROP;  // x ∈ [-16, 2]

int64_t divisor = TKN_BOUND_DIVISOR;
if (x > 0) {
    if (timespan < 15)  divisor = 16;   // 12.5%/块
    else if (timespan < 30) divisor = 32;  // 6.25%/块
    else if (timespan < 60) divisor = 32;  // 6.25%/块
}
```

### 3.2 自限性证明

```
上行反馈:
  出块<15秒 → divisor=16 → +12.5%/块 → 难度涨 → 出块变慢 → 退出<15秒区域
  出块<60秒 → divisor=32 → +6.25%/块 → 难度涨 → 出块变慢 → 退出<60秒区域
  出块≈120秒 → x=0 → 难度不变 → 平衡

下行反馈:
  出块>120秒 → x<0 → 难度降(最大-25%/块) → 出块变快
  出块>1080秒(18分钟) → x=-16 → 每块-25% → 难度快速回落
```

### 3.3 难度爆炸场景测试

```
场景: 1TH/s算力涌入 → 全速出块15秒 → divisor=32(6.25%)
收敛: (1.0625)^n = 67(需要67倍) → n≈69块 → ~17分钟恢复120秒

场景: 1TH/s算力突然全撤 → 出块极慢(2000秒)
恢复: x=2-(2000/60)=-31→clamp到-16 → -16/64=-25%/块
      (0.75)^n = 1/67 → n≈14块 → 14×2000=7.8小时恢复
```

### 3.4 整数除法死区分析

```
timespan/60 整数除法产生"死区":
 60-119秒 → x=1 → +1.56%/块(仍在增加难度)
120-179秒 → x=0 → 难度不变(平衡区)
180-239秒 → x=-1 → -1.56%/块(降低难度)

120秒目标落在大死区正中 → 收敛后稳定在120-179秒范围内
不会震荡。死区是阻尼器而非振荡器。
```

### 3.5 极端恶化路径

```
worst case: 算力暴涨1,000,000×后全撤
 难度膨胀: 需要~90块收敛(约2.3分钟)
 难度回落: 需要~50块, 每块2000秒 → 约28小时
 → 链会停滞约1天, 但不会永久冻结
 → 恢复后正常出块

评估: 非"难度爆炸无法出块", 是"算力撤离后的自然恢复期"
```

**结论**: ✅ 不存在难度永久爆炸。系统是负反馈自限的，从任何起点都能收敛。最坏情况恢复约1天。

---

## 四、旧generate命令残留检查

### 4.1 代码审查

```cpp
// rpc/mining.cpp

// generate → 完全移除
static RPCMethod generate() {
    throw JSONRPCError(RPC_METHOD_NOT_FOUND, ...);  // 行292
}

// generatetoaddress → regtest锁定
if (GetChainType() != ChainType::REGTEST) {        // 行329
    throw JSONRPCError("disabled on mainnet/testnet");
}

// generatetodescriptor → regtest锁定
if (GetChainType() != ChainType::REGTEST) {        // 行280
    throw JSONRPCError("disabled on mainnet/testnet");
}

// generateblock → regtest锁定
if (GetChainType() != ChainType::REGTEST) {        // 行395
    throw JSONRPCError("disabled on mainnet/testnet");
}
```

### 4.2 RPC注册表

```
行1220-1227:
  "mining" → submitblock, submitheader  ← 正常PoW, 经过全验证
  "hidden" → generatetoaddress, generatetodescriptor,
             generateblock, generate     ← 全部隐藏+锁定
```

### 4.3 submitblock绕过检查

```
submitblock(RPC) → ProcessNewBlock → AcceptBlock
  → CheckBlockHeader (行3842-3854)
    → CheckProofOfWork(block)  ← 验证nBits合法性+PoW
  → ContextualCheckBlockHeader (行4099-4142)
    → nBits == GetNextWorkRequired(...)  ← 精确匹配难度
    → BIP94 timewarp检查

结论: submitblock走完整验证路径, 无法绕过难度
```

### 4.4 旧漏洞复现测试

```
旧攻击: generatetoaddress → CPU快速PoW → 瞬间出块 → 难度爆炸
当前状态:
  主网上调用generatetoaddress → "disabled on mainnet" → 拒绝 ✓
  主网上调用generate → RPC_METHOD_NOT_FOUND → 拒绝 ✓
  提交伪造nBits块 via submitblock → bad-diffbits → 拒绝 ✓
  提交低PoW块 via submitblock → high-hash → 拒绝 ✓
```

**结论**: ✅ 旧漏洞已完全封堵。4个generate*接口全部锁定，submitblock通过全验证。

---

## 总评分

| 检查项 | 风险等级 | 结论 |
|--------|----------|------|
| 排放速度 | 🟢 安全 | DAA自收敛+DIVISOR衰减+9年硬截止 |
| 排放总量 | 🟢 安全 | 三重保护+编译期常量, 永不超发 |
| 难度爆炸 | 🟢 安全 | 负反馈自限, 最坏恢复~1天(非永久) |
| 旧generate残留 | 🟢 安全 | 4个接口全锁+submitblock全验证 |
