# TKNC 推理付费完整指南

> **版本**：v1.0 | **更新日期**：2026-06-23 | **适用范围**：TKNC mainnet

> ⚠️ **架构变更声明（2026-06-29）**
>
> 本文档中关于 **Escrow 预付锁仓模型**（§3.3、§7.3b、§14 等）的描述已**废弃**。
> 新架构遵循 `TKNC使用说明书.md` §5.3 步骤4 + 铁律2：
>
> - **废弃 Escrow 预付锁仓** — 资金不再停留在 Escrow UTXO
> - **废弃 Web 端 locked_balance / refundable_balance / refunded** — Web 服务器纯黄页展示
> - **废弃 Web 端 /api/wallet/refund 端点** — 先用后付模式下不存在退款需求
> - **废弃 Web 端 createAPIKey 中的 on-chain payment** — Web 只发凭证，不碰资金
> - **真正先用后付**：推理先发生 → 客户节点本地累记 → 每满 1 TKNC → 付费钱包 TransferTo 矿工钱包 → 链上确认
> - **资金从不曾停留在任何中间位置**（节点 DB / 服务器 / 合约）
>
> 本文档保留作为历史参考。**权威架构以 `TKNC使用说明书.md` 为准**。

---

## 目录

1. [概述](#1-概述)
2. [系统架构](#2-系统架构)
3. [核心概念](#3-核心概念)
4. [完整11步流程](#4-完整11步流程)
5. [步骤1：矿工设定兑换比例](#5-步骤1矿工设定兑换比例)
6. [步骤2：客户查看兑换比例](#6-步骤2客户查看兑换比例)
7. [步骤3：创建API Key与Escrow](#7-步骤3创建api-key与escrow)
8. [步骤4：设定付费钱包密码](#8-步骤4设定付费钱包密码)
9. [步骤5：解锁付费钱包](#9-步骤5解锁付费钱包)
10. [步骤6：充值到付费钱包](#10-步骤6充值到付费钱包)
11. [步骤7：发起推理请求](#11-步骤7发起推理请求)
12. [步骤8：握手校验](#12-步骤8握手校验)
13. [步骤9：矿工执行推理](#13-步骤9矿工执行推理)
14. [步骤10：先用后付自动转账](#14-步骤10先用后付自动转账)
15. [步骤11：停止推理并提现](#15-步骤11停止推理并提现)
16. [RPC命令速查表](#16-rpc命令速查表)
17. [HTTP API速查表](#17-http-api速查表)
18. [异常处理与常见错误](#18-异常处理与常见错误)
19. [资金安全保障](#19-资金安全保障)
20. [附录：测试数据示例](#20-附录测试数据示例)

---

## 1. 概述

TKNC 是一个去中心化的区块链+LLM推理网络。矿工提供GPU算力运行大语言模型，客户通过支付TKNC代币获取推理服务。整个付费流程遵循**先用后付**原则——推理先发生，按真实产出token计费，每累计满1 TKNC自动发起链上转账。

### 核心原则

| 原则 | 说明 |
|------|------|
| **零中转** | 推理数据客户节点↔矿工节点端到端直连，Web服务器仅做黄页展示 |
| **先用后付** | 不预付、不锁仓，推理后按token计费，每满1 TKNC自动链上转账 |
| **握手校验** | 推理前微型推理→token核对→价格验证，双方一致才建立推理链路 |

### 三种角色

| 角色 | 程序 | 职责 |
|------|------|------|
| **矿工** | tknc-miner | 本地GPU计算：PoW挖矿+LLM推理，绑定127.0.0.1:9332 |
| **节点** | tkncd | 对外通信网关、区块链、钱包、Token统计、P2P网络、API Gateway(:9313) |
| **Web服务器** | Express+nginx | 纯黄页展示：矿工列表、价格展示、用户登录，不参与推理/计费 |

---

## 2. 系统架构

```
┌──────────────────────────────────────────────────────────────────┐
│                     TKNC 推理付费架构                              │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌────────────┐     ┌────────────────┐     ┌──────────────┐    │
│  │  Web服务器  │     │     节点        │     │    矿工       │    │
│  │ (种子服务器)│◄───►│   (tkncd)      │◄───►│ (tknc-miner) │    │
│  │            │     │                │     │              │    │
│  │ ·纯黄页展示│     │ ★API Gateway   │     │ ·仅本地计算   │    │
│  │ ·矿工发现  │     │   (:9313)      │     │ ·GPU推理     │    │
│  │ ·价格展示  │     │ ·Token统计     │     │ ·PoW挖矿     │    │
│  │ ·用户登录  │     │ ·付费钱包管理   │     │ ·127.0.0.1   │    │
│  │            │     │ ·Escrow管理    │     │   :9332      │    │
│  └────────────┘     │ ·P2P网络       │     └──────────────┘    │
│                     │ ·RPC(:9331)    │                          │
│  ❌不碰推理/计费/资金│ └────────────────┘                          │
│                                                                  │
│  端口说明：                                                       │
│  · 9313 = 节点API Gateway（OpenAI兼容HTTP API）                   │
│  · 9331 = 节点RPC（JSON-RPC调用）                                 │
│  · 9333 = 节点P2P（节点间通信）                                   │
│  · 9332 = 矿工API（仅127.0.0.1，绝不对外）                        │
│  · 80   = Web服务器（黄页展示，不提供推理API）                     │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. 核心概念

### 3.1 兑换比例（Exchange Rate）

矿工通过 `setminerprice` RPC 设定自己的兑换比例，例如：

- `100 TKNC = 1,000,000 tokens` → 即每1 TKNC可获取10,000 tokens
- `50 TKNC = 1,000,000 tokens` → 即每1 TKNC可获取20,000 tokens（更便宜）

**计算公式**：
```
tokens_per_tknc = 1,000,000 / price_per_1m_tknc
rate_tknc_per_token = COIN / tokens_per_tknc
```

### 3.2 API Key

API Key 是客户节点的消费凭证，格式为 `tknc_` + 32位十六进制字符（如 `tknc_4d068389807307dff9f3938212ad49c7`）。

- 通过 `tknc_createapikey` RPC 创建
- 绑定消费上限（balance）
- 存储在节点LevelDB中，通过P2P同步到全网
- 推理请求必须携带API Key

### 3.3 Escrow（消费限额管理）

Escrow 记录客户与矿工之间的消费关系和限额：

| 字段 | 说明 |
|------|------|
| escrow_id | 唯一标识，格式 `escrow_{apikey片段}_{随机hex}` |
| user_wallet | 客户钱包地址 |
| miner_wallet | 矿工钱包地址 |
| total_tknc | 总消费限额 |
| consumed_tknc | 已消费金额 |
| spending_limit | 剩余可消费金额 = total_tknc - consumed_tknc |
| quota_tokens | 总token配额 |
| used_tokens | 已使用token数 |
| rate_tokens_per_tknc | 兑换比例 |
| state | 状态：CREATED / ACTIVE / EXHAUSTED / SUSPENDED / PAYMENT_PENDING / CLOSED |

**Escrow状态转换**：
```
CREATED → ACTIVE（首次消费后自动激活）
ACTIVE → EXHAUSTED（消费达上限）
ACTIVE → PAYMENT_PENDING（付费钱包转账失败）
ACTIVE → SUSPENDED（矿工离线时暂停）
任意 → CLOSED（Escrow过期后关闭，90天有效期）
```

### 3.4 付费钱包（Payment Wallet）

付费钱包是一个独立的热钱包，用于推理过程中的自动转账：

```
主钱包（冷钱包）──fundpaymentwallet──→ 付费钱包（热钱包）
                                        │
                                        ├── TransferTo → 矿工钱包（每满1 TKNC自动）
                                        │
                                        └── withdrawpaymentwallet → 主钱包（推理结束后）
```

**付费钱包安全模型**：
- 主钱包私钥从不进入节点进程
- 付费钱包是独立CWallet实例，有独立密码和锁定机制
- 解锁后才能执行转账（TransferTo）和提现（Withdraw）
- 支持超时自动锁定（默认300秒）

### 3.5 先用后付计费

```
推理中: 客户节点本地累记消费
  0.3 TKNC → 0.6 TKNC → 0.9 TKNC → 1.0 TKNC
                                              ↓
                              满1 TKNC → 付费钱包.TransferTo(矿工钱包, 1 TKNC)
                              → 链上交易确认 → 矿工收到TKNC
                                              ↓
                              继续累记: 0.2 → 0.5 → 0.8 → 1.0
                                              ↓
                              再满1 TKNC → 再次链上转账
```

**关键特性**：
- 资金从不曾停留在任何中间位置（节点DB/服务器/合约）
- 服务器崩溃不影响已上链交易
- 未上链消费最多损失 <1 TKNC

---

## 4. 完整11步流程

```
步骤1:  矿工登录WEB → 设定兑换比例（如100TKNC=1M token）
        ↓ setminerprice RPC

步骤2:  客户登录WEB → 找到矿工 → 查看兑换比例
        ↓ getminerprice RPC

步骤3:  客户在节点创建API Key（指定消费上限，如100 TKNC）
        ↓ tknc_createapikey + createescrow RPC

步骤4:  客户设定付费钱包密码
        ↓ setpaymentwalletpassword RPC

步骤5:  客户解锁付费钱包
        ↓ unlockpaymentwallet RPC

步骤6:  客户从主钱包充值到付费钱包
        ↓ fundpaymentwallet RPC

步骤7:  客户节点发起推理请求（携带API Key）
        ↓ 节点验证API Key + 检查付费钱包余额 + 检查锁定状态

步骤8:  握手校验（微型推理→token核对→价格验证→客户确认）
        ↓ /v1/chat/handshake

步骤9:  矿工执行推理 → 返回结果+节点统计token数
        ↓ /v1/chat/completions

步骤10: 双方节点核对累记消费 → 每满1TKNC → 付费钱包转账给矿工
        ↓ TransferTo (需密码解锁)

步骤11: 停止推理 → 付费钱包剩余资金转回主钱包
        ↓ withdrawpaymentwallet RPC (需密码解锁)
```

---

## 5. 步骤1：矿工设定兑换比例

### 功能说明

矿工通过 `setminerprice` RPC 设定自己的token兑换比例。价格存储在节点LevelDB中，用于握手校验和计费计算。矿工也可以通过Web页面设定价格（调用相同RPC）。

### RPC命令

**方法名**：`setminerprice`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| miner_wallet | string | 是 | 矿工钱包地址 |
| price_per_1m_tknc | number | 是 | 价格：每100万tokens的TKNC数量（必须>0） |

**调用示例**：

```powershell
# PowerShell (JSON-RPC)
$cred = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("tkncadmin:tkncpass123"))
$headers = @{Authorization="Basic $cred"}
$body = '{"jsonrpc":"1.0","id":"1","method":"setminerprice","params":["token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y",100]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

```bash
# tknc-cli
tknc-cli -rpcuser=tkncadmin -rpcpassword=tkncpass123 setminerprice "token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y" 100
```

**返回值**：

```json
{
  "miner_wallet": "token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y",
  "price_per_1m_tknc": 100,
  "tokens_per_tknc": 10000,
  "status": "success"
}
```

| 返回字段 | 说明 |
|---------|------|
| miner_wallet | 矿工钱包地址 |
| price_per_1m_tknc | 设定的价格（TKNC/1M tokens） |
| tokens_per_tknc | 计算得出：每1 TKNC对应的token数 |
| status | 操作状态 |

### 兑换比例示例

| 设定价格 | 含义 | tokens_per_tknc |
|---------|------|----------------|
| 100 | 100 TKNC = 1M tokens | 10,000 |
| 50 | 50 TKNC = 1M tokens | 20,000 |
| 10 | 10 TKNC = 1M tokens | 100,000 |
| 1 | 1 TKNC = 1M tokens | 1,000,000 |

### 注意事项

- 价格持久化存储在LevelDB中，节点重启后不丢失
- 价格修改后立即生效，影响后续所有推理请求的计费
- tokens_per_tknc 范围限制在 [1000, 1000000]

---

## 6. 步骤2：客户查看兑换比例

### 功能说明

客户通过 `getminerprice` RPC 查看矿工的兑换比例。也可通过Web黄页页面查看（仅展示参考值，权威价格来自握手校验）。

### RPC命令

**方法名**：`getminerprice`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| miner_wallet | string | 否 | 矿工钱包地址（省略则返回第一个注册矿工的价格） |

**调用示例**：

```powershell
# 查看指定矿工价格
$body = '{"jsonrpc":"1.0","id":"1","method":"getminerprice","params":["token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# 查看第一个注册矿工价格（省略参数）
$body = '{"jsonrpc":"1.0","id":"1","method":"getminerprice","params":[]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

**返回值**：

```json
{
  "miner_wallet": "token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y",
  "price_per_1m_tknc": 100,
  "tokens_per_tknc": 10000,
  "exchange_rate_display": "100 TKNC = 1M tokens"
}
```

| 返回字段 | 说明 |
|---------|------|
| miner_wallet | 矿工钱包地址 |
| price_per_1m_tknc | 矿工定价（TKNC/1M tokens） |
| tokens_per_tknc | 每1 TKNC对应的token数 |
| exchange_rate_display | 人类可读的兑换比例 |

### 注意事项

- 如果没有矿工设定过价格，返回默认值 10 TKNC/1M tokens
- Web黄页展示的价格仅供参考，权威价格在握手校验中获取

---

## 7. 步骤3：创建API Key与Escrow

### 功能说明

客户需要创建API Key作为推理请求的凭证，同时创建Escrow记录消费限额和兑换比例。API Key存储在LevelDB中并通过P2P同步到全网节点。

### 3a. 创建API Key

**方法名**：`tknc_createapikey`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| balance | number | 是 | 预付余额（TKNC），即消费上限 |
| model_name | string | 否 | 可用模型名称（默认"all"） |
| expiry_days | number | 否 | 有效天数（默认365） |

**调用示例**：

```powershell
# 创建100 TKNC消费上限的API Key
$body = '{"jsonrpc":"1.0","id":"1","method":"tknc_createapikey","params":[100]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

**返回值**：

```json
{
  "api_key": "tknc_4d068389807307dff9f3938212ad49c7",
  "miner_id": "...",
  "balance": 100.00000000,
  "expiry_time": 1783976324,
  "model_name": "all",
  "synced_peers": 2
}
```

| 返回字段 | 说明 |
|---------|------|
| api_key | 生成的API Key（tknc_ + 32位hex） |
| balance | 预付余额 |
| expiry_time | 过期时间（Unix时间戳） |
| synced_peers | P2P同步到的节点数 |

### 3b. 创建Escrow

**方法名**：`createescrow`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| user_wallet | string | 是 | 客户钱包地址（付款方） |
| miner_wallet | string | 是 | 矿工钱包地址（收款方） |
| amount_tknc | number | 是 | 消费限额（TKNC） |
| rate_tokens_per_tknc | number | 是 | 兑换比例：每1 TKNC对应的token数 |
| model_name | string | 是 | 目标模型名称 |
| model_hash | string | 否 | 模型GGUF文件的SHA256哈希 |
| api_key | string | 是 | 关联的API Key |

**调用示例**：

```powershell
# 创建Escrow：100 TKNC，10000 tokens/TKNC，模型qwen2.5-0.5b-instruct
$body = @{
    jsonrpc = "1.0"
    id = "1"
    method = "createescrow"
    params = @(
        "token1qndjdpfh4s6zdk7y2mm22zg2nxkrls3m9p3045p",  # user_wallet
        "token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y",  # miner_wallet
        100,                                               # amount_tknc
        10000,                                             # rate_tokens_per_tknc
        "qwen2.5-0.5b-instruct",                          # model_name
        "",                                                # model_hash (optional)
        "tknc_4d068389807307dff9f3938212ad49c7"           # api_key
    )
} | ConvertTo-Json -Depth 5
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

**返回值**：

```json
{
  "escrow_id": "escrow_f9f3938212ad49c7_055ce0a8",
  "snapshot_hash": "e239ebc5a3dd7f240338dd7a05dcf828030c709eda1eb228e61863ed0d5de1e2",
  "total_tknc": 100.00000000,
  "quota_tokens": 1000000,
  "state": "CREATED"
}
```

| 返回字段 | 说明 |
|---------|------|
| escrow_id | Escrow唯一标识 |
| snapshot_hash | 定价快照的SHA256哈希（防篡改） |
| total_tknc | 总消费限额 |
| quota_tokens | 总token配额 = total_tknc × rate_tokens_per_tknc |
| state | 初始状态为CREATED |

### 查询Escrow信息

```powershell
# 查看Escrow详情
$body = '{"jsonrpc":"1.0","id":"1","method":"getescrowinfo","params":["escrow_f9f3938212ad49c7_055ce0a8"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# 列出所有Escrow
$body = '{"jsonrpc":"1.0","id":"1","method":"listescrows","params":[]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

---

## 8. 步骤4：设定付费钱包密码

### 功能说明

首次使用付费钱包前必须设定密码。密码用于保护付费钱包的转账操作——所有转账（TransferTo、Withdraw）都需要先解锁钱包。

### RPC命令

**方法名**：`setpaymentwalletpassword`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| old_password | string | 是 | 当前密码（首次设定传空字符串""） |
| new_password | string | 是 | 新密码 |

**调用示例**：

```powershell
# 首次设定密码
$body = '{"jsonrpc":"1.0","id":"1","method":"setpaymentwalletpassword","params":["","test1234"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# 修改密码（需要提供旧密码）
$body = '{"jsonrpc":"1.0","id":"1","method":"setpaymentwalletpassword","params":["test1234","newpassword5678"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

**返回值**：

```json
{
  "status": "success"
}
```

### 注意事项

- 首次设定：`old_password` 传空字符串 `""`，内部调用 `EncryptWallet`
- 修改密码：提供旧密码，内部调用 `ChangeWalletPassphrase`
- 密码设定后，付费钱包处于锁定状态，需要 `unlockpaymentwallet` 解锁

---

## 9. 步骤5：解锁付费钱包

### 功能说明

解锁付费钱包以允许转账操作。支持超时自动锁定，防止钱包长时间处于解锁状态。

### RPC命令

**方法名**：`unlockpaymentwallet`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| password | string | 是 | 付费钱包密码 |
| timeout | number | 否 | 解锁超时秒数（默认300，0=不自动锁定） |

**调用示例**：

```powershell
# 解锁5分钟后自动锁定（默认）
$body = '{"jsonrpc":"1.0","id":"1","method":"unlockpaymentwallet","params":["test1234"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# 解锁1小时后自动锁定
$body = '{"jsonrpc":"1.0","id":"1","method":"unlockpaymentwallet","params":["test1234",3600]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# 解锁且不自动锁定（需手动lockpaymentwallet）
$body = '{"jsonrpc":"1.0","id":"1","method":"unlockpaymentwallet","params":["test1234",0]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

**返回值**：

```json
{
  "status": "unlocked",
  "timeout_seconds": 300,
  "auto_lock": "enabled"
}
```

| 返回字段 | 说明 |
|---------|------|
| status | 解锁状态（unlocked/failed） |
| timeout_seconds | 超时秒数 |
| auto_lock | 自动锁定状态（enabled/disabled） |

### 手动锁定

```powershell
$body = '{"jsonrpc":"1.0","id":"1","method":"lockpaymentwallet","params":[]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

### 注意事项

- 付费钱包锁定时，推理中的自动转账（TransferTo）会失败，Escrow会被标记为PAYMENT_PENDING
- 建议推理期间保持钱包解锁状态
- 长时间推理建议设置较大timeout或timeout=0

---

## 10. 步骤6：充值到付费钱包

### 功能说明

从主钱包（冷钱包）向付费钱包（热钱包）转账TKNC。充值后付费钱包才有余额用于推理付费。

### RPC命令

**方法名**：`fundpaymentwallet`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| amount | number | 是 | 充值金额（TKNC） |

**调用示例**：

```powershell
# 充值100 TKNC到付费钱包
$body = '{"jsonrpc":"1.0","id":"1","method":"fundpaymentwallet","params":[100]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

**返回值**：

```json
{
  "payment_wallet_address": "token1qe6x3kp6tqxms2ppkv3m7s99utckflr94jssled",
  "amount": 100.00000000,
  "txid": "4163108353c1b967e3c61bccad3b311bb3d94b273563d74162d2a6c198f4b99b",
  "status": "sent"
}
```

| 返回字段 | 说明 |
|---------|------|
| payment_wallet_address | 付费钱包收款地址 |
| amount | 充值金额 |
| txid | 链上交易ID |
| status | 交易状态 |

### 注意事项

- 主钱包必须已解锁（`walletpassphrase`），否则返回 `RPC_WALLET_UNLOCK_NEEDED`
- 主钱包余额必须充足
- 充值金额建议 ≥ Escrow的total_tknc，确保推理不会因余额不足中断
- 交易费从主钱包额外扣除

---

## 11. 步骤7：发起推理请求

### 功能说明

客户通过节点的API Gateway发起推理请求。请求必须携带API Key，节点会验证Key有效性、Escrow状态、付费钱包余额和锁定状态。

### HTTP API调用

**端点**：`POST /v1/chat/completions`

**请求头**：

```
Authorization: Bearer tknc_4d068389807307dff9f3938212ad49c7
Content-Type: application/json
```

**请求体**：

```json
{
  "model": "qwen2.5-0.5b-instruct",
  "messages": [
    {"role": "user", "content": "What is 2+2? Answer in one sentence."}
  ],
  "stream": false
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| model | string | 是 | 模型名称 |
| messages | array | 是 | 消息列表，每条含role和content |
| stream | boolean | 否 | 是否流式输出（默认false） |

**调用示例**：

```powershell
# 通过节点API Gateway调用（注意：节点9313只监听IPv6！）
$apiKey = "tknc_4d068389807307dff9f3938212ad49c7"
$endpoint = "http://[::1]:9313/v1/chat/completions"
$body = @{
    model = "qwen2.5-0.5b-instruct"
    messages = @(@{role = "user"; content = "What is 2+2?"})
    stream = $false
} | ConvertTo-Json -Depth 5
Invoke-RestMethod -Uri $endpoint -Method Post -Body $body -ContentType "application/json" -Headers @{Authorization = "Bearer $apiKey"}
```

### P2P推理调用

```powershell
# 通过P2P直连矿工节点推理
tknc-cli -rpcuser=tkncadmin -rpcpassword=tkncpass123 p2pinference "qwen2.5-0.5b-instruct" "What is 2+2?" 672
```

### 节点验证流程

```
推理请求到达节点API Gateway
    ↓
1. 验证API Key格式和有效性
    ↓ 无效 → 401 Unauthorized
2. 查找关联的Escrow记录
    ↓ 未找到 → 403 Forbidden
3. 检查Escrow状态（CREATED/ACTIVE才允许）
    ↓ 其他状态 → 403 Forbidden
4. 检查付费钱包余额和锁定状态
    ↓ 锁定或余额不足 → 推理后TransferTo失败 → PAYMENT_PENDING
5. 转发请求到本地矿工(127.0.0.1:9332)
    ↓
6. 矿工执行推理 → 返回结果
    ↓
7. 节点统计token → 计费 → 返回OpenAI兼容格式响应
```

---

## 12. 步骤8：握手校验

### 功能说明

推理前的握手校验确保矿工正常工作且定价合理。节点执行一次微型推理，统计token数并验证合理性，返回定价信息供客户确认。

### HTTP API调用

**端点**：`POST /v1/chat/handshake`

**请求头**：

```
Authorization: Bearer tknc_4d068389807307dff9f3938212ad49c7
Content-Type: application/json
```

**调用示例**：

```powershell
$apiKey = "tknc_4d068389807307dff9f3938212ad49c7"
$endpoint = "http://[::1]:9313/v1/chat/handshake"
$body = '{"api_key":"' + $apiKey + '"}'
Invoke-RestMethod -Uri $endpoint -Method Post -Body $body -ContentType "application/json" -Headers @{Authorization = "Bearer $apiKey"}
```

**返回值（成功）**：

```json
{
  "handshake": {
    "token_verification": "verified",
    "node_prompt_tokens": 8,
    "node_completion_tokens": 12,
    "node_total_tokens": 20,
    "test_content_length": 45,
    "token_count_sane": true,
    "verified_price_per_1m_tknc": 100,
    "tokens_per_tknc": 10000,
    "exchange_rate_display": "100 TKNC = 1M tokens",
    "can_proceed": true
  },
  "message": "Token verification passed. Please confirm to start inference at 100 TKNC/1M tokens."
}
```

**返回值（失败）**：

```json
{
  "handshake": {
    "token_verification": "anomaly_detected",
    "token_count_sane": false,
    "can_proceed": false
  },
  "message": "Token verification FAILED. Possible cheating detected. Connection refused."
}
```

| 返回字段 | 说明 |
|---------|------|
| token_verification | token验证结果（verified/anomaly_detected） |
| node_prompt_tokens | 微型推理的输入token数 |
| node_completion_tokens | 微型推理的输出token数 |
| node_total_tokens | 节点独立计算的token总数 |
| token_count_sane | token计数是否合理 |
| verified_price_per_1m_tknc | 握手校验获取的矿工真实定价 |
| tokens_per_tknc | 每1 TKNC对应的token数 |
| can_proceed | 是否可以继续推理 |

### Token合理性验证规则

| 规则 | 阈值 | 说明 |
|------|------|------|
| 总token数上限 | >500 | 短prompt("Hi")返回>500 token视为异常 |
| 字符/token比 | 0.1~20 | 超出范围视为异常 |

### 注意事项

- 握手校验通过（can_proceed=true）后，客户确认价格即可调用 `/v1/chat/completions`
- 握手失败返回HTTP 403，禁止推理
- 握手中的价格是矿工真实定价（从节点LevelDB获取），不依赖Web展示

---

## 13. 步骤9：矿工执行推理

### 功能说明

握手校验通过后，客户正式发起推理请求。矿工执行LLM推理，节点独立统计token数并计费。

### 推理响应格式

**成功响应**（OpenAI兼容格式）：

```json
{
  "id": "chatcmpl-xxx",
  "object": "chat.completion",
  "model": "qwen2.5-0.5b-instruct",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "2+2 equals 4."
    },
    "finish_reason": "stop"
  }],
  "usage": {
    "prompt_tokens": 20,
    "completion_tokens": 6,
    "total_tokens": 26
  }
}
```

### 计费过程

```
推理请求到达 → 节点转发到矿工(127.0.0.1:9332)
    ↓
矿工执行推理 → 返回prompt_tokens + completion_tokens
    ↓
节点独立计算: tokens_used = prompt_tokens + completion_tokens
    ↓
节点调用 CheckAndDeductEscrow():
  · 计算费用: cost = tokens_used × rate_tknc_per_token
  · 更新Escrow: consumed_tknc += cost, spending_limit -= cost
  · 检查是否跨过1 TKNC边界:
    - 跨过 → 付费钱包.TransferTo(矿工钱包, 1 TKNC)
    - 未跨过 → 继续累记
    ↓
返回推理结果给客户
```

### 矿工模式切换

```
[默认状态] PoW挖矿
    ↓ 收到LLM推理请求
[切换到] LLM推理
    ↓ 推理完成 → 返回结果+prompt_tokens/completion_tokens
[切回] PoW挖矿
```

---

## 14. 步骤10：先用后付自动转账

### 功能说明

推理过程中，节点实时累记消费。每当累计消费跨过1 TKNC整数倍边界时，自动从付费钱包向矿工钱包发起链上转账。

### 自动转账触发逻辑

```
CheckAndDeductEscrow() 内部逻辑:

1. 计算本次推理费用: cost = tokens_used × rate_tknc_per_token
2. 更新Escrow:
   - consumed_tknc += cost
   - spending_limit = total_tknc - consumed_tknc
   - used_tokens += tokens_used
3. 检查1 TKNC边界:
   prev_full_coins = 旧consumed_tknc / COIN
   curr_full_coins = 新consumed_tknc / COIN
4. 如果 curr_full_coins > prev_full_coins:
   → 付费钱包.TransferTo(矿工钱包, (curr - prev) × COIN)
   → 链上交易确认
5. 如果TransferTo失败:
   → Escrow标记为PAYMENT_PENDING
   → 后续推理被阻止
```

### 转账示例

```
推理1: consumed 0.3 TKNC → 未满1，不上链
推理2: consumed 0.8 TKNC → 未满1，不上链
推理3: consumed 1.2 TKNC → 跨过1 TKNC边界！
  → TransferTo(矿工钱包, 1 TKNC)
  → txid = 7f02f68e8ca9dd6b9ffb92a23aabd672215e0b56d1595f355ca8fadece81598c
  → 链上确认，矿工收到1 TKNC
推理4: consumed 0.5 TKNC → 未满1，不上链（从0.2继续累记）
推理5: consumed 1.1 TKNC → 再次跨过1 TKNC边界！
  → TransferTo(矿工钱包, 1 TKNC)
  → 再次链上转账
```

### 转账失败处理

| 失败原因 | 处理 |
|---------|------|
| 付费钱包锁定 | Escrow标记PAYMENT_PENDING，推理被阻止 |
| 付费钱包余额不足 | 同上 |
| TransferTo异常 | 同上 |

**恢复方法**：
1. 解锁付费钱包：`unlockpaymentwallet "password"`
2. 充值付费钱包：`fundpaymentwallet 50`
3. Escrow自动恢复ACTIVE状态

---

## 15. 步骤11：停止推理并提现

### 功能说明

推理结束后，将付费钱包中的剩余资金转回主钱包。需要先解锁付费钱包。

### RPC命令

**方法名**：`withdrawpaymentwallet`

**参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| main_wallet_address | string | 是 | 主钱包地址（接收提现资金） |
| amount | number | 否 | 提现金额（0或省略=提现全部） |

**调用示例**：

```powershell
# 先解锁付费钱包
$body = '{"jsonrpc":"1.0","id":"1","method":"unlockpaymentwallet","params":["test1234"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# 提现全部剩余资金到主钱包
$body = '{"jsonrpc":"1.0","id":"1","method":"withdrawpaymentwallet","params":["token1qndjdpfh4s6zdk7y2mm22zg2nxkrls3m9p3045p"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# 提现指定金额到主钱包
$body = '{"jsonrpc":"1.0","id":"1","method":"withdrawpaymentwallet","params":["token1qndjdpfh4s6zdk7y2mm22zg2nxkrls3m9p3045p",50]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

**返回值**：

```json
{
  "txid": "97f367217a2acacf4a478ab7143f7023e7dc8971bc80e3c908549cf26fbe836b",
  "status": "success"
}
```

| 返回字段 | 说明 |
|---------|------|
| txid | 链上交易ID |
| status | 操作状态（success/failed） |

### 注意事项

- 付费钱包必须已解锁，否则返回 `Payment wallet is locked`
- 提现全部时，交易费从提现金额中扣除（subtract_fee=true）
- 提现指定金额时，交易费额外从付费钱包余额扣除
- 提现后付费钱包余额归零（全部提现时）

---

## 16. RPC命令速查表

### 付费钱包相关

| 命令 | 参数 | 说明 |
|------|------|------|
| `setpaymentwalletpassword` | old_password, new_password | 设定/修改付费钱包密码 |
| `unlockpaymentwallet` | password, [timeout] | 解锁付费钱包（默认300秒超时） |
| `lockpaymentwallet` | (无) | 手动锁定付费钱包 |
| `fundpaymentwallet` | amount | 从主钱包充值到付费钱包 |
| `withdrawpaymentwallet` | main_address, [amount] | 付费钱包提现到主钱包 |

### 定价相关

| 命令 | 参数 | 说明 |
|------|------|------|
| `setminerprice` | miner_wallet, price_per_1m_tknc | 设定矿工兑换比例 |
| `getminerprice` | [miner_wallet] | 查看矿工兑换比例 |

### Escrow相关

| 命令 | 参数 | 说明 |
|------|------|------|
| `createescrow` | user_wallet, miner_wallet, amount, rate, model, [hash], api_key | 创建Escrow |
| `getescrowinfo` | escrow_id | 查看Escrow详情 |
| `listescrows` | (无) | 列出所有Escrow |
| `suspendescrow` | escrow_id | 暂停Escrow |
| `closeescrow` | escrow_id | 关闭过期Escrow |
| `commitpricing` | rate, miner_wallet, user_wallet, amount, model, [hash] | 提交定价到链上OP_RETURN |

### API Key相关

| 命令 | 参数 | 说明 |
|------|------|------|
| `tknc_createapikey` | balance, [model_name], [expiry_days] | 创建API Key |
| `tknc_validateapikey` | api_key | 验证API Key |

### 通用JSON-RPC调用模板

```powershell
$cred = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("tkncadmin:tkncpass123"))
$headers = @{Authorization="Basic $cred"}
$body = '{"jsonrpc":"1.0","id":"1","method":"方法名","params":[参数列表]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

---

## 17. HTTP API速查表

### 节点API Gateway（:9313）

| 方法 | 端点 | 认证 | 说明 |
|------|------|------|------|
| POST | /v1/chat/completions | Bearer Token (api_key) | LLM推理调用（OpenAI兼容） |
| POST | /v1/chat/handshake | Bearer Token (api_key) | 推理前握手校验 |
| GET | /v1/models | Bearer Token | 可用模型列表 |

### Web服务器（种子服务器:80）

| 方法 | 端点 | 认证 | 说明 |
|------|------|------|------|
| POST | /api/login/init | 无 | 登录nonce获取 |
| POST | /api/login/verify | 签名 | 登录验证 |
| GET | /api/miners | 无 | 在线矿工列表 |
| POST | /api/create_key | session+csrf | 创建API Key |
| POST | /api/miners/set-price | session+csrf | 设置矿工价格 |

### 调用示例

```powershell
# 推理请求
$apiKey = "tknc_xxx"
$endpoint = "http://[::1]:9313/v1/chat/completions"
$body = '{"model":"qwen2.5-0.5b-instruct","messages":[{"role":"user","content":"Hello"}],"stream":false}'
Invoke-RestMethod -Uri $endpoint -Method Post -Body $body -ContentType "application/json" -Headers @{Authorization="Bearer $apiKey"}

# 握手校验
$endpoint = "http://[::1]:9313/v1/chat/handshake"
$body = '{"api_key":"' + $apiKey + '"}'
Invoke-RestMethod -Uri $endpoint -Method Post -Body $body -ContentType "application/json" -Headers @{Authorization="Bearer $apiKey"}
```

---

## 18. 异常处理与常见错误

### RPC错误码

| 错误码 | 含义 | 常见原因 |
|--------|------|---------|
| -32601 | Method not found | RPC方法名拼写错误 |
| -1 | Invalid parameter | 参数类型或值不合法 |
| -4 | Misc error | 付费钱包未初始化/未解锁 |
| -13 | Wallet unlock needed | 主钱包未解锁（fundpaymentwallet需要） |

### 常见错误与解决

| 错误信息 | 原因 | 解决方法 |
|---------|------|---------|
| `Payment wallet not initialized` | 节点未启用付费钱包 | 确认节点启动时包含付费钱包初始化 |
| `Payment wallet is locked` | 付费钱包未解锁 | 调用 `unlockpaymentwallet` |
| `Main wallet is locked` | 主钱包未解锁 | 调用 `walletpassphrase` 解锁主钱包 |
| `Insufficient balance` | 主钱包余额不足 | 先获取TKNC（挖矿/转账） |
| `Escrow not found` | API Key未关联Escrow | 先调用 `createescrow` |
| `API Key invalid or expired` | API Key无效 | 重新创建API Key |
| `No online miner available` | 矿工未注册或离线 | 检查矿工是否启动并连接节点 |
| `Handshake Failed (403)` | token计数异常 | 矿工可能虚报token，拒绝连接 |
| `Payment wallet is LOCKED — cannot transfer` | 推理中付费钱包超时锁定 | 增大unlock timeout或设为0 |

### Escrow状态异常恢复

```
PAYMENT_PENDING 状态:
  原因: TransferTo失败（钱包锁定/余额不足）
  恢复:
    1. unlockpaymentwallet "password"
    2. fundpaymentwallet 50  (如果余额不足)
    3. 下次推理时自动恢复ACTIVE

SUSPENDED 状态:
  原因: 矿工离线
  恢复: 矿工重新上线后自动恢复

EXHAUSTED 状态:
  原因: 消费达上限
  恢复: 创建新的Escrow（新的API Key + createescrow）
```

---

## 19. 资金安全保障

### 资金流向图

```
┌──────────┐  fundpaymentwallet  ┌──────────────┐  TransferTo  ┌──────────┐
│  主钱包   │ ──────────────────→ │   付费钱包    │ ───────────→ │  矿工钱包 │
│ (冷钱包)  │                     │  (热钱包)     │  每满1 TKNC  │          │
│          │ ←────────────────── │              │ ←─────────── │          │
└──────────┘  withdrawpaymentwallet └──────────────┘              └──────────┘
                                          │
                                    链上转账确认
                                    不可篡改
```

### 风险场景与保护

| 风险场景 | 保护机制 | 最大损失 |
|---------|---------|---------|
| 服务器崩溃 | 已上链交易不受影响 | <1 TKNC（未上链部分） |
| 节点损坏 | 主钱包私钥在本地 | <1 TKNC（未上链部分） |
| 矿工中途断线 | 客户节点检测超时 | <1 TKNC（未上链部分） |
| 矿工恶意不推理 | 握手校验失败→拒绝连接 | 0 TKNC |
| 矿工篡改兑换比例 | 握手校验获取真实定价 | 0 TKNC（用户可拒绝） |
| 客户恶意不付款 | 付费钱包自动转账，锁定则阻止推理 | <1 TKNC（矿工损失） |

### 安全原则

1. **主钱包私钥从不进入节点进程** — fundpaymentwallet通过CWallet API操作
2. **付费钱包独立密码保护** — 需要显式解锁才能转账
3. **超时自动锁定** — 防止钱包长时间处于解锁状态
4. **每1 TKNC实时上链** — 不在任何中间位置留存大量资金
5. **Escrow消费限额** — 防止超额消费

---

## 20. 附录：测试数据示例

### 完整测试流程数据（2026-06-22 实测）

| 项目 | 值 |
|------|-----|
| 矿工钱包 | `token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y` |
| 客户主钱包 | `token1qndjdpfh4s6zdk7y2mm22zg2nxkrls3m9p3045p` |
| 付费钱包地址 | `token1qe6x3kp6tqxms2ppkv3m7s99utckflr94jssled` |
| API Key | `tknc_4d068389807307dff9f3938212ad49c7` |
| Escrow ID | `escrow_f9f3938212ad49c7_055ce0a8` |
| 兑换比例 | 100 TKNC = 1M tokens (tokens_per_tknc=10000) |
| 付费钱包密码 | test1234 |
| 充值金额 | 100 TKNC |
| 充值txid | `4163108353c1b967e3c61bccad3b311bb3d94b273563d74162d2a6c198f4b99b` |
| TransferTo txid | `7f02f68e8ca9dd6b9ffb92a23aabd672215e0b56d1595f355ca8fadece81598c` (1 TKNC) |
| Withdraw txid | `97f367217a2acacf4a478ab7143f7023e7dc8971bc80e3c908549cf26fbe836b` |

### Escrow最终状态

```json
{
  "escrow_id": "escrow_f9f3938212ad49c7_055ce0a8",
  "total_tknc": 100.00000000,
  "consumed_tknc": 1.28060000,
  "spending_limit": 98.71940000,
  "quota_tokens": 1000000,
  "used_tokens": 12806,
  "remaining_tokens": 987194,
  "rate_tokens_per_tknc": 10000,
  "state": "ACTIVE"
}
```

### 各步骤执行命令汇总

```powershell
# === 前置：设置认证头 ===
$cred = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("tkncadmin:tkncpass123"))
$headers = @{Authorization="Basic $cred"}

# === 步骤1：矿工设定兑换比例 ===
$body = '{"jsonrpc":"1.0","id":"1","method":"setminerprice","params":["token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y",100]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# === 步骤2：客户查看兑换比例 ===
$body = '{"jsonrpc":"1.0","id":"1","method":"getminerprice","params":[]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# === 步骤3a：创建API Key ===
$body = '{"jsonrpc":"1.0","id":"1","method":"tknc_createapikey","params":[100]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# === 步骤3b：创建Escrow ===
$body = '{"jsonrpc":"1.0","id":"1","method":"createescrow","params":["token1qndjdpfh4s6zdk7y2mm22zg2nxkrls3m9p3045p","token1q6qal0ahf8f3p32frnkmmscuwgxcvcn9sve237y",100,10000,"qwen2.5-0.5b-instruct","","tknc_4d068389807307dff9f3938212ad49c7"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# === 步骤4：设定付费钱包密码 ===
$body = '{"jsonrpc":"1.0","id":"1","method":"setpaymentwalletpassword","params":["","test1234"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# === 步骤5：解锁付费钱包 ===
$body = '{"jsonrpc":"1.0","id":"1","method":"unlockpaymentwallet","params":["test1234",3600]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# === 步骤6：充值到付费钱包 ===
$body = '{"jsonrpc":"1.0","id":"1","method":"fundpaymentwallet","params":[100]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

# === 步骤7+8：握手校验 ===
$apiKey = "tknc_4d068389807307dff9f3938212ad49c7"
Invoke-RestMethod -Uri "http://[::1]:9313/v1/chat/handshake" -Method Post -Body '{"api_key":"tknc_4d068389807307dff9f3938212ad49c7"}' -ContentType "application/json" -Headers @{Authorization="Bearer $apiKey"}

# === 步骤9：执行推理 ===
$body = '{"model":"qwen2.5-0.5b-instruct","messages":[{"role":"user","content":"What is 2+2?"}],"stream":false}'
Invoke-RestMethod -Uri "http://[::1]:9313/v1/chat/completions" -Method Post -Body $body -ContentType "application/json" -Headers @{Authorization="Bearer $apiKey"}

# === 步骤10：自动（每满1 TKNC自动TransferTo，无需手动操作） ===

# === 步骤11：提现到主钱包 ===
$body = '{"jsonrpc":"1.0","id":"1","method":"unlockpaymentwallet","params":["test1234"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers

$body = '{"jsonrpc":"1.0","id":"1","method":"withdrawpaymentwallet","params":["token1qndjdpfh4s6zdk7y2mm22zg2nxkrls3m9p3045p"]}'
Invoke-RestMethod -Uri "http://127.0.0.1:9331" -Method Post -Body $body -ContentType "application/json" -Headers $headers
```

---

*文档结束*
