/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Manuel Pietschmann */
/* Suite for the crypto band: the known answers of the src/crypto/ contract, one suite over
   the vectors the specifications print: FIPS-197 5.2 and appendix C.1,
   SP 800-38A F.5.1 (CTR-AES128.Encrypt), SP 800-38B D.1, SP 800-38C C.1-C.4, IEEE
   802.15.4-2024 C.3.2.2, and the Zigbee shape of CCM*. Each vector is quoted here the way the
   standard quotes it, so the suite is checkable against the published text without the file at
   hand (docs/internals/refs.md). The mode vectors are the ones that exercise the two shapes a mode has to
   get right: a counter that crosses 2^8, and a final block that is either partial or exact. */

#include "aes.h"
#include "test_harness.h"

/* FIPS-197 §5.2: the expansion of 000102...0e0f is the schedule the standard prints (eleven
   sixteen-octet round keys in schedule order, the key itself first and then the ten the
   rounds consume) and the object keeps the key it was expanded from beside the 176
   schedule octets (src/crypto/aes.h). */
static void aesDoKeyScheduleCheck(void)
{
    static const char *keyHex = "000102030405060708090a0b0c0d0e0f";
    static const char *roundHex[11] = {
        "000102030405060708090a0b0c0d0e0f", "d6aa74fdd2af72fadaa678f1d6ab76fe",
        "b692cf0b643dbdf1be9bc5006830b3fe", "b6ff744ed2c2c9bf6c590cbf0469bf41",
        "47f7f7bc95353e03f96c32bcfd058dfd", "3caaa3e8a99f9deb50f3af57adf622aa",
        "5e390f7df7a69296a7553dc10aa31f6b", "14f9701ae35fe28c440adf4d4ea9c026",
        "47438735a41c65b9e016baf4aebf7ad2", "549932d1f08557681093ed9cbe2c974e",
        "13111d7fe3944a17f307a78b4d2b30c5"
    };
    uint8_t key16[16];
    uint8_t schedule[176];
    Aes_Key key;
    uint32_t round;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the FIPS-197 §5.2 key is sixteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the §5.2 key schedules");

    for (round = 0; round < 11; round++)
        Test_Expect(Test_ParseHex(roundHex[round], schedule + 16 * round, 16) == 16,
               "each published round key is sixteen octets");

    for (round = 0; round < 11; round++)
        Test_ExpectOctets("each published round key sits at its own index in the schedule",
                          key.roundKey + 16 * round, 16, schedule + 16 * round, 16);

    Test_ExpectOctets("Aes_Key::key is the key the schedule was built from", key.key, 16, key16, 16);
    Test_ExpectOctets("all 176 octets of Aes_Key::roundKey are the §5.2 schedule, in order",
                      key.roundKey, 176, schedule, 176);
}

/* FIPS-197 appendix C.1, the only plaintext/ciphertext pair the standard prints: the forward
   block cipher over the schedule checked above (docs/internals/refs.md: "the suite's block KAT is
   appendix C.1"). */
static void aesDoBlockCheck(void)
{
    static const char *keyHex = "000102030405060708090a0b0c0d0e0f";
    static const char *plainHex = "00112233445566778899aabbccddeeff";
    static const char *cipherHex = "69c4e0d86a7b0430d8cdb78070b4c55a";
    uint8_t key16[16];
    uint8_t plain[16];
    uint8_t cipher[16];
    uint8_t out[16];
    Aes_Key key;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the appendix C.1 key is sixteen octets");
    Test_Expect(Test_ParseHex(plainHex, plain, 16) == 16, "the appendix C.1 plaintext is sixteen octets");
    Test_Expect(Test_ParseHex(cipherHex, cipher, 16) == 16, "the appendix C.1 ciphertext is sixteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the appendix C.1 key schedules");

    Aes_EncryptBlock(&key, plain, out);

    Test_ExpectOctets("FIPS-197 appendix C.1: the vector plaintext encrypts to the vector ciphertext",
                      out, 16, cipher, 16);
}

/* CTR (SP 800-38A §6.5, vector F.5.1): the keystream is the block cipher on the counter, the
   counter is the caller's own 128-bit big-endian integer and is left at its final value, and
   the final partial block consumes only the keystream octets the message needs. */
static void aesDoCtrCheck(void)
{
    static const char *keyHex = "2b7e151628aed2a6abf7158809cf4f3c";
    static const char *ctrHex = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";
    static const char *inHex = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                               "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
    static const char *outHex = "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                                "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee";
    static const char *ctrFinalHex = "f0f1f2f3f4f5f6f7f8f9fafbfcfdff03";
    static const char *carryCtrHex = "000000000000000000000000000000ff";
    static const char *carryOutHex = "a18c80e3b93a86d370cfef49ce429bd5a0733521fefc4ce22b1981d3ec0df91c82";
    static const char *carryNextHex = "00000000000000000000000000000100";
    static const char *carryFinalHex = "00000000000000000000000000000102";
    uint8_t key16[16];
    uint8_t counter[16];
    uint8_t next[16];
    uint8_t in[64];
    uint8_t out[64];
    uint8_t want[64];
    uint8_t zeros[33];
    uint8_t tail[17];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the SP 800-38A key is sixteen octets");
    Test_Expect(Test_ParseHex(ctrHex, counter, 16) == 16, "the SP 800-38A counter is sixteen octets");
    Test_Expect(Test_ParseHex(inHex, in, 64) == 64, "the F.5.1 input is sixty-four octets");
    Test_Expect(Test_ParseHex(outHex, want, 64) == 64, "the F.5.1 output is sixty-four octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the SP 800-38A key schedules");

    Aes_CtrCipher(&key, counter, in, 64, out);

    Test_ExpectOctets("SP 800-38A F.5.1: sixty-four octets encrypt to the published ciphertext", out, 64,
                      want, 64);

    Test_Expect(Test_ParseHex(ctrFinalHex, next, 16) == 16, "the F.5.1 final counter is sixteen octets");
    Test_ExpectOctets("the caller's counter is left where four blocks put it", counter, 16, next, 16);

    /* A run that starts one octet below a carry: 33 octets is three counter blocks, the first
       of them the last one the low octet can hold, so the 17-octet tail is only right if the
       carry propagated through the whole 128-bit integer rather than the last octet alone. */
    for (octet = 0; octet < 33; octet++)
        zeros[octet] = 0;

    Test_Expect(Test_ParseHex(carryCtrHex, counter, 16) == 16, "the carry counter is sixteen octets");
    Test_Expect(Test_ParseHex(carryOutHex, want, 64) == 33, "the carry run is thirty-three octets");

    Aes_CtrCipher(&key, counter, zeros, 33, out);

    Test_ExpectOctets("a counter at ...00ff crosses 2^8 and the run past the crossing is the published one",
                      out, 33, want, 33);

    Test_Expect(Test_ParseHex(carryFinalHex, next, 16) == 16,
                "the counter three blocks on is sixteen octets");
    Test_ExpectOctets("the caller's counter is left at ...0102: ...00ff plus the three blocks the run used",
                      counter, 16, next, 16);

    /* The same 17 octets asked for directly, starting at the counter the carry produced, ...0100:
       the keystream belongs to the counter, not to the run's position in it. */
    Test_Expect(Test_ParseHex(carryNextHex, counter, 16) == 16,
           "the octet the crossing produced is ...0100, and the tail starts there");

    Aes_CtrCipher(&key, counter, zeros + 16, 17, tail);

    Test_ExpectOctets("the tail octets are what the counter the carry produced yields on its own",
                      tail, 17, want + 16, 17);

    /* The stated zero-length case: nothing is read, nothing is written, and the counter is
       not touched because no block was encrypted. */
    Test_Expect(Test_ParseHex(ctrHex, counter, 16) == 16, "an empty run starts at the F.5.1 counter");

    for (octet = 0; octet < 64; octet++)
        out[octet] = 0xAA;

    Aes_CtrCipher(&key, counter, in, 0, out);

    Test_Expect(out[0] == 0xAA && out[63] == 0xAA, "an inLen of zero writes nothing to out");

    Test_Expect(Test_ParseHex(ctrHex, next, 16) == 16, "the F.5.1 counter again");
    Test_ExpectOctets("an inLen of zero leaves the counter where the caller put it", counter, 16, next, 16);
}

/* CMAC (SP 800-38B §2.1, vector D.1): the same key at four message lengths, which is the whole
   branch structure of the mode (an empty message, a partial final block padded with 0x80 and
   XORed with K2, a final block that is exact and XORed with K1) and a truncated tag, which is
   the leftmost octets of the full one and not a second computation. */
static void aesDoCmacCheck(void)
{
    static const char *keyHex = "2b7e151628aed2a6abf7158809cf4f3c";
    static const char *emptyTagHex = "bb1d6929e95937287fa37d129b756746";
    static const char *m2Hex = "6bc1bee22e409f96e93d7e117393172a";
    static const char *t2Hex = "070a16b46b4d4144f79bdd9dd04a287c";
    static const char *m3Hex = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                               "30c81c46a35ce411";
    static const char *t3Hex = "dfa66747de9ae63030ca32611497c827";
    static const char *m4Hex = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                               "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
    static const char *t4Hex = "51f0bebf7e3b9d92fc49741779363cfe";
    uint8_t key16[16];
    uint8_t message[64];
    uint8_t mic[16];
    uint8_t want[16];
    Aes_Key key;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the SP 800-38B key is sixteen octets");
    Test_Expect(Test_ParseHex(m2Hex, message, 64) == 16, "the D.1 one-block message is sixteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the SP 800-38B key schedules");

    /* The empty message is a zero length against a valid buffer, which is what the header states. */
    Test_Expect(Aes_Cmac(&key, message, 0, mic, 16) == ZB_OK, "an empty message is a legal CMAC input");
    Test_Expect(Test_ParseHex(emptyTagHex, want, 16) == 16, "the D.1 empty-message tag is sixteen octets");
    Test_ExpectOctets("SP 800-38B D.1: the empty message MACs to the published tag", mic, 16, want, 16);

    Test_Expect(Aes_Cmac(&key, message, 16, mic, 16) == ZB_OK, "a message on the block boundary is legal");
    Test_Expect(Test_ParseHex(t2Hex, want, 16) == 16, "the D.1 one-block tag is sixteen octets");
    Test_ExpectOctets("SP 800-38B D.1: sixteen octets MAC to the published tag", mic, 16, want, 16);
    Test_Expect(Aes_Cmac(&key, message, 16, mic, 8) == ZB_OK, "a micLen of eight is inside the stated range");
    Test_ExpectOctets("a truncated tag is the leftmost octets of the full one", mic, 8, want, 8);

    Test_Expect(Test_ParseHex(m3Hex, message, 64) == 40, "the D.1 partial message is forty octets");
    Test_Expect(Aes_Cmac(&key, message, 40, mic, 16) == ZB_OK, "a partial final block is legal");
    Test_Expect(Test_ParseHex(t3Hex, want, 16) == 16, "the D.1 partial-message tag is sixteen octets");
    Test_ExpectOctets("SP 800-38B D.1: forty octets MAC to the published tag", mic, 16, want, 16);

    Test_Expect(Test_ParseHex(m4Hex, message, 64) == 64, "the D.1 four-block message is sixty-four octets");
    Test_Expect(Aes_Cmac(&key, message, 64, mic, 16) == ZB_OK, "a message of four whole blocks is legal");
    Test_Expect(Test_ParseHex(t4Hex, want, 16) == 16, "the D.1 four-block tag is sixteen octets");
    Test_ExpectOctets("SP 800-38B D.1: sixty-four octets MAC to the published tag", mic, 16, want, 16);
}

/* The argument errors the header states for the functions this suite covers: a 0 pointer where
   the header names one, and a micLen outside {8..16}. A refusal changes nothing, which is what
   makes it a refusal rather than a partial answer. */
static void aesDoArgumentCheck(void)
{
    static const char *keyHex = "2b7e151628aed2a6abf7158809cf4f3c";
    static const char *m2Hex = "6bc1bee22e409f96e93d7e117393172a";
    static const char *untouchedHex = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    uint8_t key16[16];
    uint8_t message[16];
    uint8_t mic[16];
    uint8_t want[16];
    Aes_Key key;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the argument-check key is sixteen octets");
    Test_Expect(Test_ParseHex(m2Hex, message, 16) == 16, "the argument-check message is sixteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the argument-check key schedules");

    Test_Expect(Aes_KeySchedule(0, key16) == ZB_ERR_BAD_ARGUMENT, "a 0 key object is refused");
    Test_Expect(Aes_KeySchedule(&key, 0) == ZB_ERR_BAD_ARGUMENT, "a 0 key octet string is refused");
    Test_ExpectOctets("the refused schedule left the key object alone", key.key, 16, key16, 16);

    Test_Expect(Test_ParseHex(untouchedHex, mic, 16) == 16,
           "the tag buffer starts as a pattern no refusal may write over");

    Test_Expect(Aes_Cmac(&key, 0, 16, mic, 16) == ZB_ERR_BAD_ARGUMENT,
                "a 0 in is refused whatever the length");
    Test_Expect(Aes_Cmac(&key, 0, 0, mic, 16) == ZB_ERR_BAD_ARGUMENT,
                "a 0 in with a zero length is refused too");
    Test_Expect(Aes_Cmac(&key, message, 16, 0, 16) == ZB_ERR_BAD_ARGUMENT, "a 0 mic is refused");
    Test_Expect(Aes_Cmac(&key, message, 16, mic, 7) == ZB_ERR_BAD_ARGUMENT,
           "a micLen below {8..16} is refused");
    Test_Expect(Aes_Cmac(&key, message, 16, mic, 17) == ZB_ERR_BAD_ARGUMENT,
           "a micLen above {8..16} is refused");

    Test_Expect(Test_ParseHex(untouchedHex, want, 16) == 16, "the same pattern, for the comparison");
    Test_ExpectOctets("no refused CMAC wrote any part of the tag", mic, 16, want, 16);

    Test_Expect(Aes_Cmac(&key, message, 16, mic, 8) == ZB_OK, "micLen 8 is the low end of the stated range");
    Test_Expect(Aes_Cmac(&key, message, 16, mic, 16) == ZB_OK, "micLen 16 is the high end of it");
}

/* CCM* encrypt (IEEE 802.15.4-2024 Annex B.3, and for these tag lengths the same construction
   as SP 800-38C): the three shorter appendix C vectors, all four on one key. What they pin down
   is the shape of B0: the nonce lengths that make L 8, 7 and 3, the tag lengths 4, 6 and 8, and
   a payload of one partial block, one whole block and two whole blocks. */
static void aesDoCcmVectorsCheck(void)
{
    static const char *keyHex = "404142434445464748494a4b4c4d4e4f";
    static const char *c1NonceHex = "10111213141516";
    static const char *c1AdataHex = "0001020304050607";
    static const char *c1InHex = "20212223";
    static const char *c1CtHex = "7162015b";
    static const char *c1MicHex = "4dac255d";
    static const char *c2NonceHex = "1011121314151617";
    static const char *c2AdataHex = "000102030405060708090a0b0c0d0e0f";
    static const char *c2InHex = "202122232425262728292a2b2c2d2e2f";
    static const char *c2CtHex = "d2a1f0e051ea5f62081a7792073d593d";
    static const char *c2MicHex = "1fc64fbfaccd";
    static const char *c3NonceHex = "101112131415161718191a1b";
    static const char *c3AdataHex = "000102030405060708090a0b0c0d0e0f10111213";
    static const char *c3InHex = "202122232425262728292a2b2c2d2e2f3031323334353637";
    static const char *c3CtHex = "e3b201a9f5b71a7a9b1ceaeccd97e70b6176aad9a4428aa5";
    static const char *c3MicHex = "484392fbc1b09951";
    uint8_t key16[16];
    uint8_t nonce[13];
    uint8_t adata[24];
    uint8_t in[24];
    uint8_t out[24];
    uint8_t want[24];
    uint8_t mic[16];
    Aes_Key key;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the SP 800-38C key is sixteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the SP 800-38C key schedules");

    Test_Expect(Test_ParseHex(c1NonceHex, nonce, 13) == 7, "the C.1 nonce is seven octets");
    Test_Expect(Test_ParseHex(c1AdataHex, adata, 24) == 8, "the C.1 associated data is eight octets");
    Test_Expect(Test_ParseHex(c1InHex, in, 24) == 4, "the C.1 payload is four octets");

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 7, adata, 8, in, 4, 4, out, mic) == ZB_OK,
           "C.1: a 7-octet nonce is inside {7..13}, giving L = 8");

    Test_Expect(Test_ParseHex(c1CtHex, want, 24) == 4, "the C.1 ciphertext is four octets");
    Test_ExpectOctets("SP 800-38C C.1: four octets encrypt to the published ciphertext", out, 4, want, 4);

    Test_Expect(Test_ParseHex(c1MicHex, want, 24) == 4, "the C.1 tag is four octets");
    Test_ExpectOctets("SP 800-38C C.1: the same run authenticates with the published tag", mic, 4, want, 4);

    Test_Expect(Test_ParseHex(c2NonceHex, nonce, 13) == 8, "the C.2 nonce is eight octets");
    Test_Expect(Test_ParseHex(c2AdataHex, adata, 24) == 16, "the C.2 associated data is sixteen octets");
    Test_Expect(Test_ParseHex(c2InHex, in, 24) == 16, "the C.2 payload is sixteen octets");

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 8, adata, 16, in, 16, 6, out, mic) == ZB_OK,
           "C.2: an 8-octet nonce, giving L = 7");

    Test_Expect(Test_ParseHex(c2CtHex, want, 24) == 16, "the C.2 ciphertext is sixteen octets");
    Test_ExpectOctets("SP 800-38C C.2: one whole payload block encrypts to the published ciphertext",
                      out, 16, want, 16);

    Test_Expect(Test_ParseHex(c2MicHex, want, 24) == 6, "the C.2 tag is six octets");
    Test_ExpectOctets("SP 800-38C C.2: and authenticates with the published six-octet tag", mic, 6, want, 6);

    Test_Expect(Test_ParseHex(c3NonceHex, nonce, 13) == 12, "the C.3 nonce is twelve octets");
    Test_Expect(Test_ParseHex(c3AdataHex, adata, 24) == 20, "the C.3 associated data is twenty octets");
    Test_Expect(Test_ParseHex(c3InHex, in, 24) == 24, "the C.3 payload is twenty-four octets");

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 12, adata, 20, in, 24, 8, out, mic) == ZB_OK,
           "C.3: a 12-octet nonce, giving L = 3");

    Test_Expect(Test_ParseHex(c3CtHex, want, 24) == 24, "the C.3 ciphertext is twenty-four octets");
    Test_ExpectOctets("SP 800-38C C.3: two whole payload blocks encrypt to the published ciphertext",
                      out, 24, want, 24);

    Test_Expect(Test_ParseHex(c3MicHex, want, 24) == 8, "the C.3 tag is eight octets");
    Test_ExpectOctets("SP 800-38C C.3: and authenticate with the published eight-octet tag", mic, 8, want, 8);
}

/* SP 800-38C C.4, the vector that makes the associated data longer than the 2-octet length field
   can carry: 256 copies of the counting pattern is 65536 octets, past 65280, so A' begins with
   0xff 0xfe and the 4-octet length. The 13-octet nonce gives L = 2, which is the width that
   length has to be written in, and the tag is the 14-octet one this vector publishes. The
   buffer is built in a loop rather than written out as hex. */
static void aesDoCcmLongAdataCheck(void)
{
    static const char *keyHex = "404142434445464748494a4b4c4d4e4f";
    static const char *nonceHex = "101112131415161718191a1b1c";
    static const char *patternHex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                                    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
                                    "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
                                    "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
                                    "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
                                    "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
                                    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
                                    "e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";
    static const char *inHex = "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
    static const char *ctHex = "69915dad1e84c6376a68c2967e4dab615ae0fd1faec44cc484828529463ccf72";
    static const char *micHex = "b4ac6bec93e8598e7f0dadbcea5b";
    static uint8_t adata[65536];
    uint8_t key16[16];
    uint8_t nonce[13];
    uint8_t pattern[256];
    uint8_t in[32];
    uint8_t out[32];
    uint8_t want[32];
    uint8_t mic[16];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the C.4 key is sixteen octets");
    Test_Expect(Test_ParseHex(nonceHex, nonce, 13) == 13, "the C.4 nonce is thirteen octets");
    Test_Expect(Test_ParseHex(patternHex, pattern, 256) == 256,
                "the C.4 associated-data pattern is 256 octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the C.4 key schedules");

    for (octet = 0; octet < 65536; octet++)
        adata[octet] = pattern[octet % 256];

    Test_Expect(Test_ParseHex(inHex, in, 32) == 32, "the C.4 payload is thirty-two octets");

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, adata, 65536, in, 32, 14, out, mic) == ZB_OK,
           "C.4: 65536 octets of associated data, past the 2-octet length field, are accepted");

    Test_Expect(Test_ParseHex(ctHex, want, 32) == 32, "the C.4 ciphertext is thirty-two octets");
    Test_ExpectOctets("SP 800-38C C.4: thirty-two octets over a 65536-octet A' encrypt to the published ciphertext",
                      out, 32, want, 32);

    Test_Expect(Test_ParseHex(micHex, want, 32) == 14, "the C.4 tag is fourteen octets");
    Test_ExpectOctets("SP 800-38C C.4: and authenticate with the published fourteen-octet tag", mic, 14, want,
                      14);
}

/* IEEE 802.15.4-2024 C.3.2.2, the worked example the standard prints for a security suite that
   authenticates without confidentiality: an empty payload, so no P' block and no keystream at
   all, and the whole answer is the tag. The out buffer is prefilled to show that a zero-length
   payload writes nothing. */
static void aesDoCcmMicOnlyCheck(void)
{
    static const char *keyHex = "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf";
    static const char *nonceHex = "acde4800000000010000000502";
    static const char *adataHex = "08d0842143010000000048deac020500000055cf000051525354";
    static const char *micHex = "223bc1ec841ab553";
    uint8_t key16[16];
    uint8_t nonce[13];
    uint8_t adata[26];
    uint8_t in[8];
    uint8_t out[8];
    uint8_t mic[8];
    uint8_t want[8];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the C.3.2.2 key is sixteen octets");
    Test_Expect(Test_ParseHex(nonceHex, nonce, 13) == 13, "the C.3.2.2 nonce is thirteen octets");
    Test_Expect(Test_ParseHex(adataHex, adata, 26) == 26, "the C.3.2.2 associated data is twenty-six octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the C.3.2.2 key schedules");

    for (octet = 0; octet < 8; octet++)
    {
        in[octet] = 0;
        out[octet] = 0xAA;
    }

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, adata, 26, in, 0, 8, out, mic) == ZB_OK,
           "an empty payload against a valid buffer is a legal CCM* input");

    Test_Expect(Test_ParseHex(micHex, want, 8) == 8, "the C.3.2.2 tag is eight octets");
    Test_ExpectOctets("IEEE 802.15.4-2024 C.3.2.2: the authenticated frame header MACs to the published tag",
                      mic, 8, want, 8);

    Test_Expect(out[0] == 0xAA && out[7] == 0xAA, "a zero-length payload writes no ciphertext at all");
}

/* The Zigbee shape of CCM*: a 13-octet nonce, so L = 2, and a 4-octet tag, what a Zigbee data
   frame is protected with. The last two runs are the same payload under the two forms of
   associated data the mode allows: sixteen octets of it, and none at all. The keystream never
   sees the associated data, so the ciphertext is identical while the tag is not: only B0's
   Adata flag differs. */
static void aesDoCcmZigbeeCheck(void)
{
    static const char *keyHex = "00112233445566778899aabbccddeeff";
    static const char *nonceHex = "0102030405060708090a0b0c0d";
    static const char *adataHex = "a5a4a3a2a1a0";
    static const char *inHex = "101112131415161718191a1b1c1d1e1f2021";
    static const char *ctHex = "b51cf98453976944892e6513bd606da863ce";
    static const char *micHex = "9fe0b106";
    static const char *zerosCtHex = "a50deb9747827f5391377f08a17d73b7";
    static const char *zerosMicHex = "96de0f50";
    static const char *noAdataMicHex = "3a19e191";
    uint8_t key16[16];
    uint8_t nonce[13];
    uint8_t adata[16];
    uint8_t in[18];
    uint8_t zeros[18];
    uint8_t out[18];
    uint8_t mic[16];
    uint8_t want[18];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the Zigbee key is sixteen octets");
    Test_Expect(Test_ParseHex(nonceHex, nonce, 13) == 13, "the Zigbee nonce is thirteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the Zigbee key schedules");

    for (octet = 0; octet < 18; octet++)
        zeros[octet] = 0;

    Test_Expect(Test_ParseHex(adataHex, adata, 16) == 6, "the Zigbee associated data is six octets");
    Test_Expect(Test_ParseHex(inHex, in, 18) == 18, "the Zigbee payload is eighteen octets");

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, adata, 6, in, 18, 4, out, mic) == ZB_OK,
           "a 6-octet associated data and an 18-octet payload encrypt");

    Test_Expect(Test_ParseHex(ctHex, want, 18) == 18, "the published ciphertext is eighteen octets");
    Test_ExpectOctets("the Zigbee vector: the payload encrypts to the published ciphertext", out, 18, want,
                      18);

    Test_Expect(Test_ParseHex(micHex, want, 18) == 4, "the published tag is four octets");
    Test_ExpectOctets("and authenticates with the published tag", mic, 4, want, 4);

    for (octet = 0; octet < 16; octet++)
        adata[octet] = 0;

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, adata, 16, zeros, 16, 4, out, mic) == ZB_OK,
           "sixteen octets of associated data over a sixteen-octet payload encrypt");

    Test_Expect(Test_ParseHex(zerosCtHex, want, 18) == 16, "that run's ciphertext is sixteen octets");
    Test_ExpectOctets("sixteen zero payload octets encrypt to the published ciphertext", out, 16, want, 16);

    Test_Expect(Test_ParseHex(zerosMicHex, want, 18) == 4, "and its tag is four octets");
    Test_ExpectOctets("which authenticates with the published tag", mic, 4, want, 4);

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, 0, 0, zeros, 16, 4, out, mic) == ZB_OK,
           "no associated data at all (a 0 pointer with a 0 length) is legal");

    Test_Expect(Test_ParseHex(zerosCtHex, want, 18) == 16,
                "the same published ciphertext, for the run with none");
    Test_ExpectOctets("the ciphertext does not change when the associated data goes away: CTR never sees it",
                      out, 16, want, 16);

    Test_Expect(Test_ParseHex(noAdataMicHex, want, 18) == 4, "the tag of the run without associated data");
    Test_ExpectOctets("while the tag does change, because B0 carries no Adata flag", mic, 4, want, 4);
}

/* CCM* decrypt (Annex B.3.3) over the Zigbee vector: the inverse of the run above returns the
   payload, and a tag that does not match is refused with nothing written to out, which is what
   makes an authentication failure safe rather than a partially decrypted frame. */
static void aesDoCcmDecryptCheck(void)
{
    static const char *keyHex = "00112233445566778899aabbccddeeff";
    static const char *nonceHex = "0102030405060708090a0b0c0d";
    static const char *adataHex = "a5a4a3a2a1a0";
    static const char *inHex = "101112131415161718191a1b1c1d1e1f2021";
    static const char *ctHex = "b51cf98453976944892e6513bd606da863ce";
    static const char *micHex = "9fe0b106";
    uint8_t key16[16];
    uint8_t nonce[13];
    uint8_t adata[6];
    uint8_t plain[18];
    uint8_t cipher[18];
    uint8_t tag[4];
    uint8_t ct[18];
    uint8_t mic[4];
    uint8_t out[18];
    uint8_t want[18];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the decrypt key is sixteen octets");
    Test_Expect(Test_ParseHex(nonceHex, nonce, 13) == 13, "the decrypt nonce is thirteen octets");
    Test_Expect(Test_ParseHex(adataHex, adata, 6) == 6, "the decrypt associated data is six octets");
    Test_Expect(Test_ParseHex(inHex, plain, 18) == 18, "the decrypt payload is eighteen octets");
    Test_Expect(Test_ParseHex(ctHex, cipher, 18) == 18, "the published ciphertext is eighteen octets");
    Test_Expect(Test_ParseHex(micHex, tag, 4) == 4, "the published tag is four octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the decrypt key schedules");

    for (octet = 0; octet < 18; octet++)
    {
        out[octet] = 0xAA;
        want[octet] = 0xAA;
    }

    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, adata, 6, cipher, 18, tag, 4, out) == ZB_OK,
           "the published ciphertext verifies against the published tag");

    Test_ExpectOctets("and the plaintext it writes is the published payload", out, 18, plain, 18);

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, adata, 6, plain, 18, 4, ct, mic) == ZB_OK,
           "the payload encrypts, for the round trip");

    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, adata, 6, ct, 18, mic, 4, out) == ZB_OK,
           "and what that produced decrypts");

    Test_ExpectOctets("the round trip returns the payload octet for octet", out, 18, plain, 18);

    mic[3] = (uint8_t)(mic[3] ^ 0x01);

    for (octet = 0; octet < 18; octet++)
        out[octet] = 0xAA;

    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, adata, 6, ct, 18, mic, 4, out) == ZB_ERR_AUTH,
           "one corrupted tag octet is an authentication failure");

    Test_ExpectOctets("and a refused decrypt wrote nothing to out", out, 18, want, 18);

    Test_Expect(Test_ParseHex(micHex, mic, 4) == 4, "the tag good again, for the ciphertext case");

    ct[0] = (uint8_t)(ct[0] ^ 0x01);

    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, adata, 6, ct, 18, mic, 4, out) == ZB_ERR_AUTH,
           "one corrupted ciphertext octet is too: it decrypts to a plaintext the tag does not match");

    Test_ExpectOctets("the sentinel survives, so nothing was written before the tag was checked", out, 18,
                      want, 18);
}

/* Zigbee Core R23.1 Annex C.3, the CCM* encryption the Zigbee Core prints for itself: a
   13-octet nonce, 8 octets of associated data, a 23-octet payload and M = 8, the suite's
   first published vector whose payload is neither a multiple of sixteen nor smaller than one
   block, so the CBC-MAC ends on a partial P' block and the keystream on a partial counter
   block. The annex prints its whole intermediate chain beside the answer (B0, the CBC-MAC
   chain X1..X4, S0, the unmasked T and the emitted U) and marks its vectors as provided as
   is, pending independent validation; re-deriving that chain outside this tree reproduces it
   octet for octet, and what the suite asserts is the pair the annex emits: the ciphertext
   and the tag U, which on the wire is the single 31-octet c. */
static void aesDoCcmZigbeeCoreEncryptCheck(void)
{
    static const char *keyHex = "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf";
    static const char *nonceHex = "a0a1a2a3a4a5a6a70302010006";
    static const char *adataHex = "0001020304050607";
    static const char *inHex = "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e";
    static const char *ctHex = "1a55a36abb6c610d066b3375649cef10d4664ecad854a8";
    static const char *micHex = "0a895cc1d8ff9469";
    uint8_t key16[16];
    uint8_t nonce[13];
    uint8_t adata[8];
    uint8_t in[23];
    uint8_t out[23];
    uint8_t mic[8];
    uint8_t want[23];
    Aes_Key key;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the Annex C.3 key is sixteen octets");
    Test_Expect(Test_ParseHex(nonceHex, nonce, 13) == 13, "the Annex C.3 nonce is thirteen octets, so L = 2");
    Test_Expect(Test_ParseHex(adataHex, adata, 8) == 8, "the Annex C.3 associated data is eight octets");
    Test_Expect(Test_ParseHex(inHex, in, 23) == 23, "the Annex C.3 payload is twenty-three octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the Annex C.3 key schedules");

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, adata, 8, in, 23, 8, out, mic) == ZB_OK,
           "Annex C.3: a 23-octet payload with a 13-octet nonce and M = 8 encrypts");

    Test_Expect(Test_ParseHex(ctHex, want, 23) == 23, "the Annex C.3 ciphertext is twenty-three octets");
    Test_ExpectOctets("Zigbee R23.1 Annex C.3: the payload encrypts to the published ciphertext", out, 23,
                      want, 23);

    Test_Expect(Test_ParseHex(micHex, want, 8) == 8, "the Annex C.3 tag is eight octets");
    Test_ExpectOctets("Zigbee R23.1 Annex C.3: and authenticates with the published tag U", mic, 8, want, 8);
}

/* R23.1 Annex C.4, the inverse run the same annex prints over that same pair: the published
   ciphertext and tag decrypt to the published payload, which is the outcome the annex marks
   valid. The two refusals are the file's sentinel idiom: the output buffer starts as a
   pattern no refusal may write over, and is compared against it after each refusal. */
static void aesDoCcmZigbeeCoreDecryptCheck(void)
{
    static const char *keyHex = "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf";
    static const char *nonceHex = "a0a1a2a3a4a5a6a70302010006";
    static const char *adataHex = "0001020304050607";
    static const char *inHex = "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e";
    static const char *ctHex = "1a55a36abb6c610d066b3375649cef10d4664ecad854a8";
    static const char *micHex = "0a895cc1d8ff9469";
    uint8_t key16[16];
    uint8_t nonce[13];
    uint8_t adata[8];
    uint8_t plain[23];
    uint8_t cipher[23];
    uint8_t tag[8];
    uint8_t out[23];
    uint8_t want[23];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the Annex C.4 key is sixteen octets");
    Test_Expect(Test_ParseHex(nonceHex, nonce, 13) == 13, "the Annex C.4 nonce is thirteen octets");
    Test_Expect(Test_ParseHex(adataHex, adata, 8) == 8, "the Annex C.4 associated data is eight octets");
    Test_Expect(Test_ParseHex(inHex, plain, 23) == 23, "the Annex C.4 payload is twenty-three octets");
    Test_Expect(Test_ParseHex(ctHex, cipher, 23) == 23, "the Annex C.4 ciphertext is twenty-three octets");
    Test_Expect(Test_ParseHex(micHex, tag, 8) == 8, "the Annex C.4 tag is eight octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the Annex C.4 key schedules");

    for (octet = 0; octet < 23; octet++)
    {
        out[octet] = 0xAA;
        want[octet] = 0xAA;
    }

    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, adata, 8, cipher, 23, tag, 8, out) == ZB_OK,
           "the published ciphertext verifies against the published tag");

    Test_ExpectOctets("Zigbee R23.1 Annex C.4: the plaintext it writes is the published payload", out, 23,
                      plain, 23);

    tag[7] = (uint8_t)(tag[7] ^ 0x01);

    for (octet = 0; octet < 23; octet++)
        out[octet] = 0xAA;

    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, adata, 8, cipher, 23, tag, 8, out) == ZB_ERR_AUTH,
           "one corrupted tag octet is an authentication failure");

    Test_ExpectOctets("and a refused decrypt wrote nothing to out", out, 23, want, 23);

    Test_Expect(Test_ParseHex(micHex, tag, 8) == 8, "the tag good again, for the ciphertext case");

    cipher[0] = (uint8_t)(cipher[0] ^ 0x01);

    for (octet = 0; octet < 23; octet++)
        out[octet] = 0xAA;

    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, adata, 8, cipher, 23, tag, 8, out) == ZB_ERR_AUTH,
           "one corrupted ciphertext octet is too: it decrypts to a plaintext the tag does not match");

    Test_ExpectOctets("the sentinel survives there as well, so nothing was written before the tag was checked",
                      out, 23, want, 23);
}

/* The argument errors CCM* states: a nonce outside {7..13} (the low end because L = 1 is
   reserved) and a tag length outside {4,6,8,10,12,14,16}. A refusal writes nothing, and the
   legal minimum (no associated data, no payload, a 4-octet tag) is the mic-only form the
   C.3.2.2 example is built from. */
static void aesDoCcmArgumentCheck(void)
{
    static const char *keyHex = "00112233445566778899aabbccddeeff";
    static const char *nonceHex = "0102030405060708090a0b0c0d";
    static const char *untouchedHex = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    uint8_t key16[16];
    uint8_t nonce[16];
    uint8_t in[16];
    uint8_t out[16];
    uint8_t mic[16];
    uint8_t want[16];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the CCM* argument-check key is sixteen octets");
    Test_Expect(Test_ParseHex(nonceHex, nonce, 16) == 13, "the CCM* argument-check nonce is thirteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the CCM* argument-check key schedules");

    for (octet = 0; octet < 16; octet++)
        in[octet] = 0;

    Test_Expect(Test_ParseHex(untouchedHex, mic, 16) == 16,
           "the tag buffer starts as a pattern no refusal may write over");
    Test_Expect(Test_ParseHex(untouchedHex, out, 16) == 16, "and so does the ciphertext buffer");

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 6, in, 0, in, 4, 4, out, mic) == ZB_ERR_BAD_ARGUMENT,
           "a 6-octet nonce is below {7..13}");
    Test_Expect(Aes_CcmEncrypt(&key, nonce, 14, in, 0, in, 4, 4, out, mic) == ZB_ERR_BAD_ARGUMENT,
           "a 14-octet nonce is above it: that would leave L = 1, which the mode reserves");
    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, in, 0, in, 4, 5, out, mic) == ZB_ERR_BAD_ARGUMENT,
           "a 5-octet tag is outside {4,6,8,10,12,14,16}");
    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, 0, 1, in, 4, 4, out, mic) == ZB_ERR_BAD_ARGUMENT,
           "a 0 associated-data pointer with a non-zero length is refused");
    Test_Expect(Aes_CcmDecrypt(&key, nonce, 6, in, 0, in, 4, mic, 4, out) == ZB_ERR_BAD_ARGUMENT,
           "the decrypt side checks the same nonce");
    Test_Expect(Aes_CcmDecrypt(&key, nonce, 13, 0, 1, in, 4, mic, 4, out) == ZB_ERR_BAD_ARGUMENT,
           "and the same associated data");

    Test_Expect(Test_ParseHex(untouchedHex, want, 16) == 16, "the same pattern, for the comparison");
    Test_ExpectOctets("no refused CCM* call wrote any part of the tag", mic, 16, want, 16);
    Test_ExpectOctets("or any part of the ciphertext", out, 16, want, 16);

    Test_Expect(Aes_CcmEncrypt(&key, nonce, 13, 0, 0, in, 0, 4, out, mic) == ZB_OK,
           "no associated data, no payload and a 4-octet tag is the legal minimum");
}

/* Aes_KeyWipe: the whole object goes, not just the key octets a caller could read back,
   16 octets of key and 176 of schedule, 192 in all (src/crypto/aes.h). */
static void aesDoKeyWipeCheck(void)
{
    static const char *keyHex = "000102030405060708090a0b0c0d0e0f";
    uint8_t key16[16];
    uint8_t zeros[192];
    Aes_Key key;
    uint32_t octet;

    Test_Expect(Test_ParseHex(keyHex, key16, 16) == 16, "the wipe key is sixteen octets");
    Test_Expect(Aes_KeySchedule(&key, key16) == ZB_OK, "the wipe key schedules");
    Test_ExpectOctets("the scheduled object holds the key and its expansion", key.key, 16, key16, 16);

    for (octet = 0; octet < 192; octet++)
        zeros[octet] = 0;

    Aes_KeyWipe(&key);

    Test_ExpectOctets("the wipe zeroes the 16 octets of Aes_Key::key", key.key, 16, zeros, 16);
    Test_ExpectOctets("and the 176 octets of Aes_Key::roundKey", key.roundKey, 176, zeros, 176);
    Test_ExpectOctets("every octet the object is made of is zero, so no round key is left to read back",
                      (const uint8_t *)&key, 192, zeros, 192);
}

int main(void)
{
    Test_Begin("crypto_aes");

    aesDoKeyScheduleCheck();
    aesDoBlockCheck();
    aesDoCtrCheck();
    aesDoCmacCheck();
    aesDoArgumentCheck();
    aesDoCcmVectorsCheck();
    aesDoCcmLongAdataCheck();
    aesDoCcmMicOnlyCheck();
    aesDoCcmZigbeeCheck();
    aesDoCcmDecryptCheck();
    aesDoCcmZigbeeCoreEncryptCheck();
    aesDoCcmZigbeeCoreDecryptCheck();
    aesDoCcmArgumentCheck();
    aesDoKeyWipeCheck();

    return Test_Report();
}
