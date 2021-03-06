/**
 * Copyright (C) 1996-2000 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2012 Michael R. Elkins <me@mutt.org>
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

#ifndef _MUTT_RFC822_H
#define _MUTT_RFC822_H 1

#include "lib.h"

/* possible values for RFC822Error */
enum
{
  ERR_MEMORY = 1,
  ERR_MISMATCH_PAREN,
  ERR_MISMATCH_QUOTE,
  ERR_BAD_ROUTE,
  ERR_BAD_ROUTE_ADDR,
  ERR_BAD_ADDR_SPEC
};

typedef struct address_t
{
#ifdef EXACT_ADDRESS
  char *val;      /* value of address as parsed */
#endif
  char *personal; /* real name of address */
  char *mailbox;  /* mailbox and host address */
  int group;      /* group mailbox? */
  struct address_t *next;
  bool is_intl : 1;
  bool intl_checked : 1;
} ADDRESS;

void rfc822_dequote_comment(char *s);
void rfc822_free_address(ADDRESS **p);
void rfc822_qualify(ADDRESS *addr, const char *host);
ADDRESS *rfc822_parse_adrlist(ADDRESS *top, const char *s);
ADDRESS *rfc822_cpy_adr(ADDRESS *addr, int prune);
ADDRESS *rfc822_cpy_adr_real(ADDRESS *addr);
ADDRESS *rfc822_append(ADDRESS **a, ADDRESS *b, int prune);
int rfc822_write_address(char *buf, size_t buflen, ADDRESS *addr, int display);
void rfc822_write_address_single(char *buf, size_t buflen, ADDRESS *addr, int display);
void rfc822_free_address(ADDRESS **p);
void rfc822_cat(char *buf, size_t buflen, const char *value, const char *specials);
bool rfc822_valid_msgid(const char *msgid);
int rfc822_remove_from_adrlist(ADDRESS **a, const char *mailbox);

extern int RFC822Error;
extern const char *const RFC822Errors[];

#define rfc822_error(x) RFC822Errors[x]

static inline ADDRESS *rfc822_new_address(void)
{
  return safe_calloc(1, sizeof(ADDRESS));
}

#endif /* _MUTT_RFC822_H */
