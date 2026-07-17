# TKNC Miner - Quick Start Guide

## Prerequisites

1. **NVIDIA GPU**  with latest NVIDIA driver
2. **Windows 10/11** or **Ubuntu Linux** or **macOS**
3. Download from [GitHub Releases](https://github.com/token-coin/TKNC/releases)

## Step 1: Download & Extract

Download the archive for your platform and extract it:

| File | Platform |
|------|----------|
| `TKNC-Windows.zip` | Windows |
| `Ubuntu-Linux.zip` | Linux |
| `TKNC-macos.zip` | macOS |
| `dll.zip` | Required DLL package (Windows only) |

On Windows, extract `dll.zip` into the same folder as `tkncd.exe`. The folder structure should look like:

```
TKNC/
├── tkncd.exe          # Node
├── tknc-cli.exe       # CLI wallet tool
├── tknc-miner.exe     # Miner
├── dll/               # DLL files (from dll.zip)
│   ├── ggml-cuda.dll
│   ├── ggml-vulkan.dll
│   ├── llama.dll
│   └── ...
└── models/            # LLM model files (.gguf)
```

## Step 2: Start the Node

The node must be running before starting the miner.

```bash
tkncd.exe
```

Wait until you see "block verification complete" — the node is now synced and ready.

## Step 3: Create a Wallet

Open a new terminal and run:

```bash
tknc-cli.exe
```

Follow the interactive menu:

1. Select **Create New Wallet**
2. Enter a wallet name (e.g., `mywallet`)
3. Set a password (or leave blank for no encryption)
4. Confirm the password

Your wallet address will be displayed (starts with `token1...`). Save this address — you'll need it for mining.

## Step 4: Download a Model

Place a GGUF model file in the `models/` directory:

```
TKNC/
└── models/
    └── Qwen2.5-7B-Instruct-Q5_K_M.gguf
```

The miner auto-discovers the largest `.gguf` file. Recommended models:
- **Qwen2.5-7B-Instruct-Q5_K_M** (~5 GB, fits on 6 GB GPUs)
- **Qwen2.5-14B-Instruct-Q4_K_M** (~9 GB, needs 2+ GPUs)

## Step 5: Start Mining

```bash
tknc-miner.exe -wallet=token1qyourwalletaddress -token=20000
```

| Parameter | Description |
|-----------|-------------|
| `-wallet=` | Your wallet address from Step 3 |
| `-token=` | Token exchange rate (N tokens = 1 TKNC). **Required.** |
| `-model=` | (Optional) Path to a specific .gguf model |
| `-n_ctx=` | (Optional) LLM context window size (default: 131072) |

### What happens next?

The miner will:
1. Connect to the local node (`tkncd`)
2. Detect your GPU(s) and load the CUDA or Vulkan backend
3. Load the LLM model onto GPU VRAM
4. Start mining (PoW hashing + LLM inference)

When a block is found, you'll see:

```
>>> BLOCK FOUND! Height: 8206 Hash: c0e9efd9... Miner Reward: 2919.05 TKNC
```

## Multi-GPU Support

TKNC Miner supports multiple GPUs automatically:
- **PoW Mining**: All GPUs hash in parallel, each processing independent nonce ranges
- **LLM Inference**: Model tensors are split across GPUs proportional to VRAM

No special configuration needed — just install multiple GPUs and the miner will use them all.

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "Cannot connect to TKNC node" | Start `tkncd.exe` first |
| "Model file not found" | Place `.gguf` file in `models/` directory |
| "Wallet address required" | Use `-wallet=token1q...` or place `wallet.dat` in `wallets/` |
| CUDA load failed (err=127) | Update NVIDIA driver, or use `dll/` from the latest release |
| "does not support 16-bit storage" | Vulkan fallback issue; ensure CUDA DLLs are in `dll/` folder |

## Useful Commands

```bash
# Check wallet balance
tknc-cli.exe

# Check node status
tknc-cli.exe getblockchaininfo

# Stop the node
tknc-cli.exe stop
```

---

For more information, visit: https://github.com/token-coin/TKNC



TKNC-LMM-video
https://www.youtube.com/watch?v=ZWM2AOYBQZE
