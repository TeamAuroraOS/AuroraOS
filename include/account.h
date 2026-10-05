#ifndef AURORA_ACCOUNT_H
#define AURORA_ACCOUNT_H

#include "aurora.h"

/* Aurora accounts (docs/account.md). The console links itself to an account
 * with the OAuth device flow (RFC 8628) on 3ds.aurora3ds.xyz over the joined
 * Wi-Fi network, and keeps the token in SD:/Aurora/account.txt. Accounts are
 * made on account.aurora3ds.xyz, which the link screens send the user to. */

#define ACCOUNT_HOST "3ds.aurora3ds.xyz"
#define ACCOUNT_SITE "account.aurora3ds.xyz"
#define ACCOUNT_FILE "Aurora/account.txt"

/* Reads ACCOUNT_FILE. The card must not be mounted. */
void account_load(void);

/* 1 while a token is kept. */
int account_linked(void);

/* The linked account's username as last seen, "" when not linked or not yet
 * known. */
const char *account_name(void);

/* The token for an Authorization header, or 0 when not linked. */
const char *account_token(void);

/* Deletes the token, as when the server says it was revoked. 1 when the file
 * is gone. */
int account_forget(void);

/* Settings > Aurora Account: link (or create) an account, check it, unlink.
 * `owner` is the console owner's name from USER.dat, used for the name the
 * console links under ("Nate's New 3DS"). Returns with both screens left for
 * the caller to redraw. */
void account_screen(const char *owner);

#endif
