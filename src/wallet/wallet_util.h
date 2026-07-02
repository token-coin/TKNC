#ifndef TKN_WALLET_UTIL_H
#define TKN_WALLET_UTIL_H

#include <key.h>
#include <pubkey.h>
#include <addresstype.h>
#include <key_io.h>
#include <chainparams.h>
#include <script/solver.h>
#include <hash.h>
#include <script/script.h>
#include <base58.h>

#include <string>
#include <iostream>

struct WalletInfo {
    CKey private_key;
    CPubKey public_key;
    CTxDestination destination;
    std::string address;
    std::string private_key_wif;
    CScript script_pub_key;
};

WalletInfo GenerateNewWalletAddress() {
    WalletInfo wallet;
    
    wallet.private_key = GenerateRandomKey(true);
    if (!wallet.private_key.IsValid()) {
        std::cerr << "Error: generated private key is invalid\n";
        return wallet;
    }
    
    wallet.public_key = wallet.private_key.GetPubKey();
    if (!wallet.public_key.IsValid()) {
        std::cerr << "Error: generated public key is invalid\n";
        return wallet;
    }
    
    CKeyID key_id = wallet.public_key.GetID();
    PKHash pk_hash(key_id);
    wallet.destination = pk_hash;
    
    wallet.address = EncodeDestination(wallet.destination);
    wallet.private_key_wif = EncodeSecret(wallet.private_key);
    wallet.script_pub_key = GetScriptForDestination(wallet.destination);
    
    return wallet;
}

bool ValidateWalletAddress(const std::string& address, std::string& error_msg) {
    if (address.empty()) {
        error_msg = "Wallet address is empty";
        return false;
    }

    const CChainParams& params = Params();
    std::string hrp = params.Bech32HRP();
    std::string bech32_prefix = hrp + "1";

    bool is_bech32 = (address.size() >= bech32_prefix.size() &&
                      address.substr(0, bech32_prefix.size()) == bech32_prefix);

    if (is_bech32) {
        if (address.size() < 38 || address.size() > 90) {
            error_msg = "Invalid Bech32 address length (" + std::to_string(address.size()) + "), expected 42 characters for token1... format";
            return false;
        }
        std::string decode_error;
        CTxDestination dest = DecodeDestination(address, decode_error);
        if (!IsValidDestination(dest)) {
            error_msg = "Invalid TKNC Bech32 address: " + decode_error + " (expected format: token1q...)";
            return false;
        }
        return true;
    }

    std::vector<unsigned char> data;
    if (!DecodeBase58Check(address, data, 25)) {
        error_msg = "Invalid Base58 address encoding or checksum (expected format: t... for P2PKH legacy)";
        return false;
    }

    const std::vector<unsigned char>& pubkey_prefix = params.Base58Prefix(CChainParams::PUBKEY_ADDRESS);
    if (data.size() < pubkey_prefix.size() || !std::equal(pubkey_prefix.begin(), pubkey_prefix.end(), data.begin())) {
        error_msg = "Invalid address prefix (expected 't' for TKNC P2PKH, got '" +
                     (address.empty() ? "" : std::string(1, address[0])) + "')";
        return false;
    }

    uint160 hash_size;
    if (data.size() != hash_size.size() + pubkey_prefix.size()) {
        error_msg = "Invalid Base58 address length (expected " +
                     std::to_string(hash_size.size() + pubkey_prefix.size()) + " bytes for P2PKH)";
        return false;
    }

    std::string decode_error;
    CTxDestination dest = DecodeDestination(address, decode_error);
    if (!IsValidDestination(dest)) {
        error_msg = "Invalid TKNC address: " + decode_error;
        return false;
    }

    return true;
}

void PrintWalletInfo(const WalletInfo& wallet) {
    std::cout << "========================================\n";
    std::cout << "  Wallet Info\n";
    std::cout << "========================================\n";
    std::cout << "Address: " << wallet.address << "\n";
    std::cout << "Public Key: " << HexStr(wallet.public_key) << "\n";
    std::cout << "Private Key (WIF): " << wallet.private_key_wif << "\n";
    std::cout << "ScriptPubKey: " << HexStr(wallet.script_pub_key) << "\n";
    std::cout << "========================================\n";
}

#endif // TKN_WALLET_UTIL_H
