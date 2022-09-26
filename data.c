/* snac - A simple, minimalistic ActivityPub instance */
/* copyright (c) 2022 grunfink - MIT license */

#include "xs.h"
#include "xs_io.h"
#include "xs_json.h"
#include "xs_openssl.h"

#include "snac.h"

#include <time.h>
#include <glob.h>
#include <sys/stat.h>


int srv_open(char *basedir)
/* opens a server */
{
    int ret = 0;
    xs *cfg_file = NULL;
    FILE *f;
    d_char *error = NULL;

    srv_basedir = xs_str_new(basedir);

    if (xs_endswith(srv_basedir, "/"))
        srv_basedir = xs_crop(srv_basedir, 0, -1);

    cfg_file = xs_fmt("%s/server.json", basedir);

    if ((f = fopen(cfg_file, "r")) == NULL)
        error = xs_fmt("error opening '%s'", cfg_file);
    else {
        xs *cfg_data;

        /* read full config file */
        cfg_data = xs_readall(f);

        /* parse */
        srv_config = xs_json_loads(cfg_data);

        if (srv_config == NULL)
            error = xs_fmt("cannot parse '%s'", cfg_file);
        else {
            char *host;
            char *prefix;
            char *dbglvl;

            host   = xs_dict_get(srv_config, "host");
            prefix = xs_dict_get(srv_config, "prefix");
            dbglvl = xs_dict_get(srv_config, "dbglevel");

            if (host == NULL || prefix == NULL)
                error = xs_str_new("cannot get server data");
            else {
                srv_baseurl = xs_fmt("https://%s%s", host, prefix);

                dbglevel = (int) xs_number_get(dbglvl);

                if ((dbglvl = getenv("DEBUG")) != NULL) {
                    dbglevel = atoi(dbglvl);
                    error = xs_fmt("DEBUG level set to %d from environment", dbglevel);
                }

                ret = 1;
            }
        }
    }

    if (ret == 0 && error != NULL)
        srv_log(error);

    return ret;
}


void user_free(snac *snac)
/* frees a user snac */
{
    free(snac->uid);
    free(snac->basedir);
    free(snac->config);
    free(snac->key);
    free(snac->actor);
}


int user_open(snac *snac, char *uid)
/* opens a user */
{
    int ret = 0;

    memset(snac, '\0', sizeof(struct _snac));

    if (validate_uid(uid)) {
        xs *cfg_file;
        FILE *f;

        snac->uid = xs_str_new(uid);

        snac->basedir = xs_fmt("%s/user/%s", srv_basedir, uid);

        cfg_file = xs_fmt("%s/user.json", snac->basedir);

        if ((f = fopen(cfg_file, "r")) != NULL) {
            xs *cfg_data;

            /* read full config file */
            cfg_data = xs_readall(f);
            fclose(f);

            if ((snac->config = xs_json_loads(cfg_data)) != NULL) {
                xs *key_file = xs_fmt("%s/key.json", snac->basedir);

                if ((f = fopen(key_file, "r")) != NULL) {
                    xs *key_data;

                    key_data = xs_readall(f);
                    fclose(f);

                    if ((snac->key = xs_json_loads(key_data)) != NULL) {
                        snac->actor = xs_fmt("%s/%s", srv_baseurl, uid);
                        ret = 1;
                    }
                    else
                        srv_log(xs_fmt("cannot parse '%s'", key_file));
                }
                else
                    srv_log(xs_fmt("error opening '%s'", key_file));
            }
            else
                srv_log(xs_fmt("cannot parse '%s'", cfg_file));
        }
        else
            srv_debug(2, xs_fmt("error opening '%s'", cfg_file));
    }
    else
        srv_log(xs_fmt("invalid user '%s'", uid));

    if (!ret)
        user_free(snac);

    return ret;
}


d_char *user_list(void)
/* returns the list of user ids */
{
    d_char *list;
    xs *spec;
    glob_t globbuf;

    globbuf.gl_offs = 1;

    list = xs_list_new();
    spec = xs_fmt("%s/user/" "*", srv_basedir);

    if (glob(spec, 0, NULL, &globbuf) == 0) {
        int n;
        char *p;

        for (n = 0; (p = globbuf.gl_pathv[n]) != NULL; n++) {
            if ((p = strrchr(p, '/')) != NULL)
                list = xs_list_append(list, p + 1);
        }
    }

    globfree(&globbuf);

    return list;
}


float mtime(char *fn)
/* returns the mtime of a file or directory, or 0.0 */
{
    struct stat st;
    float r = 0.0;

    if (stat(fn, &st) != -1)
        r = (float)st.st_mtim.tv_sec;

    return r;
}


d_char *_follower_fn(snac *snac, char *actor)
{
    xs *md5 = xs_md5_hex(actor, strlen(actor));
    return xs_fmt("%s/followers/%s.json", snac->basedir, md5);
}


int follower_add(snac *snac, char *actor, char *msg)
/* adds a follower */
{
    int ret = 201; /* created */
    xs *fn = _follower_fn(snac, actor);
    FILE *f;

    if ((f = fopen(fn, "w")) != NULL) {
        xs *j = xs_json_dumps_pp(msg, 4);

        fwrite(j, 1, strlen(j), f);
        fclose(f);
    }
    else
        ret = 500;

    snac_debug(snac, 2, xs_fmt("follower_add %s %s", actor, fn));

    return ret;
}


int follower_del(snac *snac, char *actor)
/* deletes a follower */
{
    xs *fn = _follower_fn(snac, actor);

    unlink(fn);

    snac_debug(snac, 2, xs_fmt("follower_del %s %s", actor, fn));

    return 200;
}


int follower_check(snac *snac, char *actor)
/* checks if someone is a follower */
{
    xs *fn = _follower_fn(snac, actor);

    return !!(mtime(fn) != 0.0);
}


d_char *follower_list(snac *snac)
/* returns the list of followers */
{
    d_char *list;
    xs *spec;
    glob_t globbuf;

    list = xs_list_new();
    spec = xs_fmt("%s/followers/" "*.json", snac->basedir);

    if (glob(spec, 0, NULL, &globbuf) == 0) {
        int n;
        char *fn;

        for (n = 0; (fn = globbuf.gl_pathv[n]) != NULL; n++) {
            FILE *f;

            if ((f = fopen(fn, "r")) != NULL) {
                xs *j = xs_readall(f);
                xs *o = xs_json_loads(j);

                if (o != NULL)
                    list = xs_list_append(list, o);

                fclose(f);
            }
        }
    }

    globfree(&globbuf);

    return list;
}


d_char *_timeline_find_fn(snac *snac, char *id)
/* returns the file name of a timeline entry by its id */
{
    xs *md5  = xs_md5_hex(id, strlen(id));
    xs *spec = xs_fmt("%s/timeline/" "*-%s.json", snac->basedir, md5);
    glob_t globbuf;
    d_char *fn = NULL;

    if (glob(spec, 0, NULL, &globbuf) == 0 && globbuf.gl_pathc) {
        /* get just the first file */
        fn = xs_str_new(globbuf.gl_pathv[0]);
    }

    globfree(&globbuf);

    return fn;
}


int timeline_here(snac *snac, char *id)
/* checks if an object is already downloaded */
{
    xs *fn = _timeline_find_fn(snac, id);

    return fn != NULL;
}


d_char *timeline_find(snac *snac, char *id)
/* gets a message from the timeline by id */
{
    xs *fn  = _timeline_find_fn(snac, id);
    xs *msg = NULL;

    if (fn != NULL) {
        FILE *f;

        if ((f = fopen(fn, "r")) != NULL) {
            xs *j = xs_readall(f);

            msg = xs_json_loads(j);
            fclose(f);
        }
    }

    return msg;
}


void timeline_del(snac *snac, char *id)
/* deletes a message from the timeline */
{
    xs *fn = _timeline_find_fn(snac, id);

    if (fn != NULL) {
        xs *lfn = NULL;

        unlink(fn);
        snac_debug(snac, 1, xs_fmt("timeline_del %s", id));

        /* try to delete also from the local timeline */
        lfn = xs_replace(fn, "/timeline/", "/local/");

        if (unlink(lfn) != -1)
            snac_debug(snac, 1, xs_fmt("timeline_del (local) %s", id));
    }
}


d_char *timeline_get(snac *snac, char *fn)
/* gets a timeline entry by file name */
{
    d_char *d = NULL;
    FILE *f;

    if ((f = fopen(fn, "r")) != NULL) {
        xs *j = xs_readall(f);

        d = xs_json_loads(j);
        fclose(f);
    }

    return d;
}


d_char *timeline_list(snac *snac)
/* returns a list of the timeline filenames */
{
    d_char *list;
    xs *spec = xs_fmt("%s/timeline/" "*.json", snac->basedir);
    glob_t globbuf;
    int max;

    /* maximum number of items in the timeline */
    max = xs_number_get(xs_dict_get(srv_config, "max_timeline_entries"));

    list = xs_list_new();

    /* get the list in reverse order */
    if (glob(spec, 0, NULL, &globbuf) == 0) {
        int n;

        if (max > globbuf.gl_pathc)
            max = globbuf.gl_pathc;

        for (n = 0; n < max; n++) {
            char *fn = globbuf.gl_pathv[globbuf.gl_pathc - n - 1];

            list = xs_list_append(list, fn);
        }
    }

    globfree(&globbuf);

    return list;
}


d_char *_timeline_new_fn(snac *snac, char *id)
/* creates a new filename */
{
    xs *ntid = tid(0);
    xs *md5  = xs_md5_hex(id, strlen(id));

    return xs_fmt("%s/timeline/%s-%s.json", snac->basedir, ntid, md5);
}


void _timeline_write(snac *snac, char *id, char *msg, char *parent, char *referrer)
/* writes a timeline entry and refreshes the ancestors */
{
    xs *fn = _timeline_new_fn(snac, id);
    FILE *f;

    if ((f = fopen(fn, "w")) != NULL) {
        xs *j = xs_json_dumps_pp(msg, 4);

        fwrite(j, strlen(j), 1, f);
        fclose(f);

        snac_debug(snac, 1, xs_fmt("_timeline_write %s %s", id, fn));
    }

    /* related to this user? link to local timeline */
    if (xs_startswith(id, snac->actor) ||
        (!xs_is_null(parent) && xs_startswith(parent, snac->actor)) ||
        (!xs_is_null(referrer) && xs_startswith(referrer, snac->actor))) {
        xs *lfn = xs_replace(fn, "/timeline/", "/local/");
        link(fn, lfn);

        snac_debug(snac, 1, xs_fmt("_timeline_write (local) %s %s", id, lfn));
    }

    if (!xs_is_null(parent)) {
        /* update the parent, adding this id to its children list */
        xs *pfn   = _timeline_find_fn(snac, parent);
        xs *p_msg = NULL;

        if (pfn != NULL && (f = fopen(pfn, "r")) != NULL) {
            xs *j;

            j = xs_readall(f);
            fclose(f);

            p_msg = xs_json_loads(j);
        }

        if (p_msg == NULL)
            return;

        xs *meta     = xs_dup(xs_dict_get(p_msg, "_snac"));
        xs *children = xs_dup(xs_dict_get(meta,  "children"));

        /* add the child if it's not already there */
        if (xs_list_in(children, id) == -1)
            children = xs_list_append(children, id);

        /* re-store */
        meta  = xs_dict_set(meta,  "children", children);
        p_msg = xs_dict_set(p_msg, "_snac",    meta);

        xs *nfn = _timeline_new_fn(snac, parent);

        if ((f = fopen(nfn, "w")) != NULL) {
            xs *j = xs_json_dumps_pp(p_msg, 4);

            fwrite(j, strlen(j), 1, f);
            fclose(f);

            unlink(pfn);

            snac_debug(snac, 1,
                xs_fmt("_timeline_write updated parent %s %s", parent, nfn));

            /* try to do the same with the local */
            xs *olfn = xs_replace(pfn, "/timeline/", "/local/");

            if (unlink(olfn) != -1 || xs_startswith(id, snac->actor)) {
                xs *nlfn = xs_replace(nfn, "/timeline/", "/local/");

                link(nfn, nlfn);

                snac_debug(snac, 1,
                    xs_fmt("_timeline_write updated parent (local) %s %s", parent, nlfn));
            }
        }
        else
            return;

        /* now iterate all parents up, just renaming the files */
        xs *grampa = xs_dup(xs_dict_get(meta, "parent"));

        while (!xs_is_null(grampa)) {
            xs *gofn = _timeline_find_fn(snac, grampa);

            if (gofn == NULL)
                break;

            /* create the new filename */
            xs *gnfn = _timeline_new_fn(snac, grampa);

            rename(gofn, gnfn);

            snac_debug(snac, 1,
                xs_fmt("_timeline_write updated grampa %s %s", grampa, gnfn));

            /* try to do the same with the local */
            xs *golfn = xs_replace(gofn, "/timeline/", "/local/");

            if (unlink(golfn) != -1) {
                xs *gnlfn = xs_replace(gnfn, "/timeline/", "/local/");

                link(gnfn, gnlfn);

                snac_debug(snac, 1,
                    xs_fmt("_timeline_write updated grampa (local) %s %s", parent, gnlfn));
            }

            /* now open it and get its own parent */
            if ((f = fopen(gnfn, "r")) != NULL) {
                xs *j = xs_readall(f);
                fclose(f);

                xs *g_msg    = xs_json_loads(j);
                d_char *meta = xs_dict_get(g_msg, "_snac");
                d_char *p    = xs_dict_get(meta,  "parent");

                free(grampa);
                grampa = xs_dup(p);
            }
        }
    }
}


int timeline_add(snac *snac, char *id, char *o_msg, char *parent, char *referrer)
/* adds a message to the timeline */
{
    xs *pfn = _timeline_find_fn(snac, id);

    if (pfn != NULL) {
        snac_log(snac, xs_fmt("timeline_add refusing rewrite %s %s", id, pfn));
        return 0;
    }

    xs *msg = xs_dup(o_msg);
    xs *md;

    /* add new metadata */
    md = xs_json_loads("{"
        "\"children\":     [],"
        "\"liked_by\":     [],"
        "\"announced_by\": [],"
        "\"version\":      \"snac/2.x\","
        "\"referrer\":     null,"
        "\"parent\":       null"
    "}");

    if (!xs_is_null(parent))
        md = xs_dict_set(md, "parent", parent);

    if (!xs_is_null(referrer))
        md = xs_dict_set(md, "referrer", referrer);

    msg = xs_dict_set(msg, "_snac", md);

    _timeline_write(snac, id, msg, parent, referrer);

    snac_log(snac, xs_fmt("timeline_add %s", id));

    return 1;
}



void timeline_admire(snac *snac, char *id, char *admirer, int like)
/* updates a timeline entry with a new admiration */
{
    xs *ofn = _timeline_find_fn(snac, id);
    FILE *f;

    if (ofn != NULL && (f = fopen(ofn, "r")) != NULL) {
        xs *j1 = xs_readall(f);
        fclose(f);

        xs *msg  = xs_json_loads(j1);
        xs *meta = xs_dup(xs_dict_get(msg, "_snac"));
        xs *list;

        if (like)
            list = xs_dup(xs_dict_get(meta, "liked_by"));
        else
            list = xs_dup(xs_dict_get(meta, "announced_by"));

        /* add the admirer if it's not already there */
        if (xs_list_in(list, admirer) == -1)
            list = xs_list_append(list, admirer);

        /* set the admirer as the referrer */
        meta = xs_dict_set(meta, "referrer", admirer);

        /* re-store */
        if (like)
            meta = xs_dict_set(meta, "liked_by", list);
        else
            meta = xs_dict_set(meta, "announced_by", list);

        msg = xs_dict_set(msg, "_snac", meta);

        unlink(ofn);

        _timeline_write(snac, id, msg, xs_dict_get(meta, "parent"), admirer);

        snac_log(snac, xs_fmt("timeline_admire (%s) %s %s",
            like ? "Like" : "Announce", id, admirer));
    }
    else
        snac_log(snac, xs_fmt("timeline_admire ignored for unknown object %s", id));
}


d_char *_following_fn(snac *snac, char *actor)
{
    xs *md5 = xs_md5_hex(actor, strlen(actor));
    return xs_fmt("%s/following/%s.json", snac->basedir, md5);
}


int following_add(snac *snac, char *actor, char *msg)
/* adds to the following list */
{
    int ret = 201; /* created */
    xs *fn = _following_fn(snac, actor);
    FILE *f;

    if ((f = fopen(fn, "w")) != NULL) {
        xs *j = xs_json_dumps_pp(msg, 4);

        fwrite(j, 1, strlen(j), f);
        fclose(f);
    }
    else
        ret = 500;

    snac_debug(snac, 2, xs_fmt("following_add %s %s", actor, fn));

    return ret;
}


int following_del(snac *snac, char *actor)
/* someone is no longer following us */
{
    xs *fn = _following_fn(snac, actor);

    unlink(fn);

    snac_debug(snac, 2, xs_fmt("following_del %s %s", actor, fn));

    return 200;
}


int following_check(snac *snac, char *actor)
/* checks if someone is following us */
{
    xs *fn = _following_fn(snac, actor);

    return !!(mtime(fn) != 0.0);
}


d_char *_muted_fn(snac *snac, char *actor)
{
    xs *md5 = xs_md5_hex(actor, strlen(actor));
    return xs_fmt("%s/muted/%s.json", snac->basedir, md5);
}


void mute(snac *snac, char *actor)
/* mutes a moron */
{
    xs *fn = _muted_fn(snac, actor);
    FILE *f;

    if ((f = fopen(fn, "w")) != NULL) {
        fprintf(f, "%s\n", actor);
        fclose(f);

        snac_debug(snac, 2, xs_fmt("muted %s %s", actor, fn));
    }
}


void unmute(snac *snac, char *actor)
/* actor is no longer a moron */
{
    xs *fn = _muted_fn(snac, actor);

    unlink(fn);

    snac_debug(snac, 2, xs_fmt("unmuted %s %s", actor, fn));
}


int is_muted(snac *snac, char *actor)
/* check if someone is muted */
{
    xs *fn = _muted_fn(snac, actor);

    return !!(mtime(fn) != 0.0);
}


d_char *_actor_fn(snac *snac, char *actor)
/* returns the file name for an actor */
{
    xs *md5 = xs_md5_hex(actor, strlen(actor));
    return xs_fmt("%s/actors/%s.json", snac->basedir, md5);
}


int actor_add(snac *snac, char *actor, char *msg)
/* adds a follower */
{
    int ret = 201; /* created */
    xs *fn = _actor_fn(snac, actor);
    FILE *f;

    if ((f = fopen(fn, "w")) != NULL) {
        xs *j = xs_json_dumps_pp(msg, 4);

        fwrite(j, 1, strlen(j), f);
        fclose(f);
    }
    else
        ret = 500;

    snac_debug(snac, 2, xs_fmt("actor_add %s %s", actor, fn));

    return ret;
}


int actor_get(snac *snac, char *actor, d_char **data)
/* returns an already downloaded actor */
{
    xs *fn = _actor_fn(snac, actor);
    float t;
    float max_time;
    int status;
    FILE *f;

    t = mtime(fn);

    /* no mtime? there is nothing here */
    if (t == 0.0)
        return 404;

    /* maximum time for the actor data to be considered stale */
    max_time = 3600.0 * 36.0;

    if (t + max_time < (float) time(NULL)) {
        /* actor data exists but also stinks */

        if ((f = fopen(fn, "a")) != NULL) {
            /* write a blank at the end to 'touch' the file */
            fwrite(" ", 1, 1, f);
            fclose(f);
        }

        status = 205; /* "205: Reset Content" "110: Response Is Stale" */
    }
    else {
        /* it's still valid */
        status = 200;
    }

    if ((f = fopen(fn, "r")) != NULL) {
        xs *j = xs_readall(f);

        fclose(f);

        *data = xs_json_loads(j);
    }
    else
        status = 500;

    return status;
}


void enqueue_input(snac *snac, char *msg, char *req)
/* enqueues an input message */
{
    xs *ntid = tid(0);
    xs *fn   = xs_fmt("%s/queue/%s.json", snac->basedir, ntid);
    xs *tfn  = xs_fmt("%s.tmp", fn);
    FILE *f;

    if ((f = fopen(tfn, "w")) != NULL) {
        xs *qmsg = xs_dict_new();
        xs *j;

        qmsg = xs_dict_append(qmsg, "type",   "input");
        qmsg = xs_dict_append(qmsg, "object", msg);
        qmsg = xs_dict_append(qmsg, "req",    req);

        j = xs_json_dumps_pp(qmsg, 4);

        fwrite(j, strlen(j), 1, f);
        fclose(f);

        rename(tfn, fn);

        snac_debug(snac, 1, xs_fmt("enqueue_input %s", fn));
    }
}


void enqueue_output(snac *snac, char *msg, char *actor, int retries)
/* enqueues an output message for an actor */
{
    if (strcmp(actor, snac->actor) == 0) {
        snac_debug(snac, 1, xs_str_new("enqueue refused to myself"));
        return;
    }

    int qrt  = xs_number_get(xs_dict_get(srv_config, "query_retry_minutes"));
    xs *ntid = tid(retries * 60 * qrt);
    xs *fn   = xs_fmt("%s/queue/%s.json", snac->basedir, ntid);
    xs *tfn  = xs_fmt("%s.tmp", fn);
    FILE *f;

    if ((f = fopen(tfn, "w")) != NULL) {
        xs *qmsg = xs_dict_new();
        xs *rn   = xs_number_new(retries);
        xs *j;

        qmsg = xs_dict_append(qmsg, "type",    "output");
        qmsg = xs_dict_append(qmsg, "actor",   actor);
        qmsg = xs_dict_append(qmsg, "object",  msg);
        qmsg = xs_dict_append(qmsg, "retries", rn);

        j = xs_json_dumps_pp(qmsg, 4);

        fwrite(j, strlen(j), 1, f);
        fclose(f);

        rename(tfn, fn);

        snac_debug(snac, 1, xs_fmt("enqueue_output %s %s %d", actor, fn, retries));
    }
}


d_char *queue(snac *snac)
/* returns a list with filenames that can be dequeued */
{
    xs *spec = xs_fmt("%s/queue/" "*.json", snac->basedir);
    d_char *list = xs_list_new();
    glob_t globbuf;
    time_t t = time(NULL);

    if (glob(spec, 0, NULL, &globbuf) == 0) {
        int n;
        char *p;

        for (n = 0; (p = globbuf.gl_pathv[n]) != NULL; n++) {
            /* get the retry time from the basename */
            char *bn = strrchr(p, '/');
            time_t t2 = atol(bn + 1);

            if (t2 > t)
                snac_debug(snac, 2, xs_fmt("queue not yet time for %s", p));
            else {
                list = xs_list_append(list, p);
                snac_debug(snac, 2, xs_fmt("queue ready for %s", p));
            }
        }
    }

    globfree(&globbuf);

    return list;
}


d_char *dequeue(snac *snac, char *fn)
/* dequeues a message */
{
    FILE *f;
    d_char *obj = NULL;

    if ((f = fopen(fn, "r")) != NULL) {
        /* delete right now */
        unlink(fn);

        xs *j = xs_readall(f);
        obj = xs_json_loads(j);

        fclose(f);
    }

    return obj;
}
