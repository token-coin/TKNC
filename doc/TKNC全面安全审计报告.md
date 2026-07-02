# TKNC 区块链系统全面安全审计报告

> 审计日期：2026-06-30
> 审计范围：src/ 全部源码、配置文件、脚本、与 CyberChain(CCX) 对比
> 审计方法：4个并行代理覆盖 PoW/共识、排放/奖励、RPC接口、重复逻辑/死代码

---

## 一、审计发现汇总

| 严重程度 | 数量 | 说明 |
|----------|------|------|
| **Critical** | 5 | 可直接导致系统瘫痪/超发/共识分叉 |
| **High** | 12 | 可被利用绕过安全约束或造成损失 |
| **Medium** | 11 | 逻辑缺陷或特定条件下的风险 |
| **Low** | 8 | 代码质量/死代码/潜在隐患 |

---

## 二、Critical 级发现

### C-1: `static cn_v4_hash_t ctx` 线程安全问题 — 可导致共识分叉
- **位置**: `src/pow/tknchash.cpp:35`
- **问题**: `TKNCComputeHashWithTable` 使用 `static cn_v4_hash_t ctx`，其内部 scratchpad（lpad/spad）是可变状态。多线程并发调用（验证+RPC）会导致哈希输出不确定
- **影响**: 同一区块在不同线程上下文可能产生不同哈希 → 合法块被拒绝或非法块被接受 → 共识分叉
- **修复**: 改为 `thread_local cn_v4_hash_t ctx` 或每次创建新实例

### C-2: D3D11 GPU矿工使用与验证完全不同的哈希算法
- **位置**: `src/pow/real_gpu_miner.cpp:70-141` (HLSL shader) vs `src/pow/tknchash.cpp:37` (验证)
- **问题**: D3D11矿工的HLSL kernel是自定义的8xuint32+查表+64轮算法，与验证用的Cryptonight完全不同。GPU搜索的nonce几乎100%被CPU验证拒绝
- **影响**: D3D11矿工完全无法有效挖矿；GPU算力被浪费
- **修复**: 移除D3D11矿工路径，仅使用OpenCL矿工（tokenhash.cl）

### C-3: MAX_MONEY 远小于实际总供应量 — 合法交易被拒绝
- **位置**: `src/consensus/amount.h:26`
- **问题**: `MAX_MONEY = 21000000 * COIN`（Bitcoin值），但TKNC总量是10亿。`MoneyRange()` 会拒绝任何超过2100万TKNC的金额
- **影响**: 持有大量TKNC的地址无法创建合法UTXO；高额交易被拒绝
- **修复**: `MAX_MONEY = 1000000000 * COIN`

### C-4: RPC弱密码 + 公网完全开放
- **位置**: `build/bin/tknc.conf:2-3`, `tknc_root.conf`, `tknc_seed.conf`
- **问题**: 所有配置文件使用 `rpcuser=tkncadmin / rpcpassword=tkncpass123` + `rpcallowip=0.0.0.0/0` + `rpcbind=0.0.0.0`
- **影响**: 任何人可远程通过RPC调用 `stop`、`invalidateblock`、`sendrawtransaction`、所有escrow RPC
- **修复**: 使用随机cookie认证；移除 `rpcallowip=0.0.0.0/0`；默认仅绑定localhost

### C-5: `invalidateblock`/`reconsiderblock`/`preciousblock` 在 mainnet 可用
- **位置**: `src/rpc/blockchain.cpp:1754,1799,1692`
- **问题**: 这三个RPC虽标记为 "hidden" 但仍可调用，且无 `ChainType::REGTEST` 限制。结合C-4弱密码，攻击者可远程操纵链状态
- **影响**: 攻击者可反复 invalidateblock 制造链分叉
- **修复**: 添加 `ChainType::REGTEST` 限制

---

## 三、High 级发现

### H-1: 排放计算使用浮点数迭代 — 共识分叉风险
- **位置**: `src/economics/emission.h:79-108`
- **问题**: `GetTKNCTotalEmitted` 用 `double` 迭代累加，最多230万次。不同编译器/平台的浮点行为不同 → 不同节点算出不同奖励 → 共识分叉
- **附加**: O(n²)性能问题——每个区块验证都要遍历到当前高度
- **修复**: 改用定点整数运算；使用缓存或闭式公式

### H-2: 创世块5000 TKNC未计入排放模型 — 超发
- **位置**: `src/tknc_chainparams.cpp:43` vs `src/economics/emission.h:31-32`
- **问题**: 创世块奖励5000 TKNC，但 `GetTKNCBlockSubsidy(0)` 返回0，`GetTKNCTotalEmitted(0)` 返回0
- **影响**: 实际流通量 = 排放公式总量 + 5000 TKNC，超出声明的10亿总量
- **修复**: 在 `GetTKNCTotalEmitted` 中计入创世块奖励，或创世块奖励改为0

### H-3: Escrow计费整数溢出 — 可极低成本消耗推理资源
- **位置**: `src/rpc/escrow.cpp:729`
- **问题**: `CAmount cost = tokens_used * escrow.rate_tknc_per_token` 无溢出检查。溢出为负数后被设为1 satoshi
- **修复**: 计算前检查 `tokens_used > INT64_MAX / escrow.rate_tknc_per_token`

### H-4: Escrow无链上锁定 — 免费获取推理服务
- **位置**: `src/rpc/escrow.cpp:92-200` (createescrow), `713-820` (CheckAndDeductEscrow)
- **问题**: Escrow只是链下LevelDB记录，不锁定任何TKNC。用户可创建大额escrow → 消耗推理 → 转走TKNC → 支付失败但推理已完成
- **修复**: 实现链上锁定交易（锁定TKNC到escrow地址）

### H-5: Inference Gateway `/api/v1/create_key` 无认证
- **位置**: `src/inference_gateway.cpp:1824, 1672-1728`
- **问题**: 绑定 `0.0.0.0:9313`，`/api/v1/create_key` 端点无任何认证。任何人可创建无限API Key，绕过escrow计费
- **修复**: 添加RPC认证或管理员token验证

### H-6: WebSocket代理无认证 + 自动NAT-PMP
- **位置**: `src/ws_proxy.cpp:241, 143-223, 257-280`
- **问题**: 绑定 `0.0.0.0`，无认证，直接代理到本地矿工 `127.0.0.1:9332`。还自动通过NAT-PMP映射端口到外部
- **修复**: 添加API Key验证；移除自动NAT-PMP

### H-7: 硬编码API Key在源码中
- **位置**: `build/bin/ipv6_proxy.js:91` 等12处
- **问题**: `tknc_9dc37fdce623aff8ee8dcdc3065b5afb` 硬编码在源码和测试文件中
- **修复**: 吊销该Key；从源码中移除所有硬编码密钥

### H-8: `CheckBlockHeader` 绕过 `CheckProofOfWork(header)` 的nBits合法性检查
- **位置**: `src/validation.cpp:3843-3857` vs `src/pow.cpp:106-136`
- **问题**: `CheckBlockHeader` 直接调 `TKNCComputeHash` 而非 `CheckProofOfWork(header)`，跳过了 nTime==0 检查和 nBits 的 fNegative/fOverflow 检查
- **修复**: `CheckBlockHeader` 应调用 `CheckProofOfWork(header, params)`

### H-9: OpenCL矿工64位目标比较，fallback逻辑可导致候选泛滥
- **位置**: `src/pow/opencl_miner.cpp:671-681`
- **问题**: 当 `shift < 192` 时 `target_u64 = ~0ULL`（接受一切），导致海量假阳性涌入CPU验证
- **修复**: 修复fallback逻辑，正确处理高难度target

### H-10: 独立矿工与节点矿工的手续费分配不一致
- **位置**: `src/node/miner.cpp:178` (含手续费) vs `src/miner/miner.cpp:458` (不含手续费)
- **问题**: node/miner.cpp 的 team_reward 包含手续费的10%，但 miner/miner.cpp 的不包含。验证逻辑基于总额比例 → 独立矿工的区块可能被拒绝
- **修复**: 统一两处的手续费分配逻辑

### H-11: Team Wallet验证缺少return — 代码质量问题
- **位置**: `src/validation.cpp:2612-2633`
- **问题**: 多处 `state.Invalid(...)` 后缺少 `return`，虽然最终安全依赖于2643行的检查，但不符合Bitcoin Core模式
- **修复**: 每处 `state.Invalid(...)` 后加 `return false`

### H-12: `submitblock` 无链类型限制 + force_processing — CPU DoS
- **位置**: `src/rpc/mining.cpp:1120-1170`
- **问题**: `submitblock` 在mainnet可用，`force_processing=true` 强制处理。攻击者可提交大量无效区块触发全量验证
- **修复**: 添加速率限制或mainnet额外权限检查

---

## 四、Medium 级发现

| 编号 | 位置 | 问题 |
|------|------|------|
| M-1 | `pow.cpp:72-74,108-110` | Genesis block旁路——`CheckProofOfWork` 在hash==genesisHash时直接返回true |
| M-2 | `pow.cpp:41` | `PermittedDifficultyTransition` 在 `fPowAllowMinDifficultyBlocks=true` 时直接返回true，跳过4x检查 |
| M-3 | `validation.cpp:4133` | testnet无BIP94 timewarp保护（`enforce_BIP94=false`） |
| M-4 | `validation.cpp:2622-2632` | Team wallet验证基于总输出比例而非固定分配，可被额外输出稀释 |
| M-5 | `escrow.h:55` vs `escrow.cpp:740` | `spending_limit` 存储字段与计算值可能不一致 |
| M-6 | `escrow_db.cpp:20-25` | `WriteEscrow`/`EraseEscrow` 不检查数据库操作返回值，始终返回true |
| M-7 | `rpc/escrow.cpp:739-799` | 支付失败后consumed_tknc已扣除但未转账，矿工损失 |
| M-8 | `rpc/mining.cpp:821-839` | `getblocktemplate` 在高度<100时绕过IBD检查，无regtest限制 |
| M-9 | `rpc/escrow.cpp:202-268` | `getescrowinfo`/`listescrows` 返回api_key等敏感信息 |
| M-10 | `rpc/net.cpp:1264,1001` | `sendmsgtopeer`/`addpeeraddress` 无regtest限制 |
| M-11 | `rpc/mining.cpp:153-178` | `GenerateBlock` nonce溢出处理可能导致循环 |

---

## 五、Low 级发现 / 死代码

| 编号 | 位置 | 问题 |
|------|------|------|
| L-1 | `consensus/params.h:89` | `nSubsidyHalvingInterval` 残留Bitcoin参数，从未赋值使用 |
| L-2 | `consensus/amount.h:26` | `MAX_MONEY=21000000*COIN` 是Bitcoin值（见C-3） |
| L-3 | `ipc/capnp/mining.capnp:15` | `maxMoney=2100000000000000` Bitcoin值 |
| L-4 | `test/validation_tests.cpp:24-65` | 旧Bitcoin减半测试残留 |
| L-5 | `pow/cn_slow_hash.hpp:299` | `RYO_USE_SOFTWARE_AES` 环境变量名残留 |
| L-6 | 多个pow/*.cpp | Ryo Currency/Monero/SUMOKOIN 版权头残留 |
| L-7 | `pow/opencl_miner.cpp:347-368` | tokenhash.cl可从外部文件加载，存在kernel替换风险 |
| L-8 | `build/bin/debug_proxy.js` | 记录完整请求体含认证信息 |

---

## 六、重复逻辑发现

### R-1: `cn_slow_hash` 类在两个头文件中完整重复定义
- `src/pow/cn_slow_hash.hpp` (70-360行) vs `src/pow/tokenhash_impl.hpp` (23-139行)
- 两套不兼容的实现，可能导致编译混乱

### R-2: `cn_sptr` 类重复定义
- `cn_slow_hash.hpp:139-171` vs `tokenhash_impl.hpp:52-73`（方法集不同）

### R-3: `hw_check_aes()` / `check_avx2()` 重复定义
- `cn_slow_hash.hpp` vs `tokenhash_impl.hpp`（check_avx2实现不同）

### R-4: `keccakf` 三处独立实现
- `keccak.h` (extern声明) + `tokenhash.cpp:26` (static定义) + `crypto/sha3.cpp:17` (KeccakF大写)

### R-5: `_umul128` 三处完全相同的定义
- `cn_slow_hash_soft.cpp:492` + `hard_intel.cpp:290` + `hard_aarr.cpp:312`

### R-6: 难度计算函数重复
- `pow.cpp` 有 `GetNextWorkRequired` + `CalculateNextWorkRequired`，都调用 `CalculateNextDifficultyTarget`

---

## 七、与 CyberChain(CCX) 安全对比

| 安全维度 | CCX (go-cyberchain) | TKNC | 差距 |
|----------|---------------------|------|------|
| **哈希一致性** | ✓ 矿工=验证=同一fpHash | ✗ D3D11用不同算法；static ctx线程不安全 | TKNC差 |
| **PoW比较空间** | ✓ 完整256位 | ⚠ 8字节有效（后24字节全0） | TKNC差 |
| **难度调整** | ✓ 以太坊式，单步+0.098% | ✓ 4倍clamp（已修复） | 相当 |
| **generatetoaddress** | N/A（geth无此RPC） | ✓ 已限regtest（已修复） | TKNC有额外限制 |
| **RPC安全** | ✓ geth默认cookie认证 | ✗ 弱密码+公网开放 | TKNC差 |
| **链状态RPC** | ✓ 无invalidateblock等 | ✗ mainnet可用invalidateblock | TKNC差 |
| **排放计算** | ✓ 整数运算+右移减半 | ✗ 浮点迭代+O(n²) | TKNC差 |
| **MAX_MONEY** | ✓ 匹配总量 | ✗ 21M（Bitcoin值） | TKNC差 |
| **资金锁定** | N/A（无escrow） | ✗ Escrow无链上锁定 | TKNC独有风险 |
| **推理网关** | N/A | ✗ create_key无认证 | TKNC独有风险 |
| **代码整洁度** | ✓ 基于geth规范 | ✗ 大量重复定义+死代码 | TKNC差 |

---

## 八、修复优先级

### P0 — 立即修复（影响共识/资金安全）
1. **C-1**: `static ctx` → `thread_local`（1行改动，防共识分叉）
2. **C-3**: `MAX_MONEY` → `1000000000 * COIN`（1行改动，防交易被拒）
3. **C-4**: 配置文件移除弱密码和 `rpcallowip=0.0.0.0/0`
4. **C-5**: invalidateblock等添加regtest限制
5. **H-1**: 排放计算改用整数运算

### P1 — 尽快修复（影响安全/功能）
6. **H-3**: Escrow计费添加溢出检查
7. **H-5**: create_key端点添加认证
8. **H-6**: WS代理添加认证+移除NAT-PMP
9. **H-8**: CheckBlockHeader调用CheckProofOfWork(header)

### P2 — 计划修复（代码质量/防御纵深）
10. **C-2**: 移除D3D11矿工或修复HLSL shader
11. **H-4**: 实现Escrow链上锁定
12. **H-9**: 修复OpenCL 64位目标比较fallback
13. **H-10**: 统一手续费分配逻辑
14. 清理重复代码和死代码

---

## 九、结论

TKNC系统存在**5个Critical级**和**12个High级**安全问题。最紧急的是：

1. **线程安全(C-1)** — 1行改动即可修复，但如果不修可能导致共识分叉
2. **MAX_MONEY(C-3)** — 1行改动即可修复，但如果不修大额交易会被拒
3. **RPC安全(C-4/C-5)** — 配置文件修改即可，但如果不修攻击者可远程操控链
4. **排放浮点(H-1)** — 需要重写排放计算，但如果不修可能导致共识分叉

与CCX相比，TKNC在哈希一致性、RPC安全、排放计算、代码整洁度方面均有明显差距。CCX的架构（geth+fpHash）天然具备TKNC缺失的多重防护。

**建议**：先修复P0级的5个问题（改动量小但影响大），再逐步处理P1/P2。
