/*
 * hash.c :  dumping and reading hash tables to/from files.
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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



#include <stdlib.h>
#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_file_io.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_hash.h"
#include "svn_io.h"
#include "svn_pools.h"



/* 
 * The format of a dumped hash table is:
 *
 *   K <nlength>
 *   name (a string of <nlength> bytes, followed by a newline)
 *   V <vlength>
 *   val (a string of <vlength> bytes, followed by a newline)
 *   [... etc, etc ...]
 *   END
 *
 *
 * (Yes, there is a newline after END.)
 *
 * For example:
 *
 *   K 5
 *   color
 *   V 3
 *   red
 *   K 11
 *   wine review
 *   V 376
 *   A forthright entrance, yet coquettish on the tongue, its deceptively
 *   fruity exterior hides the warm mahagony undercurrent that is the
 *   hallmark of Chateau Fraisant-Pitre.  Connoisseurs of the region will
 *   be pleased to note the familiar, subtle hints of mulberries and
 *   carburator fluid.  Its confident finish is marred only by a barely
 *   detectable suggestion of rancid squid ink.
 *   K 5 
 *   price
 *   V 8
 *   US $6.50
 *   END
 *
 */




/*** Code. ***/

svn_error_t *
svn_hash_write (apr_hash_t *hash, 
                apr_file_t *destfile,
                apr_pool_t *pool)
{
  apr_hash_index_t *this;      /* current hash entry */
  apr_pool_t *iterpool;

  iterpool = svn_pool_create (pool);

  for (this = apr_hash_first (pool, hash); this; this = apr_hash_next (this))
    {
      const void *key;
      void *val;
      apr_ssize_t keylen;
      const char* buf;
      const svn_string_t *value;

      svn_pool_clear (iterpool);

      /* Get this key and val. */
      apr_hash_this (this, &key, &keylen, &val);

      /* Output name length, then name. */
      SVN_ERR (svn_io_file_write_full (destfile, "K ", 2, NULL, iterpool));

      buf = apr_psprintf (iterpool, "%" APR_SSIZE_T_FMT, keylen);
      SVN_ERR (svn_io_file_write_full (destfile, 
                                       buf, strlen (buf), NULL, iterpool));
      SVN_ERR (svn_io_file_write_full (destfile, "\n", 1, NULL, iterpool));

      SVN_ERR (svn_io_file_write_full (destfile, 
                                       (const char *) key, keylen, 
                                       NULL, iterpool));
      SVN_ERR (svn_io_file_write_full (destfile, "\n", 1, NULL, iterpool));

      /* Output value length, then value. */
      value = val;

      SVN_ERR (svn_io_file_write_full (destfile, "V ", 2, NULL, pool));

      buf = apr_psprintf (iterpool, "%" APR_SIZE_T_FMT, value->len);
      SVN_ERR (svn_io_file_write_full (destfile, buf, 
                                       strlen (buf), NULL, iterpool));
      SVN_ERR (svn_io_file_write_full (destfile, "\n", 1, NULL, iterpool));

      SVN_ERR (svn_io_file_write_full (destfile, value->data, value->len, 
                                       NULL, iterpool));
      SVN_ERR (svn_io_file_write_full (destfile, "\n", 1, NULL, iterpool));
    }

  svn_pool_destroy (iterpool);

  SVN_ERR (svn_io_file_write_full (destfile, "END\n", 4, NULL, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_hash_read (apr_hash_t *hash, 
               apr_file_t *srcfile,
               apr_pool_t *pool)
{
  svn_error_t *err;
  char buf[SVN_KEYLINE_MAXLEN];
  apr_size_t num_read;
  char c;
  int first_time = 1;
  

  while (1)
    {
      /* Read a key length line.  Might be END, though. */
      apr_size_t len = sizeof(buf);

      err = svn_io_read_length_line (srcfile, buf, &len, pool);
      if (err && APR_STATUS_IS_EOF(err->apr_err) && first_time)
        {
          /* We got an EOF on our very first attempt to read, which
             means it's a zero-byte file.  No problem, just go home. */        
          svn_error_clear (err);
          return SVN_NO_ERROR;
        }
      else if (err)
        /* Any other circumstance is a genuine error. */
        return err;

      first_time = 0;

      if (((len == 3) && (buf[0] == 'E') && (buf[1] == 'N') && (buf[2] == 'D'))
          || ((len == 9)
              && (buf[0] == 'P')
              && (buf[1] == 'R')       /* We formerly used just "END" to */
              && (buf[2] == 'O')       /* end a property hash, but later */
              && (buf[3] == 'P')       /* we added "PROPS-END", so that  */
              && (buf[4] == 'S')       /* the fs dump format would be    */
              && (buf[5] == '-')       /* more human-readable.  That's   */
              && (buf[6] == 'E')       /* why we accept either way here. */
              && (buf[7] == 'N')
              && (buf[8] == 'D')))
        {
          /* We've reached the end of the dumped hash table, so leave. */
          return SVN_NO_ERROR;
        }
      else if ((buf[0] == 'K') && (buf[1] == ' '))
        {
          /* Get the length of the key */
          size_t keylen = (size_t) atoi (buf + 2);

          /* Now read that much into a buffer, + 1 byte for null terminator */
          void *keybuf = apr_palloc (pool, keylen + 1);
          SVN_ERR (svn_io_file_read_full (srcfile, 
                                          keybuf, keylen, &num_read, pool));
          ((char *) keybuf)[keylen] = '\0';

          /* Suck up extra newline after key data */
          SVN_ERR (svn_io_file_getc (&c, srcfile, pool));
          if (c != '\n') 
            return svn_error_create (SVN_ERR_MALFORMED_FILE, NULL, NULL);

          /* Read a val length line */
          len = sizeof(buf);
          SVN_ERR (svn_io_read_length_line (srcfile, buf, &len, pool));

          if ((buf[0] == 'V') && (buf[1] == ' '))
            {
              svn_string_t *value = apr_palloc (pool, sizeof (*value));

              /* Get the length of the value */
              int vallen = atoi (buf + 2);

              /* Again, 1 extra byte for the null termination. */
              void *valbuf = apr_palloc (pool, vallen + 1);
              SVN_ERR (svn_io_file_read_full (srcfile, 
                                              valbuf, vallen, 
                                              &num_read, pool));
              ((char *) valbuf)[vallen] = '\0';

              /* Suck up extra newline after val data */
              SVN_ERR (svn_io_file_getc (&c, srcfile, pool));
              if (c != '\n')
                return svn_error_create (SVN_ERR_MALFORMED_FILE, NULL, NULL);

              value->data = valbuf;
              value->len = vallen;

              /* The Grand Moment:  add a new hash entry! */
              apr_hash_set (hash, keybuf, keylen, value);
            }
          else
            {
              return svn_error_create (SVN_ERR_MALFORMED_FILE, NULL, NULL);
            }
        }
      else
        {
          return svn_error_create (SVN_ERR_MALFORMED_FILE, NULL, NULL);
        }
    } /* while (1) */
}


svn_error_t *
svn_hash_diff (apr_hash_t *hash_a,
               apr_hash_t *hash_b,
               svn_hash_diff_func_t diff_func,
               void *diff_func_baton,
               apr_pool_t *pool)
{
  apr_hash_index_t *hi;

  if (hash_a)
    for (hi = apr_hash_first (pool, hash_a); hi; hi = apr_hash_next (hi))
      {
        const void *key;
        apr_ssize_t klen;
        
        apr_hash_this (hi, &key, &klen, NULL);
        
        if (hash_b && (apr_hash_get (hash_b, key, klen)))
          SVN_ERR ((*diff_func) (key, klen, svn_hash_diff_key_both,
                                 diff_func_baton));
        else
          SVN_ERR ((*diff_func) (key, klen, svn_hash_diff_key_a,
                                 diff_func_baton));
      }

  if (hash_b)
    for (hi = apr_hash_first (pool, hash_b); hi; hi = apr_hash_next (hi))
      {
        const void *key;
        apr_ssize_t klen;
        
        apr_hash_this (hi, &key, &klen, NULL);
        
        if (! (hash_a && apr_hash_get (hash_a, key, klen)))
          SVN_ERR ((*diff_func) (key, klen, svn_hash_diff_key_b,
                                 diff_func_baton));
      }

  return SVN_NO_ERROR;
}
