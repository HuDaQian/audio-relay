#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace audio_relay {

// SHA-256
void sha256(const uint8_t* data, size_t len, uint8_t hash[32]);

// HMAC-SHA256
void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* data, size_t data_len,
                 uint8_t out[32]);

// HKDF-SHA256 (RFC 5869) for 32-byte key output
void hkdf_sha256_32(const uint8_t* salt, size_t salt_len,
                    const uint8_t* ikm, size_t ikm_len,
                    const uint8_t* info, size_t info_len,
                    uint8_t okm[32]);

// ChaCha20-Poly1305 AEAD encrypt (RFC 8439)
// out_ciphertext must have room for plaintext_len bytes.
// out_tag will receive 16 bytes.
void chacha20_poly1305_seal(const uint8_t key[32],
                            const uint8_t nonce[12],
                            const uint8_t* aad, size_t aad_len,
                            const uint8_t* plaintext, size_t plaintext_len,
                            uint8_t* out_ciphertext,
                            uint8_t out_tag[16]);

// Constant-time memory comparison to prevent timing attacks
bool constant_time_eq(const uint8_t* a, const uint8_t* b, size_t len);
bool constant_time_eq_str(const std::string& a, const std::string& b);

// Compute HMAC-SHA256 and return lower-case hex string
std::string hmac_sha256_hex(const uint8_t* key, size_t key_len, const std::string& data);

} // namespace audio_relay
