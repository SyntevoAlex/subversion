/*
 * config.c :  reading configuration information
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#define APR_WANT_STRFUNC
#define APR_WANT_MEMFUNC
#include <apr_want.h>

#include <apr_lib.h>
#include <apr_user.h>
#include "svn_error.h"
#include "config_impl.h"




/* Section table entries. */
typedef struct cfg_section_t cfg_section_t;
struct cfg_section_t
{
  /* The section name. */
  const char *name;

  /* The section name, converted into a hash key. */
  const char *hash_key;

  /* Table of cfg_option_t's. */
  apr_hash_t *options;
};


/* Option table entries. */
typedef struct cfg_option_t cfg_option_t;
struct cfg_option_t
{
  /* The option name. */
  const char *name;

  /* The option name, converted into a hash key. */
  const char *hash_key;

  /* The unexpanded option value. */
  svn_stringbuf_t *value;

  /* The expanded option value. */
  svn_stringbuf_t *x_value;

  /* Expansion flag. If this is TRUE, this value has already been expanded.
     In this case, if x_value is NULL, no expansions were necessary,
     and value should be used directly. */
  svn_boolean_t expanded;
};



svn_error_t *
svn_config_read (svn_config_t **cfgp, const char *file,
                 svn_boolean_t must_exist, apr_pool_t *pool)
{
  svn_config_t *cfg = apr_palloc (pool, sizeof (*cfg));
  svn_error_t *err;

  cfg->sections = apr_hash_make (pool);
  cfg->pool = pool;
  cfg->x_pool = svn_pool_create (pool);
  cfg->x_values = FALSE;
  cfg->tmp_key = svn_stringbuf_create ("", pool);

  /* Yes, this is platform-specific code in Subversion, but there's no
     practical way to migrate it into APR, as it's simultaneously
     Subversion-specific and Windows-specific.  Even if we eventually
     want to have APR offer a generic config-reading interface, it
     makes sense to test it here first and migrate it later. */
#ifdef SVN_WIN32
  if (0 == strncmp (file, SVN_REGISTRY_PREFIX, SVN_REGISTRY_PREFIX_LEN))
    err = svn_config__parse_registry (cfg, file + SVN_REGISTRY_PREFIX_LEN,
                                      must_exist);
  else
#endif /* SVN_WIN32 */
    err = svn_config__parse_file (cfg, file, must_exist);

  if (err != SVN_NO_ERROR)
    return err;
  else
    *cfgp = cfg;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_config_read_all (svn_config_t **cfgp, apr_pool_t *pool)
{
  /* Read things in this order, with later reads overriding the
   * results of earlier ones:
   *
   *    1. Windows registry system config or global config file,
   *       whichever applies (can't have both).
   *
   *    2. Windows registry user config, if any.
   *
   *    3. User's config file, if any.
   */

#ifdef SVN_WIN32
  svn_config_read (cfgp, SVN_REGISTRY_SYS_CONFIG_PATH, FALSE, pool);
  svn_config_merge (*cfgp, SVN_REGISTRY_USR_CONFIG_PATH, FALSE);
#else  /* ! SVN_WIN32 */
  svn_config_read (cfgp, SVN_CONFIG__SYS_FILE, FALSE, pool);
#endif  /* SVN_WIN32 */

  /* Check for user config file in both Windows and non-Windows. */
  {
    apr_status_t apr_err;
    
    /* ### Are there any platforms where APR_HAS_USER is not defined?
       This code won't compile without it.  */
    
    char *usr_cfg_path;
    apr_uid_t uid;
    apr_gid_t gid;
    char *username;
    char *homedir;
   
    /* ### Will these calls fail under Windows sometimes?  If so,
       we shouldn't error, we should just fall fack to registry. */
   
    apr_err = apr_current_userid (&uid, &gid, pool);
    if (apr_err)
      return svn_error_create
        (apr_err, 0, NULL, pool,
         "svn_config_read_all: unable to get current userid.");
   
    apr_err = apr_get_username (&username, uid, pool);
    if (apr_err)
      return svn_error_create
        (apr_err, 0, NULL, pool,
         "svn_config_read_all: unable to get username.");
   
    apr_err = apr_get_home_directory (&homedir, username, pool);
    if (apr_err)
      return svn_error_createf
        (apr_err, 0, NULL, pool,
         "svn_config_read_all: unable to get home dir for user %s.", username);

    /* ### No compelling reason to use svn's path lib here. */
    usr_cfg_path = apr_psprintf
      (pool, "%s/%s/%s", homedir, SVN_CONFIG__DIRECTORY, SVN_CONFIG__FILE);

    SVN_ERR (svn_config_merge (*cfgp, usr_cfg_path, FALSE));
  }

  return SVN_NO_ERROR;
}



/* Iterate through CFG, passing BATON to CALLBACK for every (SECTION, OPTION)
   pair.  Stop if CALLBACK returns TRUE.  Allocate from POOL. */
static void
for_each_option (svn_config_t *cfg, void *baton, apr_pool_t *pool,
                 svn_boolean_t callback (void *same_baton,
                                         cfg_section_t *section,
                                         cfg_option_t *option))
{
  apr_hash_index_t *sec_ndx;
  for (sec_ndx = apr_hash_first (pool, cfg->sections);
       sec_ndx != NULL;
       sec_ndx = apr_hash_next (sec_ndx))
    {
      void *sec_ptr;
      cfg_section_t *sec;
      apr_hash_index_t *opt_ndx;

      apr_hash_this (sec_ndx, NULL, NULL, &sec_ptr);
      sec = sec_ptr;

      for (opt_ndx = apr_hash_first (pool, sec->options);
           opt_ndx != NULL;
           opt_ndx = apr_hash_next (opt_ndx))
        {
          void *opt_ptr;
          cfg_option_t *opt;

          apr_hash_this (opt_ndx, NULL, NULL, &opt_ptr);
          opt = opt_ptr;

          if (callback (baton, sec, opt))
            return;
        }
    }
}



static svn_boolean_t
merge_callback (void *baton, cfg_section_t *section, cfg_option_t *option)
{
  svn_config_set (baton, section->name, option->name, option->value->data);
  return FALSE;
}

svn_error_t *
svn_config_merge (svn_config_t *cfg, const char *file,
                  svn_boolean_t must_exist)
{
  /* The original config hash shouldn't change if there's an error
     while reading the confguration, so read into a temporary table.
     ### We could use a tmp subpool for this, since merge_cfg is going
     to be tossed afterwards.  Premature optimization, though? */
  svn_config_t *merge_cfg;
  SVN_ERR (svn_config_read (&merge_cfg, file, must_exist, cfg->pool));

  /* Now copy the new options into the original table. */
  for_each_option (merge_cfg, cfg, merge_cfg->pool, merge_callback);
  return SVN_NO_ERROR;
}



/* Remove variable expansions from CFG.  Walk through the options tree,
   killing all expanded values, then clear the expanded value pool. */
static svn_boolean_t
rmex_callback (void *baton, cfg_section_t *section, cfg_option_t *option)
{
  /* Only clear the `expanded' flag if the value actually contains
     variable expansions. */
  if (option->expanded && option->x_value != NULL)
    {
      option->x_value = NULL;
      option->expanded = FALSE;
    }

  (void)(baton);                /* Unused parameter. */
  (void)(section);              /* Unused parameter. */
  return FALSE;
}

static void
remove_expansions (svn_config_t *cfg)
{
  if (!cfg->x_values)
    return;

  for_each_option (cfg, NULL, cfg->x_pool, rmex_callback);
  apr_pool_clear (cfg->x_pool);
  cfg->x_values = FALSE;
}



/* Canonicalize a string for hashing.  Modifies KEY in place. */
static APR_INLINE char *
make_hash_key (char *key)
{
  register char *p;
  for (p = key; *p != 0; ++p)
    *p = apr_tolower (*p);
  return key;
}


/* Return a pointer to an option in CFG, or NULL if it doesn't exist.
   if SECTIONP is non-null, return a pointer to the option's section.
   OPTION may be NULL. */
static cfg_option_t *
find_option (svn_config_t *cfg, const char *section, const char *option,
             cfg_section_t **sectionp)
{
  void *sec_ptr;

  /* Canonicalize the hash key */
  svn_stringbuf_set (cfg->tmp_key, section);
  make_hash_key (cfg->tmp_key->data);

  sec_ptr = apr_hash_get (cfg->sections, cfg->tmp_key->data,
                          cfg->tmp_key->len);
  if (sectionp != NULL)
    *sectionp = sec_ptr;

  if (sec_ptr != NULL && option != NULL)
    {
      cfg_section_t *sec = sec_ptr;

      /* Canonicalize the option key */
      svn_stringbuf_set (cfg->tmp_key, option);
      make_hash_key (cfg->tmp_key->data);

      return apr_hash_get (sec->options, cfg->tmp_key->data,
                           cfg->tmp_key->len);
    }

  return NULL;
}


/* Set VALUEP according to the OPT's value. */
static void
make_string_from_option (svn_string_t *valuep,
                         svn_config_t *cfg, cfg_option_t *opt)
{
  /* TODO: Expand the option's value */
  (void)(cfg);

  if (opt->x_value)
    {
      valuep->data = opt->x_value->data;
      valuep->len = opt->x_value->len;
    }
  else
    {
      valuep->data = opt->value->data;
      valuep->len = opt->value->len;
    }
}



void
svn_config_get (svn_config_t *cfg, svn_string_t *valuep,
                const char *section, const char *option,
                const char *default_value)
{
  cfg_option_t *opt = find_option (cfg, section, option, NULL);
  if (opt != NULL)
    {
      make_string_from_option (valuep, cfg, opt);
    }
  else
    {
      /* TODO: Expand default_value */
      valuep->data = default_value;
      valuep->len = strlen (default_value);
    }
}



void
svn_config_set (svn_config_t *cfg,
                const char *section, const char *option,
                const char *value)
{
  cfg_section_t *sec;
  cfg_option_t *opt;

  remove_expansions (cfg);

  opt = find_option (cfg, section, option, &sec);
  if (opt != NULL)
    {
      /* Replace the option's value. */
      svn_stringbuf_set (opt->value, value);
      opt->expanded = FALSE;
      return;
    }

  /* Create a new option */
  opt = apr_palloc (cfg->pool, sizeof (*opt));
  opt->name = apr_pstrdup (cfg->pool, option);
  opt->hash_key = make_hash_key (apr_pstrdup (cfg->pool, option));

  opt->value = svn_stringbuf_create (value, cfg->pool);
  opt->x_value = NULL;
  opt->expanded = FALSE;

  if (sec == NULL)
    {
      /* Even the section doesn't exist. Create it. */
      sec = apr_palloc (cfg->pool, sizeof (*sec));
      sec->name = apr_pstrdup (cfg->pool, section);
      sec->hash_key = make_hash_key (apr_pstrdup (cfg->pool, section));
      sec->options = apr_hash_make (cfg->pool);
      apr_hash_set (cfg->sections, sec->hash_key, APR_HASH_KEY_STRING, sec);
    }

  apr_hash_set (sec->options, opt->hash_key, APR_HASH_KEY_STRING, opt);
}



int
svn_config_enumerate (svn_config_t *cfg, const char *section,
                      svn_config_enumerator_t callback, void *baton)
{
  cfg_section_t *sec;
  apr_hash_index_t *opt_ndx;
  int count;

  find_option (cfg, section, NULL, &sec);
  if (sec == NULL)
    return 0;

  count = 0;
  for (opt_ndx = apr_hash_first (cfg->x_pool, sec->options);
       opt_ndx != NULL;
       opt_ndx = apr_hash_next (opt_ndx))
    {
      void *opt_ptr;
      cfg_option_t *opt;
      svn_string_t temp_value;

      apr_hash_this (opt_ndx, NULL, NULL, &opt_ptr);
      opt = opt_ptr;

      ++count;
      make_string_from_option (&temp_value, cfg, opt);
      if (!callback (opt->name, &temp_value, baton))
        break;
    }

  return count;
}



/*
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */
