// OpenSSL compatibility layer for macOS using CommonCrypto
// This provides the necessary OpenSSL functions used by tknc-cli

#ifndef OPENSSL_COMPAT_H
#define OPENSSL_COMPAT_H

#ifdef __APPLE__
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>

// Define EVP_sha512 for compatibility
inline void* EVP_sha512(void) { return nullptr; }

// PKCS5_PBKDF2_HMAC replacement using CommonCrypto
inline int PKCS5_PBKDF2_HMAC(
    const char* password, int password_len,
    const unsigned char* salt, int salt_len,
    int iterations, void* digest,
    int key_len, unsigned char* out) {
    (void)digest; // Unused, always use HMAC-SHA512
    if (CCKeyDerivationPBKDF(kCCPBKDF2,
        password, password_len > 0 ? password_len : 0,
        salt, salt_len,
        kCCPRFHmacAlgSHA512,
        iterations,
        out, key_len) == kCCSuccess) {
        return 1;
    }
    return 0;
}

#else
// Non-Apple platforms: include real OpenSSL
#include <openssl/evp.h>
#endif

#endif // OPENSSL_COMPAT_H
