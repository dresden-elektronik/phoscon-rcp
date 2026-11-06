/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Manuel Pietschmann */
#ifndef AES_H
#define AES_H

/*
 * Aes, the block cipher and the modes the security band needs: FIPS-197 (the block), IEEE
 * 802.15.4-2024 Annex B (CCM*), SP 800-38A (CTR), SP 800-38B (CMAC). Forward cipher only,
 * because every mode the stack runs is built from forward block encryptions. The key
 * object is caller-owned.
 *
 * The block cipher is a SEAM: Aes_KeySchedule and Aes_EncryptBlock are each defined by
 * exactly one implementation file, and the build links exactly one; the CMake option
 * ZB_AES_IMPLEMENTATION picks the file, and two definitions fail the link.
 */

#include <stdint.h>
#include "zb_status.h"

/* The expanded key: the 16-octet key and the 11 round keys in schedule order
   (FIPS-197 §5). 192 bytes, caller-owned. */
typedef struct Aes_Key
{
    uint8_t key[16];
    uint8_t roundKey[176];
} Aes_Key;

/* Expand a 128-bit key (FIPS-197 §5.2). ZB_OK, or ZB_ERR_BAD_ARGUMENT for a 0 key. */
ZbStatus Aes_KeySchedule(Aes_Key *key, const uint8_t key16[16]);

/* Zero the object, key and schedule. */
void Aes_KeyWipe(Aes_Key *key);

/* THE SEAM: the forward block cipher (FIPS-197 §5.1). One definition per build. */
void Aes_EncryptBlock(const Aes_Key *key, const uint8_t in[16], uint8_t out[16]);

/* CTR mode (SP 800-38A §6.5): counter is the caller's 128-bit big-endian integer,
   incremented as a whole after every block and left at its final value; inLen may be 0. */
void Aes_CtrCipher(const Aes_Key *key, uint8_t counter[16],
                   const uint8_t *in, uint32_t inLen, uint8_t *out);

/* CMAC (SP 800-38B / RFC 4493): micLen in {8..16}, the leftmost micLen octets of the
   full 16-octet MAC; inLen may be 0. ZB_OK, or ZB_ERR_BAD_ARGUMENT for a 0 in or a
   micLen outside the range. */
ZbStatus Aes_Cmac(const Aes_Key *key, const uint8_t *in, uint32_t inLen,
                  uint8_t *mic, uint32_t micLen);

/* CCM* (IEEE 802.15.4-2024 Annex B.3; identical to NIST SP 800-38C for M <= 8):
   nonceLen in {7..13} (L = 15 - nonceLen, L = 1 reserved), micLen in {4,6,8,10,12,14,16},
   inLen < 2^(8*L). out receives the ciphertext (inLen octets), mic the tag (micLen
   octets); on the wire the tag comes last. ZB_OK, or ZB_ERR_BAD_ARGUMENT for a 0
   pointer or an out-of-range length. */
ZbStatus Aes_CcmEncrypt(const Aes_Key *key,
                        const uint8_t *nonce, uint32_t nonceLen,
                        const uint8_t *adata, uint32_t adataLen,
                        const uint8_t *in, uint32_t inLen,
                        uint32_t micLen,
                        uint8_t *out, uint8_t *mic);

/* The inverse (Annex B.3.3): in is the ciphertext (inLen octets), mic the tag.
   ZB_OK with the plaintext written, or ZB_ERR_AUTH when the tag does not verify,
   then nothing is written to out. */
ZbStatus Aes_CcmDecrypt(const Aes_Key *key,
                        const uint8_t *nonce, uint32_t nonceLen,
                        const uint8_t *adata, uint32_t adataLen,
                        const uint8_t *in, uint32_t inLen,
                        const uint8_t *mic, uint32_t micLen,
                        uint8_t *out);

#endif /* AES_H */
