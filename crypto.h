#ifndef _CRYPTO_H_
#define _CRYPTO_H_

#include "sockstream.h"

#define NONCELEN 32 /* bytes = 256 bits */

typedef enum Crypto_State {
    CRYPTO_STATE_WAITNONCE,
    CRYPTO_STATE_WAITREPLY,
    CRYPTO_STATE_ESTABLISHED,
    CRYPTO_STATE_BAD,
} Crypto_State;

typedef struct crypto_state_t {
    unsigned char secret[1024];
    int secretlen;
    unsigned char nonce[NONCELEN], othernonce[NONCELEN];
    int othernonce_bytes;
    unsigned char reply[20];
    int reply_bytes;
    Crypto_State state;
} crypto_state_t;

/* sends initial message immediately after socket is opened to hand-shake an
 * encrypted connection. */
int crypto_start(jrs_sockstream_t *sockstream, char *secretfile, crypto_state_t *state);

typedef enum Crypto_Status {
    CRYPTO_WAITING,
    CRYPTO_ESTABLISHED,
    CRYPTO_BAD,
} Crypto_Status;

/* waits for reply after initial message. indicates that either
 * reply is still in progress, reply is complete and OK, or reply
 * is bad. */
Crypto_Status crypto_wait(jrs_sockstream_t *sockstream, crypto_state_t *state);

#endif
