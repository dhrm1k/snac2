/* snac - A simple, minimalistic ActivityPub instance */
/* copyright (c) 2022 grunfink - MIT license */

#define XS_IMPLEMENTATION

#include "xs.h"
#include "xs_io.h"
#include "xs_encdec.h"
#include "xs_json.h"
#include "xs_curl.h"
#include "xs_openssl.h"
#include "xs_socket.h"
#include "xs_httpd.h"

#include "snac.h"

#include <sys/time.h>


d_char *srv_basedir = NULL;
d_char *srv_config  = NULL;
d_char *srv_baseurl = NULL;
int     srv_running = 0;

int dbglevel = 0;


d_char *xs_time(char *fmt, int local)
/* returns a d_char with a formated time */
{
    time_t t = time(NULL);
    struct tm tm;
    char tmp[64];

    if (local)
        localtime_r(&t, &tm);
    else
        gmtime_r(&t, &tm);

    strftime(tmp, sizeof(tmp), fmt, &tm);

    return xs_str_new(tmp);
}


d_char *tid(int offset)
/* returns a time-based Id */
{
    struct timeval tv;
    struct timezone tz;

    gettimeofday(&tv, &tz);

    return xs_fmt("%10d.%06d", tv.tv_sec + offset, tv.tv_usec);
}


void srv_debug(int level, d_char *str)
/* logs a debug message */
{
    xs *msg = str;

    if (xs_str_in(msg, srv_basedir) != -1) {
        /* replace basedir with ~ */
        xs *o_str = msg;
        msg = xs_replace(o_str, srv_basedir, "~");
    }

    if (dbglevel >= level) {
        xs *tm = xs_local_time("%H:%M:%S");
        fprintf(stderr, "%s %s\n", tm, msg);
    }
}


int validate_uid(char *uid)
/* returns if uid is a valid identifier */
{
    while (*uid) {
        if (!(isalnum(*uid) || *uid == '_'))
            return 0;

        uid++;
    }

    return 1;
}


void snac_debug(snac *snac, int level, d_char *str)
/* prints a user debugging information */
{
    xs *o_str = str;
    d_char *msg = xs_fmt("[%s] %s", snac->uid, o_str);

    if (xs_str_in(msg, snac->basedir) != -1) {
        /* replace long basedir references with ~ */
        xs *o_str = msg;
        msg = xs_replace(o_str, snac->basedir, "~");
    }

    srv_debug(level, msg);
}


d_char *hash_password(char *uid, char *passwd, char *nonce)
/* hashes a password */
{
    xs *d_nonce = NULL;
    xs *combi;
    xs *hash;

    if (nonce == NULL)
        nonce = d_nonce = xs_fmt("%08x", random());

    combi = xs_fmt("%s:%s:%s", nonce, uid, passwd);
    hash  = xs_sha1_hex(combi, strlen(combi));

    return xs_fmt("%s:%s", nonce, hash);
}


int check_password(char *uid, char *passwd, char *hash)
/* checks a password */
{
    int ret = 0;
    xs *spl = xs_split_n(hash, ":", 1);

    if (xs_list_len(spl) == 2) {
        xs *n_hash = hash_password(uid, passwd, xs_list_get(spl, 0));

        ret = (strcmp(hash, n_hash) == 0);
    }

    return ret;
}
