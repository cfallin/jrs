#include "crypto.h"
#include <openssl/rc4.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <stdio.h>

//#define DEBUG

#ifdef DEBUG

static void
dump_bytes(uint8_t *bytes, int len)
{
    char buf[1024];
    char *p = buf;
    int remaining = sizeof(buf);
    int i;
    for (i = 0; i < len; i++) {
        int l = snprintf(p, remaining, "%02x ", bytes[i]);
        p += l;
        remaining -= l;
    }
    jrs_log("%s", buf);
}

#endif

/*
 * Protocol:
 *
 * First each end sends a nonce to the other end.
 *
 * Then each end replies with RC4(secret, SHA1(other end's nonce, secret)).
 *
 * If each end gets a valid reply (which can be checked, since each end knows
 * its own nonce and the shared secret), then the connection is established
 * using SHA1(other end's nonce, secret) as the key to send.
 * */

int
crypto_start(jrs_sockstream_t *sockstream, char *secretfile, crypto_state_t *state)
{
    FILE *f;

    /* read the secret file and set up the state. */
    f = fopen(secretfile, "r");
    if (!f) {
        perror("error");
        jrs_log("could not read secret file '%s'.", secretfile);
        return 1;
    }

    memset(state->secret, 0, sizeof(state->secret));
    state->secretlen = fread(state->secret, 1, sizeof(state->secret), f);
    fclose(f);

    if (state->secretlen < 1)
        return 1;

    /* get a random nonce. */
    if (!RAND_bytes(state->nonce, NONCELEN))
        return 1;

#ifdef DEBUG
    jrs_log("sending nonce:");
    dump_bytes(state->nonce, NONCELEN);
#endif

    /* send the nonce to the other end. */
    jrs_sockstream_write(sockstream, state->nonce, NONCELEN);

    state->state = CRYPTO_STATE_WAITNONCE;
    state->othernonce_bytes = 0;

    return 0;
}

Crypto_Status
crypto_wait(jrs_sockstream_t *sockstream, crypto_state_t *state)
{
    SHA_CTX sha;
    RC4_KEY rc4;

    switch (state->state) {

        case CRYPTO_STATE_WAITNONCE:
            /* try to read bytes of the other's nonce until we block */
            while (state->othernonce_bytes < NONCELEN) {
                unsigned char buf[1];
                int readbyte = jrs_sockstream_read(sockstream, buf, 1);
                if (readbyte == 0)
                    break;
                state->othernonce[state->othernonce_bytes++] = buf[0];
            }

            /* do we have the full nonce now? */
            if (state->othernonce_bytes == NONCELEN) {

#ifdef DEBUG
                jrs_log("got other's nonce:");
                dump_bytes(state->othernonce, NONCELEN);
#endif
                
                char sha1[20], sha1_rc4[20];

                /* compute the reply: RC4(secret, SHA1(othernonce, secret)) */
                SHA1_Init(&sha);
                SHA1_Update(&sha, (const void *)state->othernonce, NONCELEN);
                SHA1_Update(&sha, (const void *)state->secret, state->secretlen);
                SHA1_Final(sha1, &sha);

                RC4_set_key(&rc4, state->secretlen, state->secret);
                RC4(&rc4, sizeof(sha1), sha1, sha1_rc4);

#ifdef DEBUG
                jrs_log("RC4(secret, SHA1(other's nonce, secret)) =");
                dump_bytes(sha1_rc4, 20);
#endif

                /* send the reply */
                jrs_sockstream_write(sockstream, sha1_rc4, sizeof(sha1_rc4));

                /* switch state */
                state->state = CRYPTO_STATE_WAITREPLY;
                state->reply_bytes = 0;
            }
            break;

        case CRYPTO_STATE_WAITREPLY:
            /* try to read bytes of the other's reply until we block */
            while (state->reply_bytes < 20) {
                unsigned char buf[1];
                int readbyte = jrs_sockstream_read(sockstream, buf, 1);
                if (readbyte == 0)
                    break;
                state->reply[state->reply_bytes++] = buf[0];
            }

            /* do we have the full reply now? */
            if (state->reply_bytes == 20) {

#ifdef DEBUG
                jrs_log("got reply:");
                dump_bytes(state->reply, 20);
#endif

                char sha1[20], sha1_rc4[20];

                /* compute the expected reply: RC4(secret, SHA1(our nonce,
                 * secret)) */
                SHA1_Init(&sha);
                SHA1_Update(&sha, (const void *)state->nonce, NONCELEN);
                SHA1_Update(&sha, (const void *)state->secret,
                        state->secretlen);
                SHA1_Final(sha1, &sha);

                RC4_set_key(&rc4, state->secretlen, state->secret);
                RC4(&rc4, sizeof(sha1), sha1, sha1_rc4);

#ifdef DEBUG
                jrs_log("expected reply:");
                dump_bytes(sha1_rc4, 20);
#endif

                /* compare expected reply with actual reply */
                if (memcmp(sha1_rc4, state->reply, 20)) {
                    state->state = CRYPTO_STATE_BAD;
#ifdef DEBUG
                    jrs_log("bad reply");
#endif
                    return CRYPTO_BAD;
                }
                else {
                    RC4_KEY sendkey, recvkey;

                    /* at this point, set up encryption on the stream with
                     * the key SHA1(othernonce, secret) to send and
                     * SHA1(nonce, secret) to receive */

                    unsigned char sha1[20];

                    SHA1_Init(&sha);
                    SHA1_Update(&sha, (const void *)state->othernonce, NONCELEN);
                    SHA1_Update(&sha, (const void *)state->secret,
                            state->secretlen);
                    SHA1_Final(sha1, &sha);
                    RC4_set_key(&sendkey, 20, sha1);

                    SHA1_Init(&sha);
                    SHA1_Update(&sha, (const void *)state->nonce, NONCELEN);
                    SHA1_Update(&sha, (const void *)state->secret,
                            state->secretlen);
                    SHA1_Final(sha1, &sha);
                    RC4_set_key(&recvkey, 20, sha1);

                    jrs_sockstream_set_rc4(sockstream, &sendkey, &recvkey);

#ifdef DEBUG
                    jrs_log("crypto link established");
#endif
                    state->state = CRYPTO_STATE_ESTABLISHED;
                    return CRYPTO_ESTABLISHED;
                }
            }

            break;

        case CRYPTO_STATE_ESTABLISHED:
            return CRYPTO_ESTABLISHED;
    }

    return CRYPTO_WAITING;
}
