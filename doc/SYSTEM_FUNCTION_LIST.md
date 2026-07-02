# TKNC Blockchain - 全系统功能清单

> **生成时间**: 2026-05-16
> **最后更新**: 2026-06-07 (Session 42 - 架构文档对齐用户最新要求：节点获取公网IP/Token配额计费/Web模型交易平台/排放完毕安全维护)
> **当前节点状态**: mainnet, 高度=7↑, 钱包=tknc+miner
> **当前网络模式**: ✅ mainnet (唯一模式)
> **Session 42变更**: ✅ 公网IP职责从矿工转移到节点 | ✅ 新增Token配额自动停止机制 | ✅ Web定位强化为模型交易平台 | ✅ 新增排放完毕后安全维护章节

---

## 总览统计

| 层级 | 组件 | 数量 | 端口/入口 |
|------|------|------|-----------|
| **L1** | CLI JSON-RPC 命令 | **184** | tknc-cli → 9331 |
| **L2** | Web API (C++ libevent) | **22** | HTTP → 9332 |
| **L3** | 后端代理路由 (Express.js) | **33** + WS×2 | HTTP → 80 |
| **L4** | 前端UI交互功能 | **52** | 浏览器 |
| **合计** | **291+** 功能点 | — | — |

---

## 架构数据流（P2P点对点模型 + 节点中转铁律 + 链上锁定定价）

> **⚠️ 核心架构（2026-06-07 更新）**:
> 1. **矿工禁止任何对外通信**（A2.7 铁律）：矿工API仅监听 127.0.0.1:9332
> 2. **公网地址由节点获取/设置**：节点在矿工启动时自动检测（IPv6优先→STUN→UPnP）或手动指定，自动上报Web
> 3. **所有外部请求必须经过节点中转**：用户→Web(80)→种子RPC(p2pinference)→P2P(9333)→目标节点→localhost:9332→矿工
> 4. **Web服务器 = 模型交易平台**：类似淘宝/亚马逊，任何人可发布模型（区块链交易上链）
> 5. **Token配额由目标节点统计**：目标节点累计输出token（矿工不参与token统计），达到链上锁定的付费额度自动停止
> 6. **兑换比例防篡改 = 链上OP_RETURN锁定**：付款时价格写入链上交易，双方从同一笔TX读取（§5.7）
> 7. **种子节点仅做路由**：只运行节点+Web服务，不挖矿/不推理/不中转数据/不计量token（2核2G限制）

```
┌──────────┐   HTTP(80)   ┌──────────────┐  RPC(p2p) ┌──────────────┐
│ 用户浏览器 │────────────▶│ 种子节点/Web  │──────────▶│ 本地种子tkncd │
│          │  登录/支付   │ (模型交易平台) │  路由请求  │ (路由,不计量) │
└──────────┘             └──────────────┘            └──────┬───────┘
                                                             │ P2P(9333)
                                                             ▼
                                                    ┌──────────────────┐
                                                    │  目标节点(tkncd)  │
                                                    │  公网IP:9333可达   │
                                                    └────────┬─────────┘
                                                             │ localhost:9332
                                                             ▼
                                                    ┌──────────────────┐
                                                    │ 矿工(tknc-miner)  │
                                                    │ PoW↔LLM自动切换   │
                                                    └──────────────────┘
                                                             │
                                               目标节点统计token（矿工不参与） → 从链上读取定价快照 → 配额检查 → 达标停止
```

---

## 目录位置规范 (Session 35更新)

> **核心原则**: data/钱包/模型文件夹根据可执行程序位置创建。节点程序在A文件夹，则模型/区块/钱包均在A文件夹下。

```
D:\block\                          # 项目根目录（必须保持整洁！）
│
├── build\                         # 【编译+运行时】可执行程序所在位置
│   ├── bin\Release\              # ✅ tkncd.exe, tknc-miner.exe, tknc-cli.exe
│   ├── data\                     # ✅ 区块链数据 (tknc.conf, blocks/, chainstate/)
│   │   └── tknc.conf            # 节点配置文件
│   └── models\                   # ✅ AI模型文件 (.gguf)
│       └── models\qwen2.5-0.5b-instruct.gguf  (468.6 MB)
│
├── data\                          # 【主数据】区块链持久化备份区（可选）
│   └── mainnet\                  # 主网模式数据副本
│
├── TKNC\                          # 【源码】系统核心源代码
│   ├── src\                      # C++源码 (miner/, node/, pow/, rpc/)
│   ├── web\                      # Web前端 (public/index.html, server.js)
│   └── doc\                      # 文档 (SYSTEM_FUNCTION_LIST.md)
│
└── test\                          # 【测试】临时文件唯一存放地
    ├── *.ps1                     # PowerShell脚本
    ├── *.stdout                  # 运行日志
    └── reports\                 # 测试报告
```

### 关键路径速查表

| 组件 | 路径 | 说明 |
|------|------|------|
| **节点程序** | `build/bin/Release/tkncd.exe` | 主节点，监听9331(RPC)+9333(P2P) |
| **矿工程序** | `build/bin/Release/tknc-miner.exe` | 矿工，API端口**9332(仅127.0.0.1)**，禁止对外通信 |
| **CLI工具** | `build/bin/Release/tknc-cli.exe` | 命令行接口 |
| **节点配置** | `build/data/tknc.conf` | RPC用户名/密码等 |
| **AI模型** | `build/models/models/*.gguf` | LLM推理模型 |
| **区块链数据** | `build/data/` | 运行时自动生成 |
| **Web前端** | `TKNC/web/public/index.html` | 52个UI功能 |
| **Express后端** | `TKNC/web/server.js` | 33条代理路由 |

### 启动命令模板

```powershell
# 启动节点（使用build/data作为数据目录）
D:\block\build\bin\Release\tkncd.exe -datadir=D:\block\build\data -conf=D:\block\build\data\tknc.conf

# 启动矿工（A2零参数启动：只需wallet，其余自动）
D:\block\build\bin\Release\tknc-miner.exe -wallet=token1qh0hyp32j3jff90lvmmgt0rwxelpuf4tuya6grm

# 启动矿工（连接远程节点：只需wallet+rpcconnect，模型自动发现）
D:\block\build\bin\Release\tknc-miner.exe -wallet=token1qh0hyp32j3jff90lvmmgt0rwxelpuf4tuya6grm -rpcconnect=139.196.28.21
```

---

# Part A: CLI JSON-RPC 命令 (173个)

> **入口**: `tknc-cli -rpcport=9331 <command>`  
> **源码目录**: `TKNC/src/rpc/` (18文件) + `TKNC/src/wallet/rpc/` (7文件)

---

## A1. 区块链 [blockchain] — 30个

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `getblockchaininfo` | *(无)* | blockchain.cpp:L1377 |
| 2 | `getchaintxstats` | nblocks, blockhash | blockchain.cpp:L1823 |
| 3 | `getblockstats` | hash_or_height, stats | blockchain.cpp:L1970 |
| 4 | `getbestblockhash` | *(无)* | blockchain.cpp:L278 |
| 5 | `getblockcount` | *(无)* | blockchain.cpp:L256 |
| 6 | `getblock` | blockhash, verbosity | blockchain.cpp:L774 |
| 7 | `getblockfrompeer` | blockhash, peer_id | blockchain.cpp:L523 |
| 8 | `getblockhash` | height | blockchain.cpp:L578 |
| 9 | `getblockheader` | blockhash, verbose | blockchain.cpp:L608 |
| 10 | `getchaintips` | *(无)* | blockchain.cpp:L1591 |
| 11 | `getdifficulty` | *(无)* | blockchain.cpp:L502 |
| 12 | `getdeploymentinfo` | blockhash | blockchain.cpp:L1523 |
| 13 | `gettxout` | txid, n, include_mempool | blockchain.cpp:L1195 |
| 14 | `gettxoutsetinfo` | hash_type, hash_or_height | blockchain.cpp:L1023 |
| 15 | `pruneblockchain` | height | blockchain.cpp:L921 |
| 16 | `verifychain` | checklevel, nblocks | blockchain.cpp:L1275 |
| 17 | `preciousblock` | blockhash | blockchain.cpp:L1690 |
| 18 | `invalidateblock` | blockhash | blockchain.cpp:L1752 |
| 19 | `reconsiderblock` | blockhash | blockchain.cpp:L1797 |
| 20 | `scantxoutset` | action, scan_objects | blockchain.cpp:L2330 |
| 21 | `scanblocks` | action, scan_objects, start_height, stop_height | blockchain.cpp:L2545 |
| 22 | `getdescriptoractivity` | blockhashes, scanobjects | blockchain.cpp:L2734 |
| 23 | `getblockfilter` | blockhash, filtertype | blockchain.cpp:L2970 |
| 24 | `dumptxoutset` | path, type, options | blockchain.cpp:L3075 |
| 25 | `loadtxoutset` | path | blockchain.cpp:L3489 |
| 26 | `getchainstates` | *(无)* | blockchain.cpp:L3583 |
| 27 | `waitfornewblock` | timeout | blockchain.cpp:L299 |
| 28 | `waitforblock` | blockhash, timeout | blockchain.cpp:L358 |
| 29 | `waitforblockheight` | height, timeout | blockchain.cpp:L419 |
| 30 | `syncwithvalidationinterfacequeue` | *(无)* | blockchain.cpp:L482 |

---

## A2. 挖矿 [mining] — 11个

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `getnetworkhashps` | nblocks, height | mining.cpp:L112 |
| 2 | `getmininginfo` | *(无)* | mining.cpp:L422 |
| 3 | `prioritisetransaction` | txid, dummy, fee_delta | mining.cpp:L508 |
| 4 | `getprioritisedtransactions` | *(无)* | mining.cpp:L553 |
| 5 | `getblocktemplate` | template_request | mining.cpp:L621 |
| 6 | `submitblock` | hexdata, dummy | mining.cpp:L1071 |
| 7 | `submitheader` | hexdata | mining.cpp:L1123 |
| 8 | `generatetoaddress` | nblocks, address, maxtries | mining.cpp:L270 |
| 9 | `generatetodescriptor` | num_blocks, descriptor, maxtries | mining.cpp:L225 |
| 10 | `generateblock` | output, transactions, submit | mining.cpp:L311 |
| 11 | `generate` | *(已废弃)* | mining.cpp:L263 |

---

## A3. 网络 [network] — 21个

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `getconnectioncount` | *(无)* | net.cpp:L70 |
| 2 | `ping` | *(无)* | net.cpp:L93 |
| 3 | `getpeerinfo` | *(无)* | net.cpp:L130 |
| 4 | `addnode` | node, command, v2transport | net.cpp:L322 |
| 5 | `addconnection` | address, connection_type | net.cpp:L386 |
| 6 | `disconnectnode` | address, nodeid | net.cpp:L449 |
| 7 | `getaddednodeinfo` | node | net.cpp:L495 |
| 8 | `getnettotals` | *(无)* | net.cpp:L569 |
| 9 | `getnetworkinfo` | *(无)* | net.cpp:L641 |
| 10 | `setban` | subnet, command, bantime | net.cpp:L748 |
| 11 | `listbanned` | *(无)* | net.cpp:L828 |
| 12 | `clearbanned` | *(无)* | net.cpp:L876 |
| 13 | `setnetworkactive` | state | net.cpp:L898 |
| 14 | `getnodeaddresses` | count, network | net.cpp:L920 |
| 15 | `addpeeraddress` | address, port, tried | net.cpp:L981 |
| 16 | `sendmsgtopeer` | peer_id, msg_type, msg | net.cpp:L1124 |
| 17 | `getaddrmaninfo` | *(无)* | net.cpp:L1172 |
| 18 | `exportasmap` | path | net.cpp:L1211 |
| 19 | `getrawaddrman` | *(无)* | net.cpp:L1301 |
| 20 | `p2pinference` | model, prompt, peer_id | net.cpp:L1041 |
| 21 | `setpublicip` | address | net.cpp:L1541 |

---

## A4. 内存池 [mempool] — 16个

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `sendrawtransaction` | hexstring, maxfeerate | mempool.cpp:L47 |
| 2 | `getprivatebroadcastinfo` | *(无)* | mempool.cpp:L142 |
| 3 | `abortprivatebroadcast` | id | mempool.cpp:L209 |
| 4 | `testmempoolaccept` | rawtxs, maxfeerate | mempool.cpp:L264 |
| 5 | `getmempoolfeeratediagram` | *(无)* | mempool.cpp:L615 |
| 6 | `getrawmempool` | verbose, mempool_sequence | mempool.cpp:L658 |
| 7 | `getmempoolancestors` | txid, verbose | mempool.cpp:L709 |
| 8 | `getmempooldescendants` | txid, verbose | mempool.cpp:L770 |
| 9 | `getmempoolcluster` | txid | mempool.cpp:L835 |
| 10 | `getmempoolentry` | txid | mempool.cpp:L870 |
| 11 | `gettxspendingprevout` | outputs, options | mempool.cpp:L903 |
| 12 | `getmempoolinfo` | *(无)* | mempool.cpp:L1075 |
| 13 | `importmempool` | filepath, options | mempool.cpp:L1117 |
| 14 | `savemempool` | *(无)* | mempool.cpp:L1178 |
| 15 | `getorphantxs` | verbosity | mempool.cpp:L1247 |
| 16 | `submitpackage` | package, maxfeerate | mempool.cpp:L1316 |

---

## A5. 原始交易 [rawtransactions] — 15个

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `getrawtransaction` | txid, verbosity, blockhash | rawtransaction.cpp:L216 |
| 2 | `createrawtransaction` | inputs, outputs, locktime | rawtransaction.cpp:L377 |
| 3 | `decoderawtransaction` | hexstring, iswitness | rawtransaction.cpp:L409 |
| 4 | `decodescript` | hexstring | rawtransaction.cpp:L450 |
| 5 | `combinerawtransaction` | txs | rawtransaction.cpp:L585 |
| 6 | `signrawtransactionwithkey` | hexstring, privkeys, prevtxs | rawtransaction.cpp:L671 |
| 7 | `decodepsbt` | psbt | rawtransaction.cpp:L1020 |
| 8 | `combinepsbt` | txs | rawtransaction.cpp:L1522 |
| 9 | `finalizepsbt` | psbt, extract | rawtransaction.cpp:L1570 |
| 10 | `createpsbt` | inputs, outputs, locktime | rawtransaction.cpp:L1627 |
| 11 | `converttopsbt` | hexstring, permitsigdata | rawtransaction.cpp:L1670 |
| 12 | `utxoupdatepsbt` | psbt, descriptors | rawtransaction.cpp:L1738 |
| 13 | `joinpsbts` | txs | rawtransaction.cpp:L1785 |
| 14 | `analyzepsbt` | psbt | rawtransaction.cpp:L1887 |
| 15 | `descriptorprocesspsbt` | psbt, descriptors | rawtransaction.cpp:L1997 |

---

## A6. 钱包 [wallet] — 56个

### A6.1 钱包管理 (11个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `getwalletinfo` | *(无)* | wallet.cpp:L34 |
| 2 | `listwalletdir` | *(无)* | wallet.cpp:L136 |
| 3 | `listwallets` | *(无)* | wallet.cpp:L182 |
| 4 | `loadwallet` | filename, load_on_startup | wallet.cpp:L213 |
| 5 | `unloadwallet` | wallet_name, load_on_startup | wallet.cpp:L434 |
| 6 | `createwallet` | wallet_name, blank, passphrase, descriptors | wallet.cpp:L346 |
| 7 | `createwalletdescriptor` | type, options | wallet.cpp:L745 |
| 8 | `restorewallet` | wallet_name, load_on_startup | wallet.cpp |
| 9 | `backupwallet` | filename | backup.cpp |
| 10 | `migratewallet` | wallet_name, passphrase | wallet.cpp:L582 |
| 11 | `simulaterawtransaction` | rawtxs, options | wallet.cpp:L489 |

### A6.2 地址与标签 (7个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 12 | `getnewaddress` | label, address_type | addresses.cpp:L21 |
| 13 | `getrawchangeaddress` | address_type | addresses.cpp |
| 14 | `getaddressinfo` | address | addresses.cpp:L368 |
| 15 | `getaddressesbylabel` | label | wallet.cpp |
| 16 | `setlabel` | address, label | wallet.cpp |
| 17 | `listlabels` | purpose | wallet.cpp |
| 18 | `listaddressgroupings` | *(无)* | wallet.cpp |

### A6.3 余额与UTXO (5个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 19 | `getbalance` | dummy, minconf, include_watchonly | coins.cpp:L164 |
| 20 | `getbalances` | *(无)* | coins.cpp:L401 |
| 21 | `listunspent` | minconf, maxconf, addresses | coins.cpp:L456 |
| 22 | `getreceivedbyaddress` | address, minconf | wallet.cpp |
| 23 | `getreceivedbylabel` | label, minconf | wallet.cpp |

### A6.4 交易查询 (8个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 24 | `gettransaction` | txid, include_watchonly | wallet.cpp |
| 25 | `listtransactions` | label, count, skip | wallet.cpp |
| 26 | `listsinceblock` | block_label, target_confirmations | wallet.cpp |
| 27 | `listreceivedbyaddress` | minconf, include_empty | wallet.cpp |
| 28 | `listreceivedbylabel` | minconf, include_empty | wallet.cpp |
| 29 | `abandontransaction` | txid | wallet.cpp |
| 30 | `rescanblockchain` | start_height, stop_height | wallet.cpp |
| 31 | `abortrescan` | *(无)* | wallet.cpp |

### A6.5 导入与描述符 (4个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 32 | `importprunedfunds` | rawtransaction, txoutproof | wallet.cpp |
| 33 | `removeprunedfunds` | txid | wallet.cpp |
| 34 | `importdescriptors` | requests | wallet.cpp |
| 35 | `listdescriptors` | active, private | wallet.cpp |
| 36 | `gethdkeys` | options(active_only, private) | wallet.cpp:L641 |

### A6.6 发送/支付 (10个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 37 | `sendtoaddress` | address, amount, comment, ... | spend.cpp:L238 |
| 38 | `sendmany` | dummy, amounts, minconf | spend.cpp:L336 |
| 39 | `send` | outputs, conf_target, estimate_mode | spend.cpp:L1165 |
| 40 | `sendall` | recipients, conf_target | spend.cpp:L1289 |
| 41 | `fundrawtransaction` | hex_string, options | wallet.cpp |
| 42 | `bumpfee` | txid, options | wallet.cpp |
| 43 | `psbtbumpfee` | txid, options | wallet.cpp |
| 44 | `walletcreatefundedpsbt` | inputs, outputs, locktime | wallet.cpp |
| 45 | `signrawtransactionwithwallet` | hexstring, autosign | wallet.cpp |
| 46 | `walletprocesspsbt` | psbt, sign, bip32derivs | wallet.cpp |

### A6.7 加密与签名 (5个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 47 | `encryptwallet` | passphrase | encrypt.cpp:L221 |
| 48 | `walletpassphrase` | passphrase, timeout | encrypt.cpp:L13 |
| 49 | `walletpassphrasechange` | oldpassphrase, newpassphrase | encrypt.cpp:L118 |
| 50 | `walletlock` | *(无)* | encrypt.cpp |
| 51 | `signmessage` | address, message | signmessage.cpp:L14 |

### A6.8 锁定与其他 (6个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 52 | `lockunspent` | unlock, transactions | wallet.cpp |
| 53 | `listlockunspent` | *(无)* | wallet.cpp |
| 54 | `setwalletflag` | flag, value | wallet.cpp:L278 |
| 55 | `keypoolrefill` | newsize | wallet.cpp |
| 56 | `walletdisplayaddress` | address | wallet.cpp |

---

## A7. 节点控制 [control] — 8个

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `getmemoryinfo` | mode ("stats"/"mallocinfo") | node.cpp:L145 |
| 2 | `logging` | include, exclude | node.cpp:L218 |
| 3 | `getindexinfo` | index_name | node.cpp:L363 |
| 4 | `setmocktime` | timestamp | node.cpp:L39 |
| 5 | `mockscheduler` | delta_time | node.cpp:L80 |
| 6 | `echo` | arg | node.cpp:L310 |
| 7 | `echojson` | arg | node.cpp:L311 |
| 8 | `echoipc` | arg | node.cpp:L313 |

---

## A8. 工具类 [util] — 10个

### A8.1 输出脚本 (4个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `validateaddress` | address | output_script.cpp:L29 |
| 2 | `createmultisig` | nrequired, keys, address_type | output_script.cpp:L89 |
| 3 | `getdescriptorinfo` | descriptor | output_script.cpp:L166 |
| 4 | `deriveaddresses` | descriptor, range | output_script.cpp:L258 |

### A8.2 签名验证 (2个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 5 | `verifymessage` | address, signature, message | signmessage.cpp:L17 |
| 6 | `signmessagewithprivkey` | privkey, message | signmessage.cpp:L62 |

### A8.3 手续费估算 (2个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 7 | `estimatesmartfee` | conf_target, estimate_mode | fees.cpp:L32 |
| 8 | `estimaterawfee` | conf_target, threshold | fees.cpp:L97 |

### A8.4 交易证明 (2个)

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 9 | `gettxoutproof` | txids, blockhash | txoutproof.cpp:L23 |
| 10 | `verifytxoutproof` | proof | txoutproof.cpp:L129 |

---

## A9. 外部签名 [external_signer] — 1个

| # | 命令 | 参数 | 源文件 |
|---|------|------|--------|
| 1 | `enumeratesigners` | *(无)* | external_signer.cpp:L20 |

---

## A10. TKNC 扩展 [tknc] — 6个

| # | 命令 | 参数 | 源文件 | 状态 |
|---|------|------|--------|------|
| 1 | `tknc_createapikey` | balance, model_name, expiry_days | tknc_apikey.cpp:L25 | ✅ LevelDB持久化 |
| 2 | `tknc_validateapikey` | api_key | tknc_apikey.cpp:L92 | 待测试 |
| 3 | `tknc_getapikeyinfo` | api_key | tknc_apikey.cpp:L126 | ✅ LevelDB读取 |
| 4 | `tknc_topupapikey` | api_key, amount | tknc_apikey.cpp:L176 | ✅ LevelDB更新 |
| 5 | `tknc_listapikeys` | *(无)* | tknc_apikey.cpp:L234 | ✅ 列表查询 |
| 6 | `tknc_revokeapikey` | api_key | tknc_apikey.cpp:L281 | ✅ 撤销写入 |

---

## A11. Escrow 链上锁仓 [escrow] — 8个 (BATCH3)

| # | 命令 | 参数 | 源文件 | 状态 |
|---|------|------|--------|------|
| 1 | `createescrow` | user_wallet, miner_wallet, amount_tknc, rate_tokens_per_tknc, model_name, model_hash, payment_txid, api_key | escrow.cpp:L39 | ✅ LevelDB持久化 |
| 2 | `releaseescrow` | escrow_id, output_tokens | escrow.cpp:L152 | ✅ 按量释放资金 |
| 3 | `refundescrow` | escrow_id | escrow.cpp:L259 | ✅ 退款到用户钱包 |
| 4 | `suspendescrow` | escrow_id | escrow.cpp:L562 | ✅ 矿工离线暂停 |
| 5 | `closeescrow` | escrow_id | escrow.cpp:L610 | ✅ 配额耗尽关闭 |
| 6 | `getescrowinfo` | escrow_id | escrow.cpp:L313 | ✅ 完整状态查询 |
| 7 | `listescrows` | *(无)* | escrow.cpp:L383 | ✅ 列表查询 |
| 8 | `commitpricing` | rate_tokens_per_tknc, miner_wallet, user_wallet, amount_tknc, model_name, model_hash | escrow.cpp:L430 | ✅ OP_RETURN哈希承诺 |

---

## A12. 模型注册 [model] — 3个 (BATCH3)

| # | 命令 | 参数 | 源文件 | 状态 |
|---|------|------|--------|------|
| 1 | `registermodel` | model_name, model_hash, file_path, size_mb, quantization, gpu_capability, publisher_wallet | model_registry.cpp:L8 | ✅ LevelDB持久化 |
| 2 | `listmodels` | *(无)* | model_registry.cpp:L121 | ✅ 列表查询 |
| 3 | `getmodelinfo` | model_name | model_registry.cpp:L71 | ✅ 详情查询 |

---

## CLI 命令分类汇总

| 类别 | 数量 | 注册函数 | 核心用途 |
|------|------|---------|---------|
| 区块链查询 | 30 | RegisterBlockchainRPCCommands | 区块/链/交易集查询 |
| 挖矿操作 | 11 | RegisterMiningRPCCommands | GBT模板/提交区块/挖矿 |
| 网络管理 | 20 | RegisterNetRPCCommands | P2P连接/封禁/推理 |
| 内存池 | 16 | RegisterMempoolRPCCommands | 交易广播/池管理 |
| 原始交易 | 15 | RegisterRawTransactionRPCCommands | PSBT/签名/解码 |
| 钱包操作 | 56 | GetWalletRPCCommands | 地址/发送/加密/备份 |
| 节点控制 | 8 | RegisterNodeRPCCommands | 内存/日志/调试 |
| 工具函数 | 10 | 4个子注册函数 | 验证/签名/费率/证明 |
| 外部签名 | 1 | RegisterSignerRPCCommands | 硬件钱包 |
| **TKNC扩展** | **6** | RegisterTKNCAPIKeyRPCCommands | API Key生命周期 |
| **Escrow** | **8** | RegisterEscrowRPCCommands | 链上锁仓+退款+定价 |
| **模型注册** | **3** | RegisterModelRegistryRPCCommands | 矿工模型注册与校验 |
| **总计** | **184** | **15个注册函数** | |

---

# Part B: Web API 端点 (22个)

> **服务**: tknc-miner 内置 HTTP 服务器 (libevent evhttp)
> **端口**: 9332 (**仅绑定 127.0.0.1！禁止 0.0.0.0！所有外部请求由节点中转**)
> **源码**: `TKNC/src/miner/api_server.cpp`
> **铁律**: 矿工不获取公网IP、不注册种子、不对外HTTP。公网地址由节点(tkncd)管理。

---

## B1. AI 推理模块

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 1 | POST | `/api/v1/chat` | HandleChatRequest | AI对话推理，API Key认证，预扣50TKNC，按量计费，支持SSE流式 |

**请求参数**: `{ api_key, messages[], stream? }`
**返回**: `{ status, response, prompt_tokens, completion_tokens, tokens_used(节点独立计算), cost, locked, refunded, remaining_balance }`

---

## B2. API Key 管理

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 2 | POST | `/api/v1/create_key` | HandleCreateKeyRequest | 创建API Key(需链上支付验证≥100TKNC, ≥1确认)**

**请求参数**: `{ miner_id, payment_tx_hash, wallet_address?, model?, expiry_days?, block_hash? }`  
**流程**: 验证链上支付→去重检查→WriteAPIKey到LevelDB

---

## B3. 认证与身份模块 (4个)

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 3 | GET/POST | `/api/v1/auth/challenge` | HandleChallengeRequest | 生成签名挑战(nonce, 600秒过期, 防重放) |
| 4 | POST | `/api/v1/auth/register_identity` | HandleRegisterMinerIdentityRequest | 注册矿工身份(公钥绑定到miner_id) |
| 5 | POST | `/api/v1/auth/verify_miner_signature` | HandleVerifyMinerSignatureRequest | ECDSA签名验证(SHA256+CPubKey.Verify) |
| 6 | POST | `/api/v1/auth/verify_usage` | HandleVerifyUsageRequest | 使用记录审计验证 |

---

## B4. 矿工管理模块 (7个)

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 7 | POST | `/api/v1/miners/register` | HandleMinerRegisterRequest | 矿工注册(去重替换, 自动online, 默认价格10) |
| 8 | POST | `/api/v1/miners/heartbeat` | HandleMinerHeartbeatRequest | 心跳上报(GPU负载/算力, 30秒超时标记offline) |
| 9 | POST | `/api/v1/miners/edit_profile` | HandleEditProfileRequest | 编辑资料(需ECDSA签名认证) |
| 10 | POST | `/api/v1/miners/price_nonce` | HandlePriceNonceRequest | 获取定价nonce(防重放, 300秒过期) |
| 11 | POST | `/api/v1/miners/set_price` | HandleSetPriceRequest | 设置Token价格(需钱包签名+RPC verifymessage) |
| 12 | GET | `/api/v1/miners/public_ip_nonce` | HandlePublicIPNonceRequest | ⚠️[已转移]公网IP设置由**节点**执行，矿工不参与（保留仅兼容旧版） |
| 13 | POST | `/api/v1/miners/set_public_ip` | HandleSetPublicIPRequest | ⚠️[已转移]P2P架构:公网IP由节点通过RPC `setpublicip` 设置，矿工不对外通信 |

---

## B5. 矿工浏览模块 (2个)

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 14 | GET | `/api/v1/miners` | HandleMinersListRequest | 在线矿工列表(支持搜索/过滤) |
| 15 | GET | `/api/v1/miners/detail` | HandleMinerDetailRequest | 单个矿工详情(含评价/评分/运行时间) |

---

## B6. 评价系统模块 (5个)

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 16 | POST | `/api/v1/miners/review` | HandleSubmitReviewRequest | 提交评价(必须使用过服务, 一人一评) |
| 17 | POST | `/api/v1/miners/review/like` | HandleReviewLikeRequest | 点赞/取消点赞(toggle, 首次扣矿工10TKNC) |
| 18 | POST | `/api/v1/miners/request_review` | HandleRequestReviewRequest | 矿工邀请用户评价(锁定10TKNC, 1h过期退还) |
| 19 | POST | `/api/v1/miners/user_review` | HandleUserInitiatedReviewRequest | 用户主动付费评价(付10TKNC给矿工) |
| 20 | GET | `/api/v1/miners/reviews` | HandleMinerReviewsQuery | 查询指定矿工所有评价 |

---

## B7. 系统资源模块

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 21 | GET | `/api/v1/system/resources` | HandleSystemResourceRequest | CPU/GPU/内存监控(CPU>30%告警)**

---

## B8. 网络统计模块

| # | 方法 | 路径 | 处理函数 | 功能 |
|---|------|------|----------|------|
| 22 | GET | `/api/v1/network/stats` | HandleNetworkStatsRequest | 全局统计(高度/难度/在线矿工/经济模型)**

---

## Web API 认证体系三层

| 层级 | 认证方式 | 应用场景 | 存储后端 |
|------|---------|---------|---------|
| L1 | API Key令牌 | chat/create_key/review | LevelDB (`api_keys/`) |
| L2 | Challenge+ECDSA签名 | edit_profile/register_identity | 内存映射表 |
| L3 | 链上钱包签名 | set_price/set_public_ip | RPC verifymessage |

---

# Part C: Express 后端路由 (33个 + WebSocket ×2)

> **服务**: Node.js Express 服务器
> **端口**: 80 (通过nginx反向代理，访问 http://139.196.28.21 无需端口号)
> **源码**: `TKNC/web/server.js`
> **角色**: 浏览器↔发现服务(电话本) ↔ P2P网络 的代理层

---

## C1. 健康检查与静态 (2个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 1 | GET | `/health` | 健康检查(在线矿工数/总矿工数/评论数) | 本地 |
| 2 | GET | `/` | 返回前端主页 index.html | 静态文件 |

---

## C2. 认证路由 (4个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 3 | POST | `/api/login/init` | 登录第一步: 生成签名nonce | 本地 |
| 4 | POST | `/api/login/verify` | 登录第二步: 验证链上签名创建session | RPC verifymessage → 9331 |
| 5 | POST | `/api/login` | 已废弃登录端点 | 本地(返回错误) |
| 6 | POST | `/api/logout` | 注销登录清除session | 本地 |

---

## C3. 用户与钱包 (4个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 7 | POST | `/api/payment/nonce` | 生成一次性支付签名nonce | 本地 |
| 8 | GET | `/api/user/info` | 获取当前登录用户信息 | 本地(session) |
| 9 | GET | `/api/wallet/balance` | 查询钱包余额 | RPC getbalance → 9331 |
| 10 | GET | `/api/wallet/usage` | 查询API Key使用统计(锁定/消耗/可退) | 本地 |

---

## C4. 支付与退款 (1个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 11 | POST | `/api/wallet/refund` | API Key退款(链上sendtoaddress) | RPC sendtoaddress → 9331 |

---

## C5. 评论系统 (8个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 12 | GET | `/api/comments` | 获取最新100条评论(全局) | 本地(内存) |
| 13 | GET | `/api/comments/by_miner/:miner_id` | 获取指定矿工评论+平均评分 | 本地 |
| 14 | GET | `/api/comments/ratings` | 所有矿工平均评分汇总 | 本地 |
| 15 | POST | `/api/comments` | 发表评论(链上支付10TKNC) | RPC sendtoaddress → 9331 |
| 16 | POST | `/api/comments/invite` | 邀请评论(矿工付10TKNC) | RPC sendtoaddress → 9331 |
| 17 | GET | `/api/comments/invites` | 查看邀请记录 | 本地 |
| 18 | DELETE | `/api/comments/:id` | 删除评论(付款方有权删除) | 本地 |
| 19 | GET | `/api/comments/eligible_miners` | 有资格评论的矿工列表 | 本地 |
| 20 | GET | `/api/comments/check_eligibility` | 验证是否有权评论指定矿工 | 本地 |

---

## C6. P2P网络 (4个)

> **⚠️ P2P架构说明**: 矿工API调用通过节点间P2P协议完成（像区块链同步），不依赖公网IP或NAT隧道。以下路由为Web端"电话本"功能。

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 21 | POST | `/api/p2p/register` | 矿工注册到全局发现服务(电话本) | 本地 |
| 22 | POST | `/api/p2p/heartbeat` | 矿工心跳上报(GPU/算力/在线状态) | 本地 |
| 23 | GET | `/api/p2p/miners` | 全部注册矿工列表(含P2P连接状态) | 本地 |
| 24 | GET | `/api/p2p/tunnel/status` | 🚫[已废弃]NAT隧道已移除(P2P架构使用节点直连,无需NAT/打洞) | 本地 |

---

## C7. AI推理网关 (3个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 25 | POST | `/api/v1/chat/gateway` | 全局推理网关(自动选最佳矿工, P2P路由) | P2P → 矿工API 9332 |
| 26 | GET | `/api/p2p/keys/:apiKey/balance` | 查询API Key余额 | 矿工API 9332 |
| 27 | POST | `/api/v1/chat` | AI推理转发(API Key认证, P2P路由) | P2P → 矿工API 9332 |

---

## C8. 矿工操作 (4个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 28 | GET | `/api/miners/:minerId` | 单个矿工详情 | 矿工API 9332 |
| 29 | POST | `/api/create_key` | 创建API Key(支付TKNC+转发矿工) | 矿工API 9332 + RPC 9331 |
| 30 | POST | `/api/miners/set-price` | 设置Token价格(签名验证) | 矿工C++ API + RPC 9331 |
| 31 | POST | `/api/miners/set-public-ip` | ⚠️[已转移]公网IP由**节点**设置(RPC setpublicip)，矿工不对外通信 | 短工C++ API + RPC 9331 |

---

## C9. 网络 (1个)

| # | 方法 | 路径 | 功能 | 代理目标 |
|---|------|------|------|----------|
| 32 | GET | `/api/network/stats` | 网络统计(区块高度等) | 本地 |

---

## WebSocket 端点 (2个)

| # | 路径 | 方向 | 消息类型 | 状态 |
|---|------|------|----------|------|
| WS-1 | `/` | 浏览器↔服务器 | get_miners / miners_list / miner_update / subscribe_miner | ✅ 正常使用 |
| WS-2 | `/ws/tunnel` | 矿工↔服务器 | tunnel_register / tunnel_heartbeat / tunnel_request / tunnel_response | 🚫[已废弃]NAT隧道, P2P架构已移除 |

---

# Part D: 前端 UI 功能清单 (52个)

> **源码**: `TKNC/web/public/index.html` (单文件 ~1365行)  
> **框架**: 原生 JavaScript (无框架依赖)

---

## D1. i18n 多语言功能 (3个)

| # | 功能名称 | 触发方式 | 说明 |
|---|----------|----------|------|
| 1 | 语言切换 | `<select onchange="switchLang()">` | 支持13种主流语言(en/zh/ja/ko/fr/de/es/ru/pt/ar/hi/th/vi) |
| 2 | 翻译函数 `t(key)` | JS调用 | 根据当前语言返回翻译文本 |
| 3 | DOM i18n渲染 | `data-i18n`, `data-i18n-attr` | 页面加载时自动替换标记元素 |

---

## D2. 认证功能 (7个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 4 | 登录表单提交 | `handleLogin(event)` | POST /api/login/init → /api/login/verify |
| 5 | 登录状态检查 | `checkLoginStatus()` 自动调用 | GET /api/user/info |
| 6 | 签名模态框 | `signatureModal` 弹窗 | 显示待签消息+tknc-cli命令模板 |
| 7 | 验证签名并登录 | `verifySignature()` | POST /api/login/verify |
| 8 | 取消签名 | `cancelSignature()` | 关闭签名模态框 |
| 9 | 登出 | `logout()` | POST /api/logout |
| 10 | 登录UI切换 | `updateLoginUI(isLoggedIn)` | 已登录/未登录视图切换 |

---

## D3. 钱包功能 (2个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 11 | 余额刷新 | `refreshBalance()` 定时+手动 | GET /api/wallet/balance + /usage |
| 12 | 余额区域显示 | `balanceSection` 条件渲染 | 可用/锁定/已消耗三栏展示 |

---

## D4. 网络统计功能 (2个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 13 | 加载网络统计 | `loadNetworkStats()` 页面加载时 | GET /api/network/stats |
| 14 | 统计卡片展示 | `statsGrid` 4个stat-card | 在线矿工/模型/请求/高度 |

---

## D5. 矿工列表功能 (8个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 15 | 加载矿工列表 | `loadMiners()` 页面加载时 | GET /api/p2p/miners |
| 16 | 筛选在线矿工 | `filterMiners('online')` | 前端过滤 |
| 17 | 筛选离线矿工 | `filterMiners('offline')` | 前端过滤 |
| 18 | 矿工卡片渲染 | `renderMiners()` 动态生成 | 含ID/模型/状态/算力/VRAM/运行时间/评分/价格/IP |
| 19 | 星级评分显示 | `renderStars(avg)` | 1-5星可视化 |
| 20 | 算力格式化 | `formatHashrate(hashrate, gpuLoad)` | MH/s单位换算 |
| 21 | 运行时间格式化 | `formatUptime(seconds)` | 天/小时/分钟格式 |
| 22 | Token比率格式化 | `formatTokenRatio(ratio)` | K/M tokens格式 |

---

## D6. 矿工操作按钮 (5个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 23 | 调用矿工API(创建Key) | `openMinerKeyModal(...)` | POST /api/payment/nonce → /create_key |
| 24 | 设置Token价格 | `openSetPriceModal(...)` | POST /api/miners/set-price |
| 25 | 设置公网IP | `openSetPublicIPModal(...)` | ✅[必需] 矿工手动指定公网地址/域名（当自动检测失败时使用） |
| 26 | 查看评论 | `openCommentsModal(...)` | GET /api/comments/by_miner/:id |
| 27 | 邀请客户评论 | `openInviteModal(...)` | POST /api/payment/nonce → /comments/invite |

---

## D7. 评论功能 (10个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 28 | 打开评论模态框 | `openCommentsModal(minerId)` | GET /api/comments/by_miner/:id |
| 29 | 关闭评论模态框 | `closeCommentsModal()` | 无 |
| 30 | 加载矿工评论 | `loadMinerComments(minerId)` | GET /api/comments/by_miner/:id |
| 31 | 发表评论 | `submitMinerComment(event)` | POST /api/payment/nonce → /comments (付10TKNC) |
| 32 | 删除评论 | `deleteMinerComment(id)` | DELETE /api/comments/:id (确认对话框) |
| 33 | 评论时间格式化 | `formatCommentTime(isoString)` | "刚刚/X分钟前" |
| 34 | 评论时长格式化 | `formatDuration(seconds)` | 秒转可读格式 |
| 35 | 删除权限检查 | `checkDeletePermission(comment)` | payment_direction判断 |
| 36 | HTML转义(XSS防护) | `escapeHtml(text)` | 无 |
| 37 | 加载评分数据 | `loadRatings()` | GET /api/comments/ratings |

---

## D8. API Key 功能 (4个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 38 | 打开Key模态框 | `openMinerKeyModal(minerId, model)` | 无 |
| 39 | 关闭Key模态框 | `closeKeyModal()` | 无 |
| 40 | 创建API Key | `createAPIKey(e)` | POST /api/payment/nonce → /create_key (最低100TKNC) |
| 41 | 支付金额输入校验 | `input` 事件监听 | 实时校验最低100TKNC |

---

## D9. 邀请评论功能 (3个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 42 | 打开邀请模态框 | `openInviteModal(minerId, wallet)` | 无 |
| 43 | 关闭邀请模态框 | `closeInviteModal()` | 无 |
| 44 | 提交邀请 | `handleInviteComment(event)` | POST /api/payment/nonce → /comments/invite (10TKNC) |

---

## D10. 设置Token价格功能 (4个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 45 | 打开定价模态框 | `openSetPriceModal(minerId, ratio, price)` | 无 |
| 46 | 关闭定价模态框 | `closePriceModal()` | 无 |
| 47 | 价格预览更新 | `updatePricePreview()` input事件 | 实时计算 "1 TKNC = X tokens" |
| 48 | 提交设置价格 | `handleSetPrice(event)` | POST /api/miners/set-price (范围1~1000000) |

---

## D11. 设置公网IP功能 (3个) - ⚠️ [已转移到节点]

> **⚠️ [职责转移 - 2026-06-07 更新]** 公网IP的获取、设置、上报已从矿工**转移到节点（tkncd）**。
>
> **正确架构（铁律，2026-06-07 更新）**:
> - **节点（tkncd）**负责：IPv6优先检测 → STUN检测 → UPnP/NAT-PMP映射 → 手动指定(RPC `setpublicip` 或 Web页面) → 上报种子/Web
> - **矿工启动时自动触发**：矿工启动 → 通知节点检测公网 → 节点检测并自动上报Web
> - 有公网IP → Web显示"在线·可调用"；无公网IP → Web显示"在线·不可调用(无公网)"
> - 公网地址是远程用户通过P2P网络(9333)找到目标节点的**必要条件**
> - Web页面手动指定功能保留（当自动检测失败时使用）

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 49 | 打开IP设置模态框 | `openSetPublicIPModal(minerId, currentIp)` | ✅[必需] 手动指定公网地址/域名 |
| 50 | 关闭IP模态框 | `closePublicIpModal()` | 无 |
| 51 | 提交设置IP | `handleSetPublicIP(event)` | ✅[必需] POST /api/miners/set-public-ip（矿工手动指定公网地址/域名） |

---

## D12. 通知系统 (1个)

| # | 功能名称 | 触发方式 | 对应API |
|---|----------|----------|---------|
| 52 | 显示通知 | `showNotification(msg, type)` | 顶部通知条(成功/错误), 3秒自动消失 |

---

## 前端模态框清单 (7个)

| ID | 用途 | 触发按钮 |
|----|------|----------|
| M1 `commentsModal` | 查看/发布评论 | "评论" 按钮 |
| M2 `inviteModal` | 邀请客户评论 | "邀请" 按钮 |
| M3 `priceModal` | 设置Token价格 | "定价" 按钮(仅自己矿工) |
| M4 `publicIpModal` | 设置公网IP | "IP" 按钮(仅自己矿工) |
| M5 `keyModal` | 创建API Key | "调用API" 按钮(仅在线矿工) |
| M6 `signatureModal` | 签名验证 | 登录/支付时自动弹出 |
| -- `notifBox` | 顶部通知条 | JS函数调用 |

---

# Part E: 核心模块清单

> **源码目录**: `TKNC/src/` 下的核心业务模块

---

## E1. 共识与PoW

| 模块 | 文件 | 功能 |
|------|------|------|
| PoW算法 | `pow/tknchash.cpp` | TKNC专用哈希算法 |
| 难度调整 | `pow/difficulty.cpp` | 动态DAA (窗口=100块, 目标120s/块) |
| 共识规则 | `consensus/params.h` | 共识参数定义 |
| 共识验证 | `consensus/tx_verify.cpp`, `consensus/tx_check.cpp` | 交易验证 |
| Merkle树 | `consensus/merkle.cpp` | Merkle根计算 |

## E2. 经济模型

| 参数 | 值 | 说明 |
|------|-----|------|
| **初始区块奖励** | **9259 TKNC** | 第1个区块的总奖励 |
| **矿工份额 (90%)** | **8333.1 TKNC** | 挖矿地址获得 |
| **团队份额 (10%)** | **925.9 TKNC** | 团队钱包获得 |
| **年减半系数** | 0.5 | 每年奖励减半 |
| **每块递减率** | 0.99999868 | 120秒/块的微量衰减 |
| **总供应量** | 39亿 TKNC | 16年排放完毕 |
| **Coinbase成熟度** | 5确认 | ~10分钟后可花费 |

### 目录结构（程序自动创建）

> **核心规则**: 程序在哪运行，数据就在哪生成。由 `-datadir` 参数决定数据根目录。
>
> **📌 重要更新 (Session 24+28)**:
> - **节点程序** (tkncd.exe) 在A文件夹运行 → **所有数据目录都在A文件夹下创建** ✅ 已验证
> - **矿工程序** (tknc-miner.exe) 可在任意位置运行，通过 `-rpcconnect` 连接远程节点
> - **模型文件** (.gguf) 需放在矿工可访问的路径（通常与矿工同目录或指定 `-model` 参数）
> - **Web服务器** (Express:80) 只在远程服务器上运行，本地不部署
>
> **✅ Session 28 验证结果 (2026-05-18)**:
> - 根目录合规: 仅 .trae/, build/, test/, TKNC/ (无文件)
> - build/data/ 包含完整节点数据: blocks, chainstate, wallets(main_wallet+miner), api_keys等
> - 矿工输出格式符合标准: `[Height:N] [pow/LLM] [Rewards:N] [XXX.XX MH/s] [GPU:XX.X%] [Req:N]`

> **✅ Session 29 验证结果 (2026-05-18)**:
> - 区块高度: **53** (持续增长)
> - 总UTXO金额: **490,709.51 TKNC** (106个UTXO)
> - main_wallet余额: **33,332.29 TKNC** (4笔交易)
> - miner钱包余额: **8,333.05 TKNC** (32笔交易，含1笔孤块)
> - 多钱包架构: ✅ **SQLite格式**, 私钥已启用
> - 地址前缀: ✅ **`token1q...`** (bech32 format)
> - 真实TKNC转账: ✅ **sendtoaddress成功** (TXID: `53a82e48...7fb50`, 1.0 TKNC, fee=0.00000141)
> - 签名功能: ✅ 自有地址正常 / ⚠️ watch-only挖矿地址限制(正常行为)
> - RPC命令测试: **70+命令通过率100%** (A1-A8主要分类)
> - 发现问题: dumpprivkey未实现(ERR-WALLET-049) | 已记录3条新错误(ID49-51)
> - Web API B类(22端点): ⚠️ 依赖矿工进程(9332端口)，待下次启动矿工时测试

> **✅ Session 30 验证结果 (2026-05-18)**:
> - 区块高度: **54** ↑ (持续挖矿中)
> - 节点进程: ✅ PID=16556, 运行正常
> - Web API B类测试(6/22端点):
>   - ✅ B5.1 GET /api/v1/miners → 返回1个online矿工(DESKTOP-IDAK2VU-1779058387)
>   - ⚠️ B7.1 GET /api/v1/network/stats → **ERR-API-017复现** (block_height=0, 应为54)
>   - ❌ B1.1 POST /api/v1/chat → **ERR-API-053新发现** (401 AI推理API Key验证失败)
>   - ✅ B3.1 POST /api/v1/auth/challenge → 签名挑战生成正常(600秒过期)
>   - ✅ B4.7 GET /api/v1/miners/public_ip_nonce → IP nonce生成正常
>   - ⚠️ B4.10 POST /api/v1/miners/price_nonce → 400错误(参数格式问题?)
> - 矿工输出格式检查:
>   - ✅ 标准格式正确: `[Height:54] [pow] [Rewards:1] [147.51 MH/s] [GPU:88.9%] [Req:0]`
>   - ❌ **ERR-OUTPUT-054**: 混入`[DEBUG-RPC] connect FAILED error=10049`调试废话
> - A5原始交易命令: ✅ createrawtransaction/signrawtransactionwithwallet/sendrawtransaction 均已实现
> - Express C类路由: ✅ 首页(/)正常 | ⚠️ /api/status返回404(需核对路由定义)
> - API Keys总数: **10个** (从tknc_listapikeys获取)
> - 关键阻塞: **F5 AI推理流程被ERR-API-053阻断**，需修复API Key验证逻辑

> **✅ Session 31 验证结果 (2026-05-19) - 🎉 历史性突破**:
> - 区块高度: **55** ↑ (持续挖矿，从54增长)
> - 节点进程: ✅ PID=16556, 运行正常
> - **🎯 F5 AI推理首次完整成功！**
>   - ✅ B1.1 POST /api/v1/chat → **status: "success"**
>   - 🎉 LLM真实回复: **"Hello! How can I assist you today?"**
>   - tokens_used: 14 | cost: **1 TKNC** | locked: 50 | refunded: 49
>   - API Key余额: 98 → **97 TKNC** (计费系统正常工作)
> - **ERR-API-053修复验证**: 
>   - 问题: ValidateAPIKey RPC缓存模式缺少实时fallback
>   - 修复: 添加48行fallback代码(L2613-2659)，缓存未命中时调用tknc_getapikeyinfo RPC
>   - 结果: API Key验证通过，不再返回401
> - **ERR-API-017复现修复验证**:
>   - ✅ B7.1 GET /api/v1/network/stats → block_height=**55** (真实值，之前是0)
>   - ✅ current_difficulty=**2.52e-07** (真实值，之前是1.0)
>   - 增强: 添加RPC失败/解析异常详细日志便于诊断
> - **ERR-OUTPUT-054修复验证**:
>   - 移除: api_server.cpp L2468 `std::cout << "[DEBUG-RPC]"` 调试输出
>   - 结果: 矿工运行正常，无调试废话污染输出
> - 矿工三步协议: ✅ Step1启动(PID=6052) → Step2测试通过 → Step3关闭完成
- 编译状态: ✅ tknc-miner.exe重新编译成功(api_server.cpp已更新)

> **✅ Session 34 验证结果 (2026-05-19)**:
> - **Web API B类测试**: **22/22端点(100%覆盖率)** ✅
>   - B1.1-B4.11: 全部11个核心端点通过
>   - B5.12-B7.27: 全部11个扩展端点通过
>   - B6.17评论点赞: ✅ 修复后返回404(ERR-CRASH-057已解决)
> - **Express C类路由测试**: **32/33路由(97%覆盖率)** ✅
>   - C1健康检查/C2登录/C3钱包/C4支付/C5评论/C6P2P/C7代理/C8矿工/C9网络: 全部正常
>   - **C7.25 POST chat/gateway**: ✅ 修复500→503(ERR-PROXY-061)
>   - **C7.26 GET keys/:key/balance**: ✅ 修复500→404(ERR-MISSING-062, 新增路由)
>   - 剩余1个: C2.4 POST /login/verify (需完整钱包签名流程)
> - **新增功能**:
>   - api_server.cpp L4414+: HandleGetAPIKeyBalanceRequest函数
>   - api_server.h L224/L280: 声明添加
>   - server.js L2209+: queryAPIKeyBalance错误处理完善(MINER_404/MINER_400)
> - **代码质量**:
>   - server.js: 33路由+23函数 = 无重复 ✅
>   - api_server.cpp: 49函数 = 无重复 ✅
>   - miner.cpp: 2辅助函数 = 无重复 ✅
> - **矿工输出格式**: `[Height:N] [pow/LLM] [Rewards:N] [XXX.XX MH/s] [GPU:XX.X%] [Req:N]` ✅

```
<datadir>/                    # -datadir 指定的目录（如 D:\block\build\data）
├── tknc.conf                 # 节点配置文件（需预先放置）
├── blocks/                   # 区块链数据（程序首次启动自动创建）
│   ├── blk*.dat             # 区块数据文件
│   ├── rev*.dat             # 撤销数据
│   └── index/               # 区块索引 (LevelDB)
├── chainstate/              # 链状态数据库 (LevelDB)
├── indexes/                  # 交易索引等 (LevelDB)
│   └── txindex/
├── wallets/                  # 钱包数据（创建钱包时自动生成）
│   ├── default/             # 默认钱包
│   │   └── wallet.dat
│   ├── miner/               # 命名钱包 "miner"
│   │   └── wallet.dat
│   └── funder/              # 命名钱包 "funder"
│       └── wallet.dat
├── api_keys/                 # API Key 数据库 (LevelDB, CAPIKeyDB初始化时创建)
│   └── api_keys/
├── peers.dat                 # P2P节点缓存
├── banlist.json              # 封禁列表
├── settings.json             # 节点设置
├── debug.log                 # 运行日志
└── tkncd.pid                 # 进程PID文件

# 模型路径（由 -model 参数指定，独立于 datadir）
models/
└── qwen2.5-0.5b-instruct.gguf
```

| 模块 | 文件 | 功能 |
|------|------|------|
| 区块奖励 | `economics/emission.h` | 区块排放曲线(初始9259→90/10分割) |
| 手续费策略 | `policy/feerate.cpp`, `policy/fees.cpp` | 费率估算 |
| RBF策略 | `policy/rbf.cpp` | 替换-by-费率 |

## E3. 矿工模块

| 模块 | 文件 | 功能 |
|------|------|------|
| 矿工主程序 | `miner/miner.cpp` | GBT轮询/提交/coinbase构造(BIP34+90/10分割) |
| API服务器 | `miner/api_server.cpp` | 22个HTTP端点(libevent) |
| 评价链 | `miner/review_chain.cpp` | ReviewChain设计(未来上链) |
| 模式切换 | `miner/mode_switcher.py` | 运行模式管理 |

## E4. API Key 系统

| 模块 | 文件 | 功能 |
|------|------|------|
| 数据结构 | `apikey/api_key.h` | APIKey结构体定义 |
| DB封装 | `apikey/api_key_db.h` | LevelDB读写接口 |
| 实现 | `apikey/api_key.cpp` | CreateAPIKey/ReadAPIKey/WriteAPIKey等 |
| RPC接口 | `rpc/tknc_apikey.cpp` | 6个JSON-RPC命令(LevelDB持久化✅) |

## E5. 模型推理

| 模块 | 文件 | 功能 |
|------|------|------|
| LLaMA加载 | `model/loader.cpp` | GGUF模型加载 |
| LLaMA运行时 | `model/runtime.cpp` | 推理执行 |
| LLaMA DLL | `model/llama_dll.cpp` | llama.cpp绑定 |
| GPU内存 | `model/gpu_memory.h` | GPU显存管理 |

## E6. 网络层

| 模块 | 文件 | 功能 |
|------|------|------|
| P2P消息 | `net/message.cpp`, `net/message.h` | P2P协议消息 |
| NAT穿透 | `net/nat_traversal.h` | NAT隧道设计 |
| DHT | `net/dht/` | 分布式哈希表(预留) |
| API协议 | `net/api_protocol.h` | 矿工间通信协议 |

## E7. 节点核心

| 模块 | 文件 | 功能 |
|------|------|------|
| 主线程 | `init.cpp` | 节点初始化/启动/关闭(含CAPIKeyDB init/shutdown) |
| 验证 | `validation.cpp` | 区块/交易验证 |
| 链状态 | `chain.cpp` | 链管理 |
| 内存池 | `txmempool.cpp` | 交易池 |
| 区块存储 | `flatfile.cpp` | BLK/DAT文件存储 |

---

# Part F: 端到端核心流程

## F1. 挖矿流程

```
tknc-miner.exe 启动
    ↓
连接 RPC 9331 (tkncd)
    ↓
循环: getblocktemplate (GBT) → TKNC Hash → submitblock
    ↓
Coinbase: vout[0]=矿工地址(90%) + vout[1]=团队地址(10%)
    ↓
BIP34: height>=1 时 coinbase 编码高度
    ↓
区块确认 → 奖励到账
```

## F2. 登录认证流程

```
用户输入钱包地址
    ↓
POST /api/login/init → 生成 nonce
    ↓
前端显示待签消息 + tknc-cli signmessage 命令模板
    ↓
用户在终端执行签名 → 粘贴 Base64
    ↓
POST /api/login/verify → RPC verifymessage (私钥永不离开客户端)
    ↓
创建 session (24h有效) → 登录成功
```

## F3. API Key 创建与使用流程

```
方式A: RPC直接创建
    tknc-cli tknc_createapikey 100.0 → 写入LevelDB → 返回 tknc_xxxx

方式B: Web前端创建(需矿工在线)
    选择矿工 → 输入金额(≥100TKNC) → 签名支付
    → POST /api/create_key → 转发矿工API 9332
    → 验证链上支付 → WriteAPIKey(LevelDB) → 返回 api_key

使用:
    POST /api/v1/chat { api_key, messages }
    → 预扣50TKNC → LLM推理 → 按量扣费 → 多退少补
```

## F4. 评论系统流程

```
发表评论:
    打开评论模态框 → 输入昵称/内容/评分(1-5星)
    → POST /api/payment/nonce → 签名(付10TKNC)
    → POST /api/comments → RPC sendtoaddress (链上转账)
    → 保存评论到内存 → 显示

删除评论:
    点击删除 → 确认对话框
    → DELETE /api/comments/:id → 验证付款方身份 → 删除

邀请评论(矿工发起):
    输入客户钱包 → 签名(付10TKNC)
    → POST /api/comments/invite → 链上转账 → 创建邀请记录
```

## F5. AI推理完整流程

> **⚠️ P2P架构**: 推理请求通过节点间P2P协议传输，不经过公网HTTP直连。

```
方式A: Web端发起(通过Express代理→P2P路由)
用户持有 api_key
    ↓
POST /api/v1/chat { api_key: "tknc_xxx", messages: [{role:"user", content:"Hello"}] }
    ↓
Express server.js → 查找矿工P2P地址 → 通过节点转发
    ↓
矿工API 9332 验证 key + 余额 → 锁定50TKNC
    ↓
调用 llama.cpp Generate() → LLM推理
    ↓
矿工返回: { response, prompt_tokens, completion_tokens }
    ↓
节点独立计算 tokens_used = prompt_tokens + completion_tokens
    ↓
计算实际费用 (tokens_used × price_per_1m_tknc)
    ↓
扣除实际费用 → 退还剩余 (locked - cost = refunded)
    ↓
返回: { response, prompt_tokens, completion_tokens, tokens_used(节点独立计算), cost, locked, refunded, remaining_balance }

方式B: P2P直连(客户端节点→矿工节点)
客户端节点(tkncd) → P2P消息 NETMSG_LLM_INFERENCE_REQUEST → 矿工节点
    ↓
矿工节点 p2p_llm.cpp ProcessInferenceRequest() → 本地LLM推理
    ↓
结果通过P2P消息 NETMSG_LLM_INFERENCE_TOKEN/DONE 返回客户端节点
```

---

# 附录: 端口配置速查

| 服务 | 端口 | 协议 | 用途 |
|------|------|------|------|
| tkncd (节点) | **9331** | JSON-RPC | 区块链节点RPC |
| tkncd (P2P) | **9333** | TCP (自定义) | 节点间P2P通信/推理请求（**节点公网可达**） |
| **tknc-miner (矿工API)** | **9332** | HTTP (libevent) | **AI推理/API Key/矿工管理(仅127.0.0.1！矿工独占端口)** |
| Express (Web后端) | **80** | HTTP (nginx代理) | 模型交易平台前端代理/Session/发现服务 |

> ⚠️ **铁律（2026-06-07 更新）**：
> - **9332 是矿工的独占端口，仅绑定 127.0.0.1。节点不使用此端口。**
> - 节点通过 RPC (`p2pinference`) + P2P 网络(9333) 接收和转发推理请求，不是通过 WebSocket 代理。
> - Web服务**绝不使用9313端口**！对外统一使用80端口(nginx反向代理)
> - 矿工绝对不能绑定0.0.0.0！
> - 公网地址是**节点的** P2P 端口(9333)，不是矿工的

---

# 附录: 字段命名规范

| 项目 | 规范 |
|------|------|
| 可执行文件 | tkncd.exe, tknc-miner.exe, tknc-cli.exe, tknc.exe, tknc-wallet.exe |
| 钱包地址前缀 | `token` (如 token1q60k3gpxjg...) |
| API Key前缀 | `tknc_` (32位hex, 如 tknc_a61ca6cb...) |
| Magic Bytes | `0x4E414200` ("NAB") |
| 产品名 | "TKNC Blockchain" (禁止Bitcoin/bitcoind) |
| 网络类型 | main (MainNet, 仅支持主网模式) |

---

# 附录: 修复历史

## 2026-05-17 (Session 2) — 高优先级Bug修复

| Bug ID | 描述 | 文件 | 修复内容 | 验证 |
|--------|------|------|----------|------|
| ERR-FLOW-012 | 模型路径硬编码忽略`-model`参数 | api_server.cpp L102 + tknc-miner.cpp L436 + api_server.h | 构造函数添加model_path_参数，Start()优先使用传入路径 | F5推理返回真实LLM输出 ✅ |
| ERR-DOM-001 | JS引用不存在的DOM ID | index.html L6791,L7279 | sigMessage→signMessageDisplay, sigCommand→signCommandDisplay (4处) | 签名模态框DOM正常渲染 ✅ |
| ERR-API-017 | B22 block_height=0未连接RPC | api_server.cpp HandleNetworkStatsRequest | 添加CallNodeRPC('getblockchaininfo')获取真实高度和难度 | height=11, difficulty=120143060 ✅ |
| ERR-PROXY-018 | Express代理3路500无优雅降级 | server.js L1348,L1362,L1410 + fetchMinerDetail L2321 | 区分ECONNREFUSED→503 vs 内部错误→500; 修复keyData未定义 | /api/miners/:id 500→200 ✅ |
| ERR-CACHE-020 | LoadMinerData覆盖online为offline | api_server.cpp L833-839 | 缓存加载时检查现有活跃状态，保留online/mining/inference | status=offline→online ✅ |

## 2026-05-17 (Session 3) — 中优先级清理

| Bug ID | 描述 | 文件 | 修改量 | 验证 |
|--------|------|------|--------|------|
| ERR-BTC-021 | BTC/Bitcoin/satoshi残留~200+处 | Qt(22文件35处)+函数名(28文件79处)+satoshi(91文件)+locale(85文件) | **~200+处** | ✅ 编译通过 |
| ERR-DUP-022 | Base64编码3处重复 | miner/miner.cpp(-43行)+api_server.cpp(-29行) | **-72行** | ✅ 编译通过 |
| — | i18n翻译补全(7/24语言) | index.html (~455键) | **~455键** | ✅ 7语言完成 |

### 遗留问题更新

| # | 问题 | 状态 |
|---|------|------|
| 1 | BTC残留 | ✅ **已清理** (~200+处) |
| 2 | Base64×3重复 | ⚠️ **部分修复** (Base64✅, 奖励×6待提取) |
| 3 | 翻译24种语言~65键缺失 | 🔄 **进行中** (7/24完成, 17/24待续) |

*最后更新: 2026-05-17 Session 3*

## 2026-05-17 (Session 4) — 全流程端到端验证

| Bug ID | 描述 | 文件 | 验证 |
|--------|------|------|------|
| — | 数据完全重置(清空blocks/chainstate/wallets/api_keys) | build/data/ | ✅ 从头开始 |
| — | 节点主网启动(height:0→17, difficulty KAS DAA) | tkncd.exe | ✅ chain:"main" |
| — | 钱包创建+初始余额(49,998 TKNC mature) | miner wallet | ✅ token前缀 |
| A1-A10 | RPC核心命令测试(20+命令) | CLI | ✅ 全部响应 |
| A10 | TKNC API Key LevelDB 6命令 | tknc_apikey.cpp | ✅ create/list/get/validate |
| B2 | create_key链上支付验证(402→success) | api_server.cpp | ✅ payment_verified:true |
| **F5** | **AI推理端到端("Hello"真实LLM输出)** | **api_server.cpp** | **✅ cost:1, refunded:49** |
| 矿工输出格式 | [Height:N][mode][Rewards][HashRate][GPU][Req] | miner.cpp | ✅ 符合用户要求 |
| D类UI | 前端5项功能+BTC残留检测(0匹配) | index.html | ✅ ERR-BTC-021无回归 |

### Session 4 新发现问题

| # | Bug ID | 描述 | 状态 |
|---|--------|------|------|
| 1 | ERR-CRASH-023 矿工崩溃(exit -1073740791) | 🔄 待修复 |
| 2 | ERR-EXPR-024 Express /api/network/stats height=0 | 🔄 待修复 |
| 3 | ERR-FLOW-008-V3 API Key双轨制(第3次) | 🔄 高优先级 |

*最后更新: 2026-05-17 Session 4*

## 2026-05-17 (Session 5) — ERR-FLOW-008根因定位 + 全流程验证

| Bug ID | 描述 | 文件 | 验证 |
|--------|------|------|------|
| — | 数据完全重置(清空blocks/chainstate/wallets/api_keys等12项) | build/data/ | ✅ 从头开始 |
| — | 节点主网启动(height:0→17, difficulty KAS DAA) | tkncd.exe | ✅ chain:"main" |
| — | 钱包创建+初始余额(49,998 TKNC mature) | miner wallet | ✅ token前缀 |
| A10×4 | API Key LevelDB全命令(create/list/get/validate) | tknc_apikey.cpp | ✅ 全部通过 |
| **F5** | **AI推理端到端("Hello"真实LLM+TKNC扣费退款)** | **api_server.cpp** | **✅ cost:1, refunded:49** |
| 矿工输出格式 | [Height:N][mode][Rewards][HashRate][GPU][Req] | miner.cpp | ✅ 符合用户要求 |
| D类UI | 前端6项功能+BTC残留(0匹配)+DOM ID回归检测 | index.html | ✅ 全部通过 |
| **ERR-FLOW-008** | **根因定位: ValidateAPIKey(L2564)从LevelDB读取但RPC Key返回401** | **api_server.cpp** | **🔴 需调试** |

### Session 5 核心发现

| # | 发现 | 严重度 | 状态 |
|---|------|--------|------|
| 1 | **ERR-FLOW-008-V4**: ValidateAPIKey确实用LevelDB，但RPC Key仍401 | 🔴 致命 | 根因已定位，待调试 |
| 2 | ERR-EXPR-024: Express /api/network/stats height=0 | 🟡 中 | 未修复 |
| 3 | ERR-RPC-016: getmininginfo缺少mining_mode字段 | 🟡 低 | 已知遗留 |

### ERR-FLOW-008根因分析

```
矿工API验证链路:
POST /api/v1/chat → HandleChatRequest(L2635)
  → ValidateAPIKey(api_key, balance) [L2685]
    → api_key_db->ReadAPIKey(key) [L2571] ← 从LevelDB读取
      → has_value()? → false → 返回401 ❌

节点RPC写入链路:
tknc_createapikey → CAPIKeyDB::WriteAPIKey(key, api_key)
  → 写入 LevelDB: build/data/api_keys/api_keys/
  
矛盾点:
├── RPC tknc_listapikeys → 能读到Key ✅ (LevelDB确实有数据)
├── 矿工 ValidateAPIKey → 读不到Key ❌ (同一个LevelDB?)
└── B2 create_key(Web层) → F5 chat成功 ✅ (Web层Key路径正确)

推测原因:
1. CAPIKeyDB实例不同(节点vs矿工各持有一个)
2. 文件锁竞争(LevelDB单写多读限制)
3. Key序列化格式不兼容
```

*最后更新: 2026-05-17 Session 5*

---

## Session 13 更新记录 (2026-05-17)

### ✅ 已完成修复

| # | 修复项 | 状态 | 说明 |
|---|--------|------|------|
| 1 | 网络模式切换 | ✅ 完成 | regtest永久移除, 仅保留mainnet |
| 2 | 钱包创建/加载 | ✅ 完成 | main_wallet, getbalance正常 |
| 3 | LLM状态显示[LLM] | ✅ 完成 | 推理时正确输出[LLM Inference] |
| 4 | API Key功能测试 | ✅ 完成 | create/validate/list全部正常 |

### 📊 Session 13 测试结果 (mainnet状态)

#### CLI RPC 测试通过
```
✅ getblockchaininfo: chain=main, blocks=0, difficulty=1
✅ getmininginfo: mining_mode=pow, next.height=1
✅ getnetworkinfo: subversion=/TKNC:1.0.0/, connections=0
✅ tknc_createapikey: api_key=tknc_7609f729..., balance=100 TKNC
✅ tknc_validateapikey: valid=true
✅ tknc_listapikeys: 3个Keys
```

#### Web API 测试通过
```
✅ POST /api/v1/chat: status=success, cost=1TKNC, remaining=99
✅ 矿工输出: [LLM Inference] 标记正确显示
```

### ⚠️ 新发现问题

| # | 问题 | 类型 | 严重程度 | 说明 |
|---|------|------|---------|------|
| 1 | **翻译不完整** | i18n | 中 | 前端38种语言选项，仅7种有翻译数据(zh/en/ja/ko/fr/de/es) |
| 2 | **P2P连接为0** | 网络 | 低 | mainnet新节点，需配置种子节点或等待DNS发现 |

### 📁 目录结构确认

```
D:\block\
├── build/          ✅ 编译产物+运行时数据
│   ├── bin/Release/ (tkncd.exe, tknc-miner.exe)
│   ├── data/        (程序自动创建的区块链数据)
│   └── models/      (AI模型 .gguf)
├── data/            ✅ 主区块链数据(预留)
├── TKNC/            ✅ 源代码
│   ├── src/
│   └── web/
└── test/            ✅ 临时文件唯一存放地
```

---

## 已知遗留问题 (优先级排序)

| # | 问题 | 类型 | 影响 |
|---|------|------|------|
| 1 | BTC残留~85项(Qt注释/satoshi/BTC函数名) | 命名规范 | UI显示Bitcoin字样 |
| 2 | 重复代码(Base64×3, HTTP×4, 奖励×6处) | DRY违规 | 维护困难 |
| 3 | 24种语言~65键翻译缺失(从modalCommentTitle起) | i18n不完整 | 切换语言后部分英文占位 |
| 4 | /api/login/init被SPA fallback拦截返回HTML | 路由顺序 | 登录初始化失败 |

---

*文档结束 · 共计 280+ 功能点 · 最后更新: 2026-05-17*
