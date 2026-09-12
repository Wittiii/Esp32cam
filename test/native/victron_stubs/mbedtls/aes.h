#pragma once

#include <cstddef>
#include <cstring>

// Deliberately bypass AES: these tests cover parsing and task interleavings,
// not mbedTLS crypto correctness. All fixtures and keys are synthetic.
extern bool testAesFails;
struct mbedtls_aes_context {};
inline void mbedtls_aes_init(mbedtls_aes_context *) {}
inline void mbedtls_aes_free(mbedtls_aes_context *) {}
inline int mbedtls_aes_setkey_enc(mbedtls_aes_context *, const unsigned char *,
                                unsigned) {
  return testAesFails ? -1 : 0;
}
inline int mbedtls_aes_crypt_ctr(mbedtls_aes_context *, size_t length, size_t *,
                               unsigned char *, unsigned char *,
                               const unsigned char *input,
                               unsigned char *output) {
  std::memcpy(output, input, length);
  return 0;
}
