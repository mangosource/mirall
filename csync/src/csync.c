/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2008-2013 by Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2012-2013 by Klaas Freitag <freitag@owncloud.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_csync.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdbool.h>

#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif
#ifdef HAVE_SYS_ICONV_H
#include <sys/iconv.h>
#endif

#include "c_lib.h"
#include "csync_private.h"
#include "csync_exclude.h"
#include "csync_statedb.h"
#include "csync_time.h"
#include "csync_util.h"
#include "csync_misc.h"
#include "std/c_private.h"

#include "csync_update.h"
#include "csync_reconcile.h"

#include "vio/csync_vio.h"

#include "csync_log.h"
#include "csync_rename.h"
#include "c_jhash.h"


// Breaking the abstraction for fun and profit.
#include "csync_owncloud.h"

static int _key_cmp(const void *key, const void *data) {
  uint64_t a;
  csync_file_stat_t *b;

  a = *(uint64_t *) (key);
  b = (csync_file_stat_t *) data;

  if (a < b->phash) {
    return -1;
  } else if (a > b->phash) {
    return 1;
  }

  return 0;
}

static int _data_cmp(const void *key, const void *data) {
  csync_file_stat_t *a, *b;

  a = (csync_file_stat_t *) key;
  b = (csync_file_stat_t *) data;

  if (a->phash < b->phash) {
    return -1;
  } else if (a->phash > b->phash) {
    return 1;
  }

  return 0;
}

int csync_create(CSYNC **csync, const char *local, const char *remote) {
  CSYNC *ctx;
  size_t len = 0;
  char *home;
  int rc;

  ctx = c_malloc(sizeof(CSYNC));
  if (ctx == NULL) {
    return -1;
  }

  ctx->status_code = CSYNC_STATUS_OK;

  /* remove trailing slashes */
  len = strlen(local);
  while(len > 0 && local[len - 1] == '/') --len;

  ctx->local.uri = c_strndup(local, len);
  if (ctx->local.uri == NULL) {
    ctx->status_code = CSYNC_STATUS_MEMORY_ERROR;
    free(ctx);
    return -1;
  }

  /* remove trailing slashes */
  len = strlen(remote);
  while(len > 0 && remote[len - 1] == '/') --len;

  ctx->remote.uri = c_strndup(remote, len);
  if (ctx->remote.uri == NULL) {
    ctx->status_code = CSYNC_STATUS_MEMORY_ERROR;
    free(ctx);
    return -1;
  }

  ctx->status_code = CSYNC_STATUS_OK;
  ctx->options.local_only_mode = false;

  ctx->pwd.uid = getuid();
  ctx->pwd.euid = geteuid();

  home = csync_get_user_home_dir();
  if (home == NULL) {
    SAFE_FREE(ctx->local.uri);
    SAFE_FREE(ctx->remote.uri);
    SAFE_FREE(ctx);
    errno = ENOMEM;
    ctx->status_code = CSYNC_STATUS_MEMORY_ERROR;
    return -1;
  }

  rc = asprintf(&ctx->options.config_dir, "%s/%s", home, CSYNC_CONF_DIR);
  SAFE_FREE(home);
  if (rc < 0) {
    SAFE_FREE(ctx->local.uri);
    SAFE_FREE(ctx->remote.uri);
    SAFE_FREE(ctx);
    errno = ENOMEM;
    ctx->status_code = CSYNC_STATUS_MEMORY_ERROR;
    return -1;
  }

  ctx->local.list     = 0;
  ctx->remote.list    = 0;
  ctx->current_fs = NULL;

  ctx->abort = false;

  *csync = ctx;
  return 0;
}

int csync_init(CSYNC *ctx) {
  int rc;
  char *config = NULL;

  if (ctx == NULL) {
    errno = EBADF;
    return -1;
  }

  ctx->status_code = CSYNC_STATUS_OK;

  /* Do not initialize twice */
  if (ctx->status & CSYNC_STATUS_INIT) {
    return 1;
  }

  /* check for uri */
  if (csync_fnmatch("owncloud://*", ctx->remote.uri, 0) == 0 && csync_fnmatch("ownclouds://*", ctx->remote.uri, 0) == 0) {
      ctx->status_code = CSYNC_STATUS_NO_MODULE;
      rc = -1;
      goto out;
  }

  ctx->local.type = LOCAL_REPLICA;

  if ( !ctx->options.local_only_mode) {
      owncloud_init(csync_get_auth_callback(ctx), csync_get_userdata(ctx));
      ctx->remote.type = REMOTE_REPLICA;
  } else {
    ctx->remote.type = LOCAL_REPLICA;
  }

  if (c_rbtree_create(&ctx->local.tree, _key_cmp, _data_cmp) < 0) {
    ctx->status_code = CSYNC_STATUS_TREE_ERROR;
    rc = -1;
    goto out;
  }

  if (c_rbtree_create(&ctx->remote.tree, _key_cmp, _data_cmp) < 0) {
    ctx->status_code = CSYNC_STATUS_TREE_ERROR;
    rc = -1;
    goto out;
  }

  ctx->status = CSYNC_STATUS_INIT;

  csync_set_module_property(ctx, "csync_context", ctx);

  /* initialize random generator */
  srand(time(NULL));

  rc = 0;

out:
  SAFE_FREE(config);
  return rc;
}

int csync_update(CSYNC *ctx) {
  int rc = -1;
  struct timespec start, finish;

  if (ctx == NULL) {
    errno = EBADF;
    return -1;
  }
  ctx->status_code = CSYNC_STATUS_OK;

  /* create/load statedb */
  if (! csync_is_statedb_disabled(ctx)) {
    rc = asprintf(&ctx->statedb.file, "%s/.csync_journal.db",
                  ctx->local.uri);
    if (rc < 0) {
        ctx->status_code = CSYNC_STATUS_MEMORY_ERROR;
        return rc;
    }
    CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "Journal: %s", ctx->statedb.file);

    if (csync_statedb_load(ctx, ctx->statedb.file, &ctx->statedb.db) < 0) {
      ctx->status_code = CSYNC_STATUS_STATEDB_LOAD_ERROR;
      rc = -1;
      return rc;
    }
  }

  ctx->status_code = CSYNC_STATUS_OK;

  csync_memstat_check();

  if (!ctx->excludes) {
      CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "No exclude file loaded or defined!");
  }

  /* update detection for local replica */
  csync_gettime(&start);
  ctx->current = LOCAL_REPLICA;
  ctx->replica = ctx->local.type;

  rc = csync_ftw(ctx, ctx->local.uri, csync_walker, MAX_DEPTH);
  if (rc < 0) {
    if(ctx->status_code == CSYNC_STATUS_OK)
        ctx->status_code = csync_errno_to_status(errno, CSYNC_STATUS_UPDATE_ERROR);
    return -1;
  }

  csync_gettime(&finish);

  CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG,
            "Update detection for local replica took %.2f seconds walking %zu files.",
            c_secdiff(finish, start), c_rbtree_size(ctx->local.tree));
  csync_memstat_check();

  if (rc < 0) {
    ctx->status_code = CSYNC_STATUS_TREE_ERROR;
    return -1;
  }

  /* update detection for remote replica */
  if( ! ctx->options.local_only_mode ) {
    csync_gettime(&start);
    ctx->current = REMOTE_REPLICA;
    ctx->replica = ctx->remote.type;

    rc = csync_ftw(ctx, ctx->remote.uri, csync_walker, MAX_DEPTH);
    if (rc < 0) {
        if(ctx->status_code == CSYNC_STATUS_OK)
            ctx->status_code = csync_errno_to_status(errno, CSYNC_STATUS_UPDATE_ERROR);
        return -1;
    }


    csync_gettime(&finish);

    CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG,
              "Update detection for remote replica took %.2f seconds "
              "walking %zu files.",
              c_secdiff(finish, start), c_rbtree_size(ctx->remote.tree));
    csync_memstat_check();
  }
  ctx->status |= CSYNC_STATUS_UPDATE;

  return 0;
}

int csync_reconcile(CSYNC *ctx) {
  int rc = -1;
  struct timespec start, finish;

  if (ctx == NULL) {
    errno = EBADF;
    return -1;
  }
  ctx->status_code = CSYNC_STATUS_OK;

  /* Reconciliation for local replica */
  csync_gettime(&start);

  ctx->current = LOCAL_REPLICA;
  ctx->replica = ctx->local.type;

  rc = csync_reconcile_updates(ctx);

  csync_gettime(&finish);

  CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG,
      "Reconciliation for local replica took %.2f seconds visiting %zu files.",
      c_secdiff(finish, start), c_rbtree_size(ctx->local.tree));

  if (rc < 0) {
      if (!CSYNC_STATUS_IS_OK(ctx->status_code)) {
          ctx->status_code = csync_errno_to_status( errno, CSYNC_STATUS_RECONCILE_ERROR );
      }
      return -1;
  }

  /* Reconciliation for remote replica */
  csync_gettime(&start);

  ctx->current = REMOTE_REPLICA;
  ctx->replica = ctx->remote.type;

  rc = csync_reconcile_updates(ctx);

  csync_gettime(&finish);

  CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG,
      "Reconciliation for remote replica took %.2f seconds visiting %zu files.",
      c_secdiff(finish, start), c_rbtree_size(ctx->remote.tree));

  if (rc < 0) {
      if (!CSYNC_STATUS_IS_OK(ctx->status_code)) {
          ctx->status_code = csync_errno_to_status(errno,  CSYNC_STATUS_RECONCILE_ERROR );
      }
      return -1;
  }

  ctx->status |= CSYNC_STATUS_RECONCILE;

  return 0;
}

/*
 * local visitor which calls the user visitor with repacked stat info.
 */
static int _csync_treewalk_visitor(void *obj, void *data) {
    int rc = 0;
    csync_file_stat_t *cur         = NULL;
    CSYNC *ctx                     = NULL;
    c_rbtree_visit_func *visitor   = NULL;
    _csync_treewalk_context *twctx = NULL;
    TREE_WALK_FILE trav;
    c_rbtree_t *other_tree = NULL;
    c_rbnode_t *other_node = NULL;

    cur = (csync_file_stat_t *) obj;
    ctx = (CSYNC *) data;

    if (ctx == NULL) {
      return -1;
    }

    /* we need the opposite tree! */
    switch (ctx->current) {
    case LOCAL_REPLICA:
        other_tree = ctx->remote.tree;
        break;
    case REMOTE_REPLICA:
        other_tree = ctx->local.tree;
        break;
    default:
        break;
    }

    other_node = c_rbtree_find(other_tree, &cur->phash);

    if (!other_node) {
        /* Check the renamed path as well. */
        int len;
        uint64_t h = 0;
        char *renamed_path = csync_rename_adjust_path(ctx, cur->path);

        if (!c_streq(renamed_path, cur->path)) {
            len = strlen( renamed_path );
            h = c_jhash64((uint8_t *) renamed_path, len, 0);
            other_node = c_rbtree_find(other_tree, &h);
        }
        SAFE_FREE(renamed_path);
    }

    if (obj == NULL || data == NULL) {
      ctx->status_code = CSYNC_STATUS_PARAM_ERROR;
      return -1;
    }
    ctx->status_code = CSYNC_STATUS_OK;

    twctx = (_csync_treewalk_context*) ctx->callbacks.userdata;
    if (twctx == NULL) {
      ctx->status_code = CSYNC_STATUS_PARAM_ERROR;
      return -1;
    }

    if (twctx->instruction_filter > 0 &&
        !(twctx->instruction_filter & cur->instruction) ) {
        return 0;
    }

    visitor = (c_rbtree_visit_func*)(twctx->user_visitor);
    if (visitor != NULL) {
      trav.path         = cur->path;
      trav.size         = cur->size;
      trav.modtime      = cur->modtime;
      trav.uid          = cur->uid;
      trav.gid          = cur->gid;
      trav.mode         = cur->mode;
      trav.type         = cur->type;
      trav.instruction  = cur->instruction;
      trav.rename_path  = cur->destpath;
      trav.etag         = cur->etag;
      trav.file_id      = cur->file_id;

      trav.error_status = cur->error_status;
      trav.should_update_etag = cur->should_update_etag;

      if( other_node ) {
          csync_file_stat_t *other_stat = (csync_file_stat_t*)other_node->data;
          trav.other.etag = other_stat->etag;
          trav.other.file_id = other_stat->file_id;
          trav.other.instruction = other_stat->instruction;
          trav.other.modtime = other_stat->modtime;
          trav.other.size = other_stat->size;
      } else {
          trav.other.etag = 0;
          trav.other.file_id = 0;
          trav.other.instruction = CSYNC_INSTRUCTION_NONE;
          trav.other.modtime = 0;
          trav.other.size = 0;
      }

      rc = (*visitor)(&trav, twctx->userdata);
      cur->instruction = trav.instruction;
      if (trav.etag != cur->etag) {
          SAFE_FREE(cur->etag);
          cur->etag = c_strdup(trav.etag);
      }

      return rc;
    }
    ctx->status_code = CSYNC_STATUS_PARAM_ERROR;
    return -1;
}

/*
 * treewalk function, called from its wrappers below.
 *
 * it encapsulates the user visitor function, the filter and the userdata
 * into a treewalk_context structure and calls the rb treewalk function,
 * which calls the local _csync_treewalk_visitor in this module.
 * The user visitor is called from there.
 */
static int _csync_walk_tree(CSYNC *ctx, c_rbtree_t *tree, csync_treewalk_visit_func *visitor, int filter)
{
    _csync_treewalk_context tw_ctx;
    int rc = -1;

    if (ctx == NULL) {
        errno = EBADF;
        return rc;
    }

    if (visitor == NULL || tree == NULL) {
        ctx->status_code = CSYNC_STATUS_PARAM_ERROR;
        return rc;
    }
    
    tw_ctx.userdata = ctx->callbacks.userdata;
    tw_ctx.user_visitor = visitor;
    tw_ctx.instruction_filter = filter;

    ctx->callbacks.userdata = &tw_ctx;

    rc = c_rbtree_walk(tree, (void*) ctx, _csync_treewalk_visitor);
    if( rc < 0 ) {
      if( ctx->status_code == CSYNC_STATUS_OK )
          ctx->status_code = csync_errno_to_status(errno, CSYNC_STATUS_TREE_ERROR);
    }
    ctx->callbacks.userdata = tw_ctx.userdata;

    return rc;
}

/*
 * wrapper function for treewalk on the remote tree
 */
int csync_walk_remote_tree(CSYNC *ctx,  csync_treewalk_visit_func *visitor, int filter)
{
    c_rbtree_t *tree = NULL;
    int rc = -1;

    if(ctx != NULL) {
        ctx->status_code = CSYNC_STATUS_OK;
        ctx->current = REMOTE_REPLICA;
        tree = ctx->remote.tree;
    }

    /* all error handling in the called function */
    rc = _csync_walk_tree(ctx, tree, visitor, filter);
    return rc;
}

/*
 * wrapper function for treewalk on the local tree
 */
int csync_walk_local_tree(CSYNC *ctx, csync_treewalk_visit_func *visitor, int filter)
{
    c_rbtree_t *tree = NULL;
    int rc = -1;

    if (ctx != NULL) {
        ctx->status_code = CSYNC_STATUS_OK;
        ctx->current = LOCAL_REPLICA;
        tree = ctx->local.tree;
    }

    /* all error handling in the called function */
    rc = _csync_walk_tree(ctx, tree, visitor, filter);
    return rc;  
}

static void _tree_destructor(void *data) {
  csync_file_stat_t *freedata = NULL;

  freedata = (csync_file_stat_t *) data;
  csync_file_stat_free(freedata);
}

/* reset all the list to empty.
 * used by csync_commit and csync_destroy */
static void _csync_clean_ctx(CSYNC *ctx)
{
    c_list_t * walk;

    /* destroy the rbtrees */
    if (c_rbtree_size(ctx->local.tree) > 0) {
        c_rbtree_destroy(ctx->local.tree, _tree_destructor);
    }

    if (c_rbtree_size(ctx->remote.tree) > 0) {
        c_rbtree_destroy(ctx->remote.tree, _tree_destructor);
    }

    csync_rename_destroy(ctx);

    for (walk = c_list_last(ctx->local.ignored_cleanup); walk != NULL; walk = c_list_prev(walk)) {
        SAFE_FREE(walk->data);
    }
    for (walk = c_list_last(ctx->remote.ignored_cleanup); walk != NULL; walk = c_list_prev(walk)) {
        SAFE_FREE(walk->data);
    }

    /* free memory */
    c_rbtree_free(ctx->local.tree);
    c_list_free(ctx->local.list);
    c_list_free(ctx->local.ignored_cleanup);
    c_rbtree_free(ctx->remote.tree);
    c_list_free(ctx->remote.list);
    c_list_free(ctx->remote.ignored_cleanup);

    ctx->remote.list = 0;
    ctx->local.list = 0;
    ctx->remote.ignored_cleanup = 0;
    ctx->local.ignored_cleanup = 0;

    SAFE_FREE(ctx->statedb.file);
}

int csync_commit(CSYNC *ctx) {
  int rc = 0;

  if (ctx == NULL) {
    return -1;
  }

  ctx->status_code = CSYNC_STATUS_OK;

  if (ctx->statedb.db != NULL
      && csync_statedb_close(ctx) < 0) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "ERR: closing of statedb failed.");
    rc = -1;
  }
  ctx->statedb.db = NULL;

  rc = csync_vio_commit(ctx);
  if (rc < 0) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "commit failed: %s",
              ctx->error_string ? ctx->error_string : "");
    goto out;
  }

  _csync_clean_ctx(ctx);

  ctx->remote.read_from_db = 0;
  ctx->read_from_db_disabled = 0;


  /* Create new trees */
  rc = c_rbtree_create(&ctx->local.tree, _key_cmp, _data_cmp);
  if (rc < 0) {
    ctx->status_code = CSYNC_STATUS_TREE_ERROR;
    goto out;
  }

  rc = c_rbtree_create(&ctx->remote.tree, _key_cmp, _data_cmp);
  if (rc < 0) {
    ctx->status_code = CSYNC_STATUS_TREE_ERROR;
    goto out;
  }


  ctx->status = CSYNC_STATUS_INIT;
  SAFE_FREE(ctx->error_string);

  rc = 0;

out:
  return rc;
}

int csync_destroy(CSYNC *ctx) {
  int rc = 0;

  if (ctx == NULL) {
    errno = EBADF;
    return -1;
  }
  ctx->status_code = CSYNC_STATUS_OK;

  if (ctx->statedb.db != NULL
      && csync_statedb_close(ctx) < 0) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "ERR: closing of statedb failed.");
    rc = -1;
  }
  ctx->statedb.db = NULL;

  /* destroy exclude list */
  csync_exclude_destroy(ctx);

  _csync_clean_ctx(ctx);

  SAFE_FREE(ctx->local.uri);
  SAFE_FREE(ctx->remote.uri);
  SAFE_FREE(ctx->options.config_dir);
  SAFE_FREE(ctx->error_string);

#ifdef WITH_ICONV
  c_close_iconv();
#endif

  SAFE_FREE(ctx);

  return rc;
}

/* Check if csync is the required version or get the version string. */
const char *csync_version(int req_version) {
  if (req_version <= LIBCSYNC_VERSION_INT) {
    return CSYNC_STRINGIFY(LIBCSYNC_VERSION);
  }

  return NULL;
}

int csync_add_exclude_list(CSYNC *ctx, const char *path) {
  if (ctx == NULL || path == NULL) {
    return -1;
  }

  return csync_exclude_load(ctx, path);
}

void csync_clear_exclude_list(CSYNC *ctx)
{
    csync_exclude_clear(ctx);
}

const char *csync_get_config_dir(CSYNC *ctx) {
  if (ctx == NULL) {
    return NULL;
  }

  return ctx->options.config_dir;
}

int csync_set_config_dir(CSYNC *ctx, const char *path) {
  if (ctx == NULL || path == NULL) {
    return -1;
  }

  SAFE_FREE(ctx->options.config_dir);
  ctx->options.config_dir = c_strdup(path);
  if (ctx->options.config_dir == NULL) {
    ctx->status_code = CSYNC_STATUS_MEMORY_ERROR;
    return -1;
  }

  return 0;
}

int csync_enable_statedb(CSYNC *ctx) {
  if (ctx == NULL) {
    return -1;
  }
  ctx->status_code = CSYNC_STATUS_OK;

  if (ctx->status & CSYNC_STATUS_INIT) {
    fprintf(stderr, "This function must be called before initialization.");
    ctx->status_code = CSYNC_STATUS_CSYNC_STATUS_ERROR;
    return -1;
  }

  ctx->statedb.disabled = 0;

  return 0;
}

int csync_disable_statedb(CSYNC *ctx) {
  if (ctx == NULL) {
    return -1;
  }
  ctx->status_code = CSYNC_STATUS_OK;

  if (ctx->status & CSYNC_STATUS_INIT) {
    fprintf(stderr, "This function must be called before initialization.");
    ctx->status_code = CSYNC_STATUS_CSYNC_STATUS_ERROR;
    return -1;
  }

  ctx->statedb.disabled = 1;

  return 0;
}

int csync_is_statedb_disabled(CSYNC *ctx) {
  if (ctx == NULL) {
    return -1;
  }
  return ctx->statedb.disabled;
}

int csync_set_auth_callback(CSYNC *ctx, csync_auth_callback cb) {
  if (ctx == NULL || cb == NULL) {
    return -1;
  }

  if (ctx->status & CSYNC_STATUS_INIT) {
    ctx->status_code = CSYNC_STATUS_CSYNC_STATUS_ERROR;
    fprintf(stderr, "This function must be called before initialization.");
    return -1;
  }
  ctx->callbacks.auth_function = cb;

  return 0;
}

const char *csync_get_statedb_file(CSYNC *ctx) {
  if (ctx == NULL) {
    return NULL;
  }
  ctx->status_code = CSYNC_STATUS_OK;

  return c_strdup(ctx->statedb.file);
}

void *csync_get_userdata(CSYNC *ctx) {
  if (ctx == NULL) {
    return NULL;
  }
  return ctx->callbacks.userdata;
}

int csync_set_userdata(CSYNC *ctx, void *userdata) {
  if (ctx == NULL) {
    return -1;
  }

  ctx->callbacks.userdata = userdata;

  return 0;
}

csync_auth_callback csync_get_auth_callback(CSYNC *ctx) {
  if (ctx == NULL) {
    return NULL;
  }

  return ctx->callbacks.auth_function;
}

int csync_set_status(CSYNC *ctx, int status) {
  if (ctx == NULL || status < 0) {
    return -1;
  }

  ctx->status = status;

  return 0;
}

CSYNC_STATUS csync_get_status(CSYNC *ctx) {
  if (ctx == NULL) {
    return -1;
  }

  return ctx->status_code;
}

int csync_set_local_only(CSYNC *ctx, bool local_only) {
    if (ctx == NULL) {
        return -1;
    }

    ctx->status_code = CSYNC_STATUS_OK;

    if (ctx->status & CSYNC_STATUS_INIT) {
        fprintf(stderr, "csync_set_local_only: This function must be called before initialization.");
        ctx->status_code = CSYNC_STATUS_CSYNC_STATUS_ERROR;
        return -1;
    }

    ctx->options.local_only_mode=local_only;

    return 0;
}

bool csync_get_local_only(CSYNC *ctx) {
    if (ctx == NULL) {
        return -1;
    }
    ctx->status_code = CSYNC_STATUS_OK;

    return ctx->options.local_only_mode;
}

const char *csync_get_status_string(CSYNC *ctx)
{
  return csync_vio_get_status_string(ctx);
}

#ifdef WITH_ICONV
int csync_set_iconv_codec(const char *from)
{
  c_close_iconv();

  if (from != NULL) {
    c_setup_iconv(from);
  }

  return 0;
}
#endif

void csync_request_abort(CSYNC *ctx)
{
  if (ctx != NULL) {
    ctx->abort = true;
  }
}

void csync_resume(CSYNC *ctx)
{
  if (ctx != NULL) {
    ctx->abort = false;
  }
}

int  csync_abort_requested(CSYNC *ctx)
{
  if (ctx != NULL) {
    return ctx->abort;
  } else {
    return (1 == 0);
  }
}

void csync_file_stat_free(csync_file_stat_t *st)
{
  if (st) {
    SAFE_FREE(st->etag);
    SAFE_FREE(st->destpath);
    SAFE_FREE(st);
  }
}

int csync_set_module_property(CSYNC* ctx, const char* key, void* value)
{
    return csync_vio_set_property(ctx, key, value);
}


int csync_set_read_from_db(CSYNC* ctx, int enabled)
{
    ctx->read_from_db_disabled = !enabled;
    return 0;
}

