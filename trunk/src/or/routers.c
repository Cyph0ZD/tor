/* Copyright 2001-2003 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#define OR_PUBLICKEY_BEGIN_TAG "-----BEGIN RSA PUBLIC KEY-----\n"
#define OR_PUBLICKEY_END_TAG "-----END RSA PUBLIC KEY-----\n"
#define OR_SIGNATURE_BEGIN_TAG "-----BEGIN SIGNATURE-----\n"
#define OR_SIGNATURE_END_TAG "-----END SIGNATURE-----\n"

#define _GNU_SOURCE
/* XXX this is required on rh7 to make strptime not complain. how bad
 * is this for portability?
 */

#include "or.h"

/****************************************************************************/

static directory_t *directory = NULL; /* router array */
static routerinfo_t *desc_routerinfo = NULL; /* my descriptor */
static char descriptor[8192]; /* string representation of my descriptor */

extern or_options_t options; /* command-line and config-file options */

/****************************************************************************/

struct directory_token;
typedef struct directory_token directory_token_t;

/* static function prototypes */
void routerlist_free(routerinfo_t *list);
static int router_add_exit_policy_from_string(routerinfo_t *router, char *s);
static int router_add_exit_policy(routerinfo_t *router, 
                                  directory_token_t *tok);
static int router_resolve_directory(directory_t *dir);

/****************************************************************************/

void router_retry_connections(void) {
  int i;
  routerinfo_t *router;

  for (i=0;i<directory->n_routers;i++) {
    router = directory->routers[i];
    if(!connection_exact_get_by_addr_port(router->addr,router->or_port)) { /* not in the list */
      log_fn(LOG_DEBUG,"connecting to OR %s:%u.",router->address,router->or_port);
      connection_or_connect(router);
    }
  }
}

routerinfo_t *router_pick_directory_server(void) {
  /* pick the first running router with a positive dir_port */
  int i;
  routerinfo_t *router, *dirserver=NULL;
  
  if(!directory)
    return NULL;

  for(i=0;i<directory->n_routers;i++) {
    router = directory->routers[i];
    if(router->dir_port > 0 && router->is_running)
      return router;
  }

  log_fn(LOG_WARN,"No dirservers are up. Giving them all another chance.");
  /* no running dir servers found? go through and mark them all as up,
   * and we'll cycle through the list again. */
  for(i=0;i<directory->n_routers;i++) {
    router = directory->routers[i];
    if(router->dir_port > 0) {
      router->is_running = 1;
      dirserver = router;
    }
  }

  return dirserver;
}

void router_upload_desc_to_dirservers(void) {
  int i;
  routerinfo_t *router;

  if(!directory)
    return;

  if (!router_get_my_descriptor()) {
    log_fn(LOG_WARN, "No descriptor; skipping upload");
    return;
  }

  for(i=0;i<directory->n_routers;i++) {
    router = directory->routers[i];
    if(router->dir_port > 0)
      directory_initiate_command(router, DIR_CONN_STATE_CONNECTING_UPLOAD);
  }
}

routerinfo_t *router_get_by_addr_port(uint32_t addr, uint16_t port) {
  int i;
  routerinfo_t *router;

  assert(directory);

  for(i=0;i<directory->n_routers;i++) {
    router = directory->routers[i];
    if ((router->addr == addr) && (router->or_port == port))
      return router;
  }
  return NULL;
}

routerinfo_t *router_get_by_link_pk(crypto_pk_env_t *pk) 
{
  int i;
  routerinfo_t *router;

  assert(directory);

  for(i=0;i<directory->n_routers;i++) {
    router = directory->routers[i];
    if (0 == crypto_pk_cmp_keys(router->link_pkey, pk))
      return router;
  }
  return NULL;
}

routerinfo_t *router_get_by_nickname(char *nickname)
{
  int i;
  routerinfo_t *router;

  assert(directory);

  for(i=0;i<directory->n_routers;i++) {
    router = directory->routers[i];
    if (0 == strcmp(router->nickname, nickname))
      return router;
  }
  return NULL;
}

void router_get_directory(directory_t **pdirectory) {
  *pdirectory = directory;
}

/* delete a router from memory */
void routerinfo_free(routerinfo_t *router)
{
  struct exit_policy_t *e;
  
  if (!router)
    return;

  tor_free(router->address);
  tor_free(router->nickname);
  if (router->onion_pkey)
    crypto_free_pk_env(router->onion_pkey);
  if (router->link_pkey)
    crypto_free_pk_env(router->link_pkey);
  if (router->identity_pkey)
    crypto_free_pk_env(router->identity_pkey);
  while (router->exit_policy) {
    e = router->exit_policy;
    router->exit_policy = e->next;
    tor_free(e->string);
    free(e);
  }
  free(router);
}

void directory_free(directory_t *dir)
{
  int i;
  for (i = 0; i < dir->n_routers; ++i)
    routerinfo_free(dir->routers[i]);
  tor_free(dir->routers);
  tor_free(dir->software_versions);
  free(dir);
}

void router_mark_as_down(char *nickname) {
  routerinfo_t *router = router_get_by_nickname(nickname);
  if(!router) /* we don't seem to know about him in the first place */
    return;
  log_fn(LOG_DEBUG,"Marking %s as down.",router->nickname);
  router->is_running = 0;
}

/* load the router list */
int router_get_list_from_file(char *routerfile)
{
  char *string;

  string = read_file_to_str(routerfile);
  if(!string) {
    log_fn(LOG_WARN,"Failed to load routerfile %s.",routerfile);
    return -1;
  }
  
  if(router_get_list_from_string(string) < 0) {
    log_fn(LOG_WARN,"The routerfile itself was corrupt.");
    free(string);
    return -1;
  }

  free(string);
  return 0;
} 


typedef enum {
  K_ACCEPT,
  K_DIRECTORY_SIGNATURE,
  K_RECOMMENDED_SOFTWARE,
  K_REJECT, 
  K_ROUTER, 
  K_SIGNED_DIRECTORY,
  K_SIGNING_KEY,
  K_ONION_KEY,
  K_LINK_KEY,
  K_ROUTER_SIGNATURE,
  K_PUBLISHED,
  K_RUNNING_ROUTERS,
  K_PLATFORM,
  _SIGNATURE, 
  _PUBLIC_KEY, 
  _ERR, 
  _EOF 
} directory_keyword;

struct token_table_ent { char *t; int v; };

static struct token_table_ent token_table[] = {
  { "accept", K_ACCEPT },
  { "directory-signature", K_DIRECTORY_SIGNATURE },
  { "reject", K_REJECT },
  { "router", K_ROUTER },
  { "recommended-software", K_RECOMMENDED_SOFTWARE },
  { "signed-directory", K_SIGNED_DIRECTORY },
  { "signing-key", K_SIGNING_KEY },
  { "onion-key", K_ONION_KEY },
  { "link-key", K_LINK_KEY },
  { "router-signature", K_ROUTER_SIGNATURE },
  { "published", K_PUBLISHED },
  { "running-routers", K_RUNNING_ROUTERS },
  { "platform", K_PLATFORM },
  { NULL, -1 }
};

#define MAX_ARGS 1024
struct directory_token {
  directory_keyword tp;
  union {
    struct {
      char *args[MAX_ARGS+1]; 
      int n_args;
    } cmd;
    char *signature;
    char *error;
    crypto_pk_env_t *public_key;
  } val;
};

/* Free any malloced resources allocated for a token.  Don't call this if
   you inherit the reference to those resources.
 */
static void
router_release_token(directory_token_t *tok)
{
  switch (tok->tp) 
    {
    case _SIGNATURE:
      free(tok->val.signature);
      break;
    case _PUBLIC_KEY:
      crypto_free_pk_env(tok->val.public_key);
      break;
    default:
      break;
    }
}

static int
_router_get_next_token(char **s, directory_token_t *tok) {
  char *next;
  crypto_pk_env_t *pkey = NULL;
  char *signature = NULL;
  int i, done;

  tok->tp = _ERR;
  tok->val.error = "";

  *s = eat_whitespace(*s);
  if (!**s) {
    tok->tp = _EOF;
    return 0;
  } else if (**s == '-') {
    next = strchr(*s, '\n');
    if (! next) { tok->val.error = "No newline at EOF"; return -1; }
    ++next;
    if (! strncmp(*s, OR_PUBLICKEY_BEGIN_TAG, next-*s)) {
      next = strstr(*s, OR_PUBLICKEY_END_TAG);
      if (!next) { tok->val.error = "No public key end tag found"; return -1; }
      next = strchr(next, '\n'); /* Part of OR_PUBLICKEY_END_TAG; can't fail.*/
      ++next;
      if (!(pkey = crypto_new_pk_env(CRYPTO_PK_RSA))) 
        return -1;
      if (crypto_pk_read_public_key_from_string(pkey, *s, next-*s)) {
        crypto_free_pk_env(pkey);
        tok->val.error = "Couldn't parse public key.";
        return -1;
      }
      tok->tp = _PUBLIC_KEY;
      tok->val.public_key = pkey;
      *s = next;
      return 0;
    } else if (! strncmp(*s, OR_SIGNATURE_BEGIN_TAG, next-*s)) {
      /* Advance past newline; can't fail. */
      *s = strchr(*s, '\n'); 
      ++*s;
      /* Find end of base64'd data */
      next = strstr(*s, OR_SIGNATURE_END_TAG);
      if (!next) { tok->val.error = "No signature end tag found"; return -1; }
      
      signature = tor_malloc(256);
      i = base64_decode(signature, 256, *s, next-*s);
      if (i<0) {
        free(signature);
        tok->val.error = "Error decoding signature."; return -1;
      } else if (i != 128) {
        free(signature);
        tok->val.error = "Bad length on decoded signature."; return -1;
      }
      tok->tp = _SIGNATURE;
      tok->val.signature = signature;

      next = strchr(next, '\n'); /* Part of OR_SIGNATURE_END_TAG; can't fail.*/
      *s = next+1;
      return 0;
    } else {
      tok->val.error = "Unrecognized begin line"; return -1;
    }
  } else {
    next = find_whitespace(*s);
    if (!next) {
      tok->val.error = "Unexpected EOF"; return -1;
    }
    for (i = 0 ; token_table[i].t ; ++i) {
      if (!strncmp(token_table[i].t, *s, next-*s)) {
        tok->tp = token_table[i].v;
        i = 0;
        done = (*next == '\n');
        *s = eat_whitespace_no_nl(next);
        while (**s != '\n' && i <= MAX_ARGS && !done) {
          next = find_whitespace(*s);
          if (*next == '\n')
            done = 1;
          *next = 0;
          tok->val.cmd.args[i++] = *s;
          *s = eat_whitespace_no_nl(next+1);
        };
        tok->val.cmd.n_args = i;
        if (i > MAX_ARGS) {
          tok->tp = _ERR;
          tok->val.error = "Too many arguments"; return -1;
        }
        return 0;
      }
    }
    tok->val.error = "Unrecognized command"; return -1;
  }
}

#ifdef DEBUG_ROUTER_TOKENS
static void 
router_dump_token(directory_token_t *tok) {
  int i;
  switch(tok->tp) 
    {
    case _SIGNATURE:
      puts("(signature)");
      return;
    case _PUBLIC_KEY:
      puts("(public key)");
      return;
    case _ERR:
      printf("(Error: %s\n)", tok->val.error);
      return;
    case _EOF:
      puts("EOF");
      return;
    case K_ACCEPT: printf("Accept"); break;
    case K_DIRECTORY_SIGNATURE: printf("Directory-Signature"); break;
    case K_REJECT: printf("Reject"); break;
    case K_RECOMMENDED_SOFTWARE: printf("Server-Software"); break;
    case K_ROUTER: printf("Router"); break;
    case K_SIGNED_DIRECTORY: printf("Signed-Directory"); break;
    case K_SIGNING_KEY: printf("Signing-Key"); break;
    case K_ONION_KEY: printf("Onion-key"); break;
    case K_LINK_KEY: printf("Link-key"); break;
    case K_ROUTER_SIGNATURE: printf("Router-signature"); break;
    case K_PUBLISHED: printf("Published"); break;
    case K_RUNNING_ROUTERS: printf("Running-routers"); break;
    case K_PLATFORM: printf("Platform"); break;
    default:
      printf("?????? %d\n", tok->tp); return;
    }
  for (i = 0; i < tok->val.cmd.n_args; ++i) {
    printf(" \"%s\"", tok->val.cmd.args[i]);
  }
  printf("\n");
  return;
}
static int
router_get_next_token(char **s, directory_token_t *tok) {
  int i;
  i = _router_get_next_token(s, tok);
  router_dump_token(tok);
  return i;
}
#else
#define router_get_next_token _router_get_next_token
#endif

int router_get_list_from_string(char *s) 
{
  if (router_get_list_from_string_impl(&s, &directory, -1, NULL)) {
    log(LOG_WARN, "Error parsing router file");
    return -1;
  }
  if (router_resolve_directory(directory)) {
    log(LOG_WARN, "Error resolving directory");
    return -1;
  }
  return 0;
}

static int router_get_hash_impl(char *s, char *digest, const char *start_str,
                                const char *end_str) 
{
  char *start, *end;
  start = strstr(s, start_str);
  if (!start) {
    log_fn(LOG_WARN,"couldn't find \"%s\"",start_str);
    return -1;
  }
  end = strstr(start+strlen(start_str), end_str);
  if (!end) {
    log_fn(LOG_WARN,"couldn't find \"%s\"",end_str);
    return -1;
  }
  end = strchr(end, '\n');
  if (!end) {
    log_fn(LOG_WARN,"couldn't find EOL");
    return -1;
  }
  ++end;
  
  if (crypto_SHA_digest(start, end-start, digest)) {
    log_fn(LOG_WARN,"couldn't compute digest");
    return -1;
  }

  return 0;
}

int router_get_dir_hash(char *s, char *digest)
{
  return router_get_hash_impl(s,digest,
                              "signed-directory","directory-signature");
}
int router_get_router_hash(char *s, char *digest)
{
  return router_get_hash_impl(s,digest,
                              "router ","router-signature");
}

/* return 0 if myversion is in start. Else return -1. */
int compare_recommended_versions(char *myversion, char *start) {
  int len_myversion = strlen(myversion);
  char *comma;
  char *end = start + strlen(start);

  log_fn(LOG_DEBUG,"checking '%s' in '%s'.", myversion, start);
  
  for(;;) {
    comma = strchr(start, ',');
    if( ((comma ? comma : end) - start == len_myversion) &&
       !strncmp(start, myversion, len_myversion)) /* only do strncmp if the length matches */
        return 0; /* success, it's there */
    if(!comma)
      return -1; /* nope */
    start = comma+1;
  }
}

int router_get_dir_from_string(char *s, crypto_pk_env_t *pkey)
{
  if (router_get_dir_from_string_impl(s, &directory, pkey)) {
    log_fn(LOG_WARN, "Couldn't parse directory.");
    return -1;
  }
  if (router_resolve_directory(directory)) {
    log_fn(LOG_WARN, "Error resolving directory");
    return -1;
  }
  if (compare_recommended_versions(VERSION, directory->software_versions) < 0) {
    log(LOG_WARN, "You are running tor version %s, which is not recommended.\nPlease upgrade to one of %s.", VERSION, directory->software_versions);
    if(options.IgnoreVersion) {
      log(LOG_WARN, "IgnoreVersion is set. If it breaks, we told you so.");
    } else {
      log(LOG_ERR,"Set IgnoreVersion config variable if you want to proceed.");
      fflush(0);
      exit(0);
    }
  }

  return 0;
}

int router_get_dir_from_string_impl(char *s, directory_t **dest,
                                    crypto_pk_env_t *pkey)
{
  directory_token_t tok;
  char digest[20];
  char signed_digest[128];
  directory_t *new_dir = NULL;
  char *versions;
  struct tm published;
  time_t published_on;
  const char *good_nickname_lst[1024];
  int n_good_nicknames;
  
#define NEXT_TOK()                                                      \
  do {                                                                  \
    if (router_get_next_token(&s, &tok)) {                              \
      log_fn(LOG_WARN, "Error reading directory: %s", tok.val.error);\
      return -1;                                                        \
    } } while (0)
#define TOK_IS(type,name)                                               \
  do {                                                                  \
    if (tok.tp != type) {                                               \
      router_release_token(&tok);                                       \
      log_fn(LOG_WARN, "Error reading directory: expected %s", name);\
      return -1;                                                        \
    } } while(0)

  if (router_get_dir_hash(s, digest)) {
    log_fn(LOG_WARN, "Unable to compute digest of directory");
    goto err;
  }
  log(LOG_DEBUG,"Received directory hashes to %02x:%02x:%02x:%02x",
      ((int)digest[0])&0xff,((int)digest[1])&0xff,
      ((int)digest[2])&0xff,((int)digest[3])&0xff);

  NEXT_TOK();
  TOK_IS(K_SIGNED_DIRECTORY, "signed-directory");

  NEXT_TOK();
  TOK_IS(K_PUBLISHED, "published");
  if (tok.val.cmd.n_args != 2) {
    log_fn(LOG_WARN, "Invalid published line");
    goto err;
  }
  tok.val.cmd.args[1][-1] = ' ';
  if (!strptime(tok.val.cmd.args[0], "%Y-%m-%d %H:%M:%S", &published)) {
    log_fn(LOG_WARN, "Published time was unparseable"); goto err;
  }
  published_on = tor_timegm(&published);  

  NEXT_TOK();
  TOK_IS(K_RECOMMENDED_SOFTWARE, "recommended-software");
  if (tok.val.cmd.n_args != 1) {
    log_fn(LOG_WARN, "Invalid recommended-software line");
    goto err;
  }
  versions = tor_strdup(tok.val.cmd.args[0]);
  
  NEXT_TOK();
  TOK_IS(K_RUNNING_ROUTERS, "running-routers");
  n_good_nicknames = tok.val.cmd.n_args;
  memcpy(good_nickname_lst, tok.val.cmd.args, n_good_nicknames*sizeof(char *));

  if (router_get_list_from_string_impl(&s, &new_dir,
                                       n_good_nicknames, good_nickname_lst)) {
    log_fn(LOG_WARN, "Error reading routers from directory");
    goto err;
  }
  new_dir->software_versions = versions;
  new_dir->published_on = published_on;

  NEXT_TOK();
  TOK_IS(K_DIRECTORY_SIGNATURE, "directory-signature");
  NEXT_TOK();
  TOK_IS(_SIGNATURE, "signature");
  if (pkey) {
    if (crypto_pk_public_checksig(pkey, tok.val.signature, 128, signed_digest)
        != 20) {
      log_fn(LOG_WARN, "Error reading directory: invalid signature.");
      free(tok.val.signature);
      goto err;
    }
    log(LOG_DEBUG,"Signed directory hash starts %02x:%02x:%02x:%02x",
        ((int)signed_digest[0])&0xff,((int)signed_digest[1])&0xff,
        ((int)signed_digest[2])&0xff,((int)signed_digest[3])&0xff);
    if (memcmp(digest, signed_digest, 20)) {
      log_fn(LOG_WARN, "Error reading directory: signature does not match.");
      free(tok.val.signature);
      goto err;
    }
  }
  free(tok.val.signature);

  NEXT_TOK();
  TOK_IS(_EOF, "end of directory");

  if (*dest) 
    directory_free(*dest);
  *dest = new_dir;

  return 0;

 err:
  if (new_dir)
    directory_free(new_dir);
  return -1;
#undef NEXT_TOK
#undef TOK_IS
}

int router_get_list_from_string_impl(char **s, directory_t **dest, 
                                     int n_good_nicknames, 
                                     const char **good_nickname_lst)
{
  routerinfo_t *router;
  routerinfo_t **rarray;
  int rarray_len = 0;
  int i;

  assert(s && *s);

  rarray = (routerinfo_t **)tor_malloc((sizeof(routerinfo_t *))*MAX_ROUTERS_IN_DIR);

  while (1) {
    *s = eat_whitespace(*s);
    if (strncmp(*s, "router ", 7)!=0)
      break;
    router = router_get_entry_from_string(s);
    if (!router) {
      log_fn(LOG_WARN, "Error reading router");
      for(i=0;i<rarray_len;i++)
        routerinfo_free(rarray[i]);
      free(rarray);
      return -1;
    }
    if (rarray_len >= MAX_ROUTERS_IN_DIR) {
      log_fn(LOG_WARN, "too many routers");
      routerinfo_free(router);
      continue;
    } 
    if (n_good_nicknames>=0) {
      router->is_running = 0;
      for (i = 0; i < n_good_nicknames; ++i) {
        if (0==strcasecmp(good_nickname_lst[i], router->nickname)) {
          router->is_running = 1;
          break;
        }
      }
    } else {
      router->is_running = 1; /* start out assuming all dirservers are up */
    }
    rarray[rarray_len++] = router;
    log_fn(LOG_DEBUG,"just added router #%d.",rarray_len);
  }
 
  if (*dest) 
    directory_free(*dest);
  *dest = (directory_t *)tor_malloc(sizeof(directory_t));
  (*dest)->routers = rarray;
  (*dest)->n_routers = rarray_len;
  (*dest)->software_versions = NULL;
  return 0;
}

static int 
router_resolve(routerinfo_t *router)
{
  struct hostent *rent;

  rent = (struct hostent *)gethostbyname(router->address);
  if (!rent) {
    log_fn(LOG_WARN,"Could not get address for router %s.",router->address);
    return -1; 
  }
  assert(rent->h_length == 4);
  memcpy(&router->addr, rent->h_addr,rent->h_length);
  router->addr = ntohl(router->addr); /* get it back into host order */

  return 0;
}

static int 
router_resolve_directory(directory_t *dir)
{
  int i, max, remove;
  if (!dir)
    dir = directory;

  max = dir->n_routers;
  for (i = 0; i < max; ++i) {
    remove = 0;
    if (router_resolve(dir->routers[i])) {
      log_fn(LOG_WARN, "Couldn't resolve router %s; removing",
             dir->routers[i]->address);
      remove = 1;
    } else if (options.Nickname &&
               !strcmp(dir->routers[i]->nickname, options.Nickname)) {
      remove = 1;
    }
    if (remove) {
      routerinfo_free(dir->routers[i]);
      dir->routers[i] = dir->routers[--max];
      --dir->n_routers;
      --i;
    }
  }
  
  return 0;
}

/* reads a single router entry from s.
 * updates s so it points to after the router it just read.
 * mallocs a new router, returns it if all goes well, else returns NULL.
 */
routerinfo_t *router_get_entry_from_string(char**s) {
  routerinfo_t *router = NULL;
  char signed_digest[128];
  char digest[128];
  directory_token_t _tok;
  directory_token_t *tok = &_tok;
  struct tm published;
  int t;

#define NEXT_TOKEN()                                                     \
  do { if (router_get_next_token(s, tok)) {                              \
      log_fn(LOG_WARN, "Error reading directory: %s", tok->val.error);\
      goto err;                                                          \
    } } while(0)

#define ARGS tok->val.cmd.args

  if (router_get_router_hash(*s, digest) < 0) {
    log_fn(LOG_WARN, "Couldn't compute router hash.");
    return NULL;
  }

  NEXT_TOKEN();

  if (tok->tp != K_ROUTER) {
    router_release_token(tok);
    log_fn(LOG_WARN,"Entry does not start with \"router\"");
    return NULL;
  }

  router = tor_malloc(sizeof(routerinfo_t));
  memset(router,0,sizeof(routerinfo_t)); /* zero it out first */
  router->onion_pkey = router->identity_pkey = router->link_pkey = NULL; 

  if (tok->val.cmd.n_args != 6) {
    log_fn(LOG_WARN,"Wrong # of arguments to \"router\"");
    goto err;
  }
  router->nickname = tor_strdup(ARGS[0]);
  if (strlen(router->nickname) > MAX_NICKNAME_LEN) {
    log_fn(LOG_WARN,"Router nickname too long.");
    goto err;
  }
  if (strspn(router->nickname, LEGAL_NICKNAME_CHARACTERS) != 
      strlen(router->nickname)) {
    log_fn(LOG_WARN, "Router nickname contains illegal characters.");
    goto err;
  }
  
  /* read router.address */
  router->address = tor_strdup(ARGS[1]);
  router->addr = 0;

  /* Read router->or_port */
  router->or_port = atoi(ARGS[2]);
  if(!router->or_port) {
    log_fn(LOG_WARN,"or_port unreadable or 0. Failing.");
    goto err;
  }
  
  /* Router->socks_port */
  router->socks_port = atoi(ARGS[3]);
  
  /* Router->dir_port */
  router->dir_port = atoi(ARGS[4]);

  /* Router->bandwidth */
  router->bandwidth = atoi(ARGS[5]);
  if (!router->bandwidth) {
    log_fn(LOG_WARN,"bandwidth unreadable or 0. Failing.");
    goto err;
  }
  
  log_fn(LOG_DEBUG,"or_port %d, socks_port %d, dir_port %d, bandwidth %d.",
    router->or_port, router->socks_port, router->dir_port, router->bandwidth);

  /* XXX Later, require platform before published. */
  NEXT_TOKEN();
  if (tok->tp == K_PLATFORM) {
    NEXT_TOKEN();
  }
  
  if (tok->tp != K_PUBLISHED) {
    log_fn(LOG_WARN, "Missing published time"); goto err;
  }
  if (tok->val.cmd.n_args != 2) {
    log_fn(LOG_WARN, "Wrong number of arguments to published"); goto err;
  }
  ARGS[1][-1] = ' '; /* Re-insert space. */
  if (!strptime(ARGS[0], "%Y-%m-%d %H:%M:%S", &published)) {
    log_fn(LOG_WARN, "Published time was unparseable"); goto err;
  }
  router->published_on = tor_timegm(&published);

  NEXT_TOKEN();
  if (tok->tp != K_ONION_KEY) {
    log_fn(LOG_WARN, "Missing onion-key"); goto err;
  }
  NEXT_TOKEN();
  if (tok->tp != _PUBLIC_KEY) {
    log_fn(LOG_WARN, "Missing onion key"); goto err;
  } /* XXX Check key length */
  router->onion_pkey = tok->val.public_key;

  NEXT_TOKEN();
  if (tok->tp != K_LINK_KEY) {
    log_fn(LOG_WARN, "Missing link-key");  goto err;
  }
  NEXT_TOKEN();
  if (tok->tp != _PUBLIC_KEY) {
    log_fn(LOG_WARN, "Missing link key"); goto err;
  } /* XXX Check key length */
  router->link_pkey = tok->val.public_key;

  NEXT_TOKEN();
  if (tok->tp != K_SIGNING_KEY) {
    log_fn(LOG_WARN, "Missing signing-key"); goto err;
  }
  NEXT_TOKEN();
  if (tok->tp != _PUBLIC_KEY) {
    log_fn(LOG_WARN, "Missing signing key"); goto err;
  }
  router->identity_pkey = tok->val.public_key;

  NEXT_TOKEN();
  while (tok->tp == K_ACCEPT || tok->tp == K_REJECT) {
    router_add_exit_policy(router, tok);
    NEXT_TOKEN();
  }
  
  if (tok->tp != K_ROUTER_SIGNATURE) {
    log_fn(LOG_WARN,"Missing router signature");
    goto err;
  }
  NEXT_TOKEN();
  if (tok->tp != _SIGNATURE) {
    log_fn(LOG_WARN,"Missing router signature");
    goto err;
  }
  assert (router->identity_pkey);

  if ((t=crypto_pk_public_checksig(router->identity_pkey, tok->val.signature,
                                   128, signed_digest)) != 20) {
    log_fn(LOG_WARN, "Invalid signature %d",t);
    goto err;
  }
  if (memcmp(digest, signed_digest, 20)) {
    log_fn(LOG_WARN, "Mismatched signature");
    goto err;
  }
  
  router_release_token(tok); /* free the signature */
  return router;

 err:
  router_release_token(tok); 
  routerinfo_free(router);
  return NULL;
#undef ARGS
#undef NEXT_TOKEN
}

void router_add_exit_policy_from_config(routerinfo_t *router) {
  char *s = options.ExitPolicy, *e;
  int last=0;
  char line[1024];

  if(!s) {
    log_fn(LOG_INFO,"No exit policy configured. Ok.");
    return; /* nothing to see here */
  }
  if(!*s) {
    log_fn(LOG_INFO,"Exit policy is empty. Ok.");
    return; /* nothing to see here */
  }

  for(;;) {
    e = strchr(s,',');
    if(!e) {
      last = 1;
      strncpy(line,s,1023); 
    } else {
      memcpy(line,s, ((e-s)<1023)?(e-s):1023); 
      line[e-s] = 0;
    }
    line[1023]=0;
    log_fn(LOG_DEBUG,"Adding new entry '%s'",line);
    if(router_add_exit_policy_from_string(router,line) < 0)
      log_fn(LOG_WARN,"Malformed exit policy %s; skipping.", line);
    if(last)
      return;
    s = e+1;
  }
}

static int
router_add_exit_policy_from_string(routerinfo_t *router,
                                   char *s)
{
  directory_token_t tok;
  char *tmp, *cp;
  int r;
  int len, idx;

  len = strlen(s);
  tmp = cp = tor_malloc(len+2);
  for (idx = 0; idx < len; ++idx) {
    tmp[idx] = tolower(s[idx]);
  }
  tmp[len]='\n';
  tmp[len+1]='\0';
  if (router_get_next_token(&cp, &tok)) {
    log_fn(LOG_WARN, "Error reading exit policy: %s", tok.val.error);
    free(tmp);
    return -1;                                                        
  }
  if (tok.tp != K_ACCEPT && tok.tp != K_REJECT) {
    log_fn(LOG_WARN, "Expected 'accept' or 'reject'.");
    free(tmp);
    return -1;
  }
  r = router_add_exit_policy(router, &tok);
  free(tmp);
  return r;
}

static int router_add_exit_policy(routerinfo_t *router, 
                                  directory_token_t *tok) {
  struct exit_policy_t *tmpe, *newe;
  struct in_addr in;
  char *arg, *address, *mask, *port, *endptr;
  int bits;

  if (tok->val.cmd.n_args != 1)
    return -1;
  arg = tok->val.cmd.args[0];

  newe = tor_malloc(sizeof(struct exit_policy_t));
  memset(newe,0,sizeof(struct exit_policy_t));
  
  newe->string = tor_malloc(8+strlen(arg));
  if (tok->tp == K_REJECT) {
    strcpy(newe->string, "reject ");
    newe->policy_type = EXIT_POLICY_REJECT;
  } else {
    assert(tok->tp == K_ACCEPT);
    strcpy(newe->string, "accept ");
    newe->policy_type = EXIT_POLICY_ACCEPT;
  }
  strcat(newe->string, arg);

  address = arg;
  mask = strchr(arg,'/');
  port = strchr(mask?mask:arg,':');
  if(!port)
    goto policy_read_failed;
  if (mask)
    *mask++ = 0;
  *port++ = 0;

  if (strcmp(address, "*") == 0) {
    newe->addr = 0;
  } else if (inet_aton(address, &in) != 0) {
    newe->addr = in.s_addr;
  } else {
    log_fn(LOG_WARN, "Malformed IP %s in exit policy; rejecting.",
           address);
    goto policy_read_failed;
  }
  if (!mask) {
    if (strcmp(address, "*") == 0)
      newe->msk = 0;
    else
      newe->msk = 0xFFFFFFFFu;
  } else {
    endptr = NULL;
    bits = (int) strtol(mask, &endptr, 10);
    if (!*endptr) {
      /* strtol handled the whole mask. */
      newe->msk = ~((1<<(32-bits))-1);
    } else if (inet_aton(mask, &in) != 0) {
      newe->msk = in.s_addr;
    } else {
      log_fn(LOG_WARN, "Malformed mask %s on exit policy; rejecting.",
             mask);
      goto policy_read_failed;
    }
  }
  if (strcmp(port, "*") == 0) {
    newe->prt = 0;
  } else { 
    endptr = NULL;
    newe->prt = strtol(port, &endptr, 10);
    if (*endptr) {
      log_fn(LOG_WARN, "Malformed port %s on exit policy; rejecting.",
             port);
      goto policy_read_failed;
    }
  }

  in.s_addr = newe->addr;
  address = tor_strdup(inet_ntoa(in));
  in.s_addr = newe->msk;
  log_fn(LOG_DEBUG,"%s %s/%s:%d",
         newe->policy_type == EXIT_POLICY_REJECT ? "reject" : "accept",
         address, inet_ntoa(in), newe->prt);
  tor_free(address);

  /* now link newe onto the end of exit_policy */

  if(!router->exit_policy) {
    router->exit_policy = newe;
    return 0;
  }

  for(tmpe=router->exit_policy; tmpe->next; tmpe=tmpe->next) ;
  tmpe->next = newe;

  return 0;

policy_read_failed:
  assert(newe->string);
  log_fn(LOG_WARN,"Couldn't parse line '%s'. Dropping", newe->string);
  tor_free(newe->string);
  free(newe);
  return -1;
}

/* Addr is 0 for "IP unknown".
 *
 * Returns -1 for 'rejected', 0 for accepted, 1 for 'maybe' (since IP is
 * unknown.
 */
int router_supports_exit_address(uint32_t addr, uint16_t port,
                                 routerinfo_t *router)
{
  return router_compare_addr_to_exit_policy(addr, port, router->exit_policy);
}

/* Addr is 0 for "IP unknown".
 *
 * Returns -1 for 'rejected', 0 for accepted, 1 for 'maybe' (since IP is
 * unknown.
 */
int router_compare_addr_to_exit_policy(uint32_t addr, uint16_t port,
                                       struct exit_policy_t *policy)
{
  int maybe_reject = 0;
  int match = 0;
  struct in_addr in;
  struct exit_policy_t *tmpe;

  for(tmpe=policy; tmpe; tmpe=tmpe->next) {
    log_fn(LOG_DEBUG,"Considering exit policy %s", tmpe->string);
    if (!addr) {
      /* Address is unknown. */
      if (tmpe->msk == 0 && port == tmpe->prt) {
        /* The exit policy is accept/reject *:port */
        match = 1;
      } else if ((!tmpe->prt || port == tmpe->prt) && 
                 tmpe->policy_type == EXIT_POLICY_REJECT) {
        /* The exit policy is reject ???:port */
        maybe_reject = 1;
      }
    } else {
      /* Address is known */
      if ( (addr & tmpe->msk) == (tmpe->addr & tmpe->msk) &&
           (!tmpe->prt || port == tmpe->prt) ) {
        /* Exact match for the policy */
        match = 1;
      }
    }
    if (match) {
      in.s_addr = addr;
      log_fn(LOG_INFO,"Address %s:%d matches exit policy '%s'",
             inet_ntoa(in), port, tmpe->string);
      if(tmpe->policy_type == EXIT_POLICY_ACCEPT)
        return 0;
      else
        return -1;
    }
  }
  if (maybe_reject)
    return 1;
  else
    return 0; /* accept all by default. */
}

/* Return 0 if my exit policy says to allow connection to conn.
 * Else return -1.
 */
int router_compare_to_exit_policy(connection_t *conn) {
  assert(desc_routerinfo);
  
  if (router_compare_addr_to_exit_policy(conn->addr, conn->port,
                                         desc_routerinfo->exit_policy))
    return -1;
  else 
    return 0;
}

const char *router_get_my_descriptor(void) {
  if (!desc_routerinfo) {
    if (router_rebuild_descriptor())
      return NULL;
  }
  log_fn(LOG_DEBUG,"my desc is '%s'",descriptor);
  return descriptor;
}
const routerinfo_t *router_get_desc_routerinfo(void) {
  if (!desc_routerinfo) {
    if (router_rebuild_descriptor()) 
      return NULL;
  }
  return desc_routerinfo;
}

int router_rebuild_descriptor(void) {
  routerinfo_t *ri;
  char localhostname[256];
  char *address = options.Address;

  if(!address) { /* if not specified in config, we find a default */
    if(gethostname(localhostname,sizeof(localhostname)) < 0) {
      log_fn(LOG_WARN,"Error obtaining local hostname");
      return -1;
    }
    address = localhostname;
    if(!strchr(address,'.')) {
      log_fn(LOG_WARN,"fqdn '%s' has only one element. Misconfigured machine?",address);
      log_fn(LOG_WARN,"Try setting the Address line in your config file.");
      return -1;
    }
  }
  ri = tor_malloc(sizeof(routerinfo_t));
  ri->address = tor_strdup(address);
  ri->nickname = tor_strdup(options.Nickname);
  /* No need to set addr. */
  ri->or_port = options.ORPort;
  ri->socks_port = options.SocksPort;
  ri->dir_port = options.DirPort;
  ri->published_on = time(NULL);
  ri->onion_pkey = crypto_pk_dup_key(get_onion_key());
  ri->link_pkey = crypto_pk_dup_key(get_link_key());
  ri->identity_pkey = crypto_pk_dup_key(get_identity_key());
  ri->bandwidth = options.TotalBandwidth;
  ri->exit_policy = NULL; /* zero it out first */
  router_add_exit_policy_from_config(ri);
  if (desc_routerinfo)
    routerinfo_free(desc_routerinfo);
  desc_routerinfo = ri;
  if (router_dump_router_to_string(descriptor, 8192, ri, get_identity_key())<0) {
    log_fn(LOG_WARN, "Couldn't dump router to string.");
    return -1;
  }
  return 0;
}

static void get_platform_str(char *platform, int len)
{
  snprintf(platform, len-1, "Tor %s on %s", VERSION, get_uname());
  platform[len-1] = '\0';
  return;
}

#define DEBUG_ROUTER_DUMP_ROUTER_TO_STRING
int router_dump_router_to_string(char *s, int maxlen, routerinfo_t *router,
                                 crypto_pk_env_t *ident_key) {
  char *onion_pkey;
  char *link_pkey;
  char *identity_pkey;
  struct in_addr in;
  char platform[256];
  char digest[20];
  char signature[128];
  char published[32];
  int onion_pkeylen, link_pkeylen, identity_pkeylen;
  int written;
  int result=0;
  struct exit_policy_t *tmpe;
#ifdef DEBUG_ROUTER_DUMP_ROUTER_TO_STRING
  char *s_tmp, *s_dup;
  routerinfo_t *ri_tmp;
#endif
  
  get_platform_str(platform, sizeof(platform));

  if (crypto_pk_cmp_keys(ident_key, router->identity_pkey)) {
    log_fn(LOG_WARN,"Tried to sign a router with a private key that didn't match router's public key!");
    return -1;
  }

  if(crypto_pk_write_public_key_to_string(router->onion_pkey,
                                          &onion_pkey,&onion_pkeylen)<0) {
    log_fn(LOG_WARN,"write onion_pkey to string failed!");
    return -1;
  }

  if(crypto_pk_write_public_key_to_string(router->identity_pkey,
                                          &identity_pkey,&identity_pkeylen)<0) {
    log_fn(LOG_WARN,"write identity_pkey to string failed!");
    return -1;
  }

  if(crypto_pk_write_public_key_to_string(router->link_pkey,
                                          &link_pkey,&link_pkeylen)<0) {
    log_fn(LOG_WARN,"write link_pkey to string failed!");
    return -1;
  }
  strftime(published, 32, "%Y-%m-%d %H:%M:%S", gmtime(&router->published_on));
  
  result = snprintf(s, maxlen, 
                    "router %s %s %d %d %d %d\n"
                    "platform %s\n"
                    "published %s\n"
                    "onion-key\n%s"
                    "link-key\n%s"
                    "signing-key\n%s",
    router->nickname,             
    router->address,
    router->or_port,
    router->socks_port,
    router->dir_port,
    router->bandwidth,
    platform,
    published,
    onion_pkey, link_pkey, identity_pkey);

  free(onion_pkey);
  free(link_pkey);
  free(identity_pkey);

  if(result < 0 || result >= maxlen) {
    /* apparently different glibcs do different things on snprintf error.. so check both */
    return -1;
  }
  written = result;

  for(tmpe=router->exit_policy; tmpe; tmpe=tmpe->next) {
    in.s_addr = tmpe->addr;
    result = snprintf(s+written, maxlen-written, "%s %s",
        tmpe->policy_type == EXIT_POLICY_ACCEPT ? "accept" : "reject",
        tmpe->msk == 0 ? "*" : inet_ntoa(in));
    if(result < 0 || result+written > maxlen) {
      /* apparently different glibcs do different things on snprintf error.. so check both */
      return -1;
    }
    written += result;
    if (tmpe->msk != 0xFFFFFFFFu) {
      in.s_addr = tmpe->msk;
      result = snprintf(s+written, maxlen-written, "/%s", inet_ntoa(in));
      if (result<0 || result+written > maxlen)
        return -1;
      written += result;
    }
    if (tmpe->prt) {
      result = snprintf(s+written, maxlen-written, ":%d", tmpe->prt);
      if (result<0 || result+written > maxlen)
        return -1;
      written += result;
    } else {
      if (written > maxlen-3)
        return -1;
      strcat(s+written, ":*");
    }
  }
  if (written > maxlen-256) /* Not enough room for signature. */
    return -1;

  strcat(s+written, "router-signature\n");
  written += strlen(s+written);
  s[written] = '\0';
  if (router_get_router_hash(s, digest) < 0)
    return -1;

  if (crypto_pk_private_sign(ident_key, digest, 20, signature) < 0) {
    log_fn(LOG_WARN, "Error signing digest");
    return -1;
  }
  strcat(s+written, "-----BEGIN SIGNATURE-----\n");
  written += strlen(s+written);
  if (base64_encode(s+written, maxlen-written, signature, 128) < 0) {
    log_fn(LOG_WARN, "Couldn't base64-encode signature");
    return -1;
  }
  written += strlen(s+written);
  strcat(s+written, "-----END SIGNATURE-----\n");
  written += strlen(s+written);
  
  if (written > maxlen-2) 
    return -1;
  /* include a last '\n' */
  s[written] = '\n';
  s[written+1] = 0;

#ifdef DEBUG_ROUTER_DUMP_ROUTER_TO_STRING
  s_tmp = s_dup = tor_strdup(s);
  ri_tmp = router_get_entry_from_string(&s_tmp);
  if (!ri_tmp) {
    log_fn(LOG_ERR, "We just generated a router descriptor we can't parse: <<%s>>", 
           s);
    return -1;
  }
  free(s_dup);
  routerinfo_free(ri_tmp);
#endif

  return written+1;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
