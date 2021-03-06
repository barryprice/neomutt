/**
 * Copyright (C) 1997-2002 Thomas Roessler <roessler@does-not-exist.org>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Generally useful, pgp-related functions. */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mutt.h"
#include "pgplib.h"
#include "lib.h"

const char *pgp_pkalgbytype(unsigned char type)
{
  switch (type)
  {
    case 1:
      return "RSA";
    case 2:
      return "RSA";
    case 3:
      return "RSA";
    case 16:
      return "ElG";
    case 17:
      return "DSA";
    case 20:
      return "ElG";
    default:
      return "unk";
  }
}

bool pgp_canencrypt(unsigned char type)
{
  switch (type)
  {
    case 1:
    case 2:
    case 16:
    case 20:
      return true;
    default:
      return false;
  }
}

bool pgp_cansign(unsigned char type)
{
  switch (type)
  {
    case 1:
    case 3:
    case 17:
    case 20:
      return true;
    default:
      return false;
  }
}

/* return values:

 * 1 = sign only
 * 2 = encrypt only
 * 3 = both
 */

short pgp_get_abilities(unsigned char type)
{
  return (pgp_canencrypt(type) << 1) | pgp_cansign(type);
}

static void pgp_free_sig(pgp_sig_t **sigp)
{
  pgp_sig_t *sp = NULL, *q = NULL;

  if (!sigp || !*sigp)
    return;

  for (sp = *sigp; sp; sp = q)
  {
    q = sp->next;
    FREE(&sp);
  }

  *sigp = NULL;
}

static void pgp_free_uid(pgp_uid_t **upp)
{
  pgp_uid_t *up = NULL, *q = NULL;

  if (!upp || !*upp)
    return;
  for (up = *upp; up; up = q)
  {
    q = up->next;
    pgp_free_sig(&up->sigs);
    FREE(&up->addr);
    FREE(&up);
  }

  *upp = NULL;
}

pgp_uid_t *pgp_copy_uids(pgp_uid_t *up, pgp_key_t parent)
{
  pgp_uid_t *l = NULL;
  pgp_uid_t **lp = &l;

  for (; up; up = up->next)
  {
    *lp = safe_calloc(1, sizeof(pgp_uid_t));
    (*lp)->trust = up->trust;
    (*lp)->flags = up->flags;
    (*lp)->addr = safe_strdup(up->addr);
    (*lp)->parent = parent;
    lp = &(*lp)->next;
  }

  return l;
}

static void _pgp_free_key(pgp_key_t *kpp)
{
  pgp_key_t kp;

  if (!kpp || !*kpp)
    return;

  kp = *kpp;

  pgp_free_uid(&kp->address);
  FREE(&kp->keyid);
  FREE(&kp->fingerprint);
  /* mutt_crypt.h: 'typedef struct pgp_keyinfo *pgp_key_t;' */
  FREE(kpp);
}

pgp_key_t pgp_remove_key(pgp_key_t *klist, pgp_key_t key)
{
  pgp_key_t *last = NULL;
  pgp_key_t p, q, r;

  if (!klist || !*klist || !key)
    return NULL;

  if (key->parent && key->parent != key)
    key = key->parent;

  last = klist;
  for (p = *klist; p && p != key; p = p->next)
    last = &p->next;

  if (!p)
    return NULL;

  for (q = p->next, r = p; q && q->parent == p; q = q->next)
    r = q;

  if (r)
    r->next = NULL;

  *last = q;
  return q;
}

void pgp_free_key(pgp_key_t *kpp)
{
  pgp_key_t p, q, r;

  if (!kpp || !*kpp)
    return;

  if ((*kpp)->parent && (*kpp)->parent != *kpp)
    *kpp = (*kpp)->parent;

  /* Order is important here:
   *
   * - First free all children.
   * - If we are an orphan (i.e., our parent was not in the key list),
   *   free our parent.
   * - free ourselves.
   */

  for (p = *kpp; p; p = q)
  {
    for (q = p->next; q && q->parent == p; q = r)
    {
      r = q->next;
      _pgp_free_key(&q);
    }
    if (p->parent)
      _pgp_free_key(&p->parent);

    _pgp_free_key(&p);
  }

  *kpp = NULL;
}
