/**
 * Copyright (C) 1996-2000,2002,2014 Michael R. Elkins <me@mutt.org>
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

#include "config.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mutt.h"
#include "copy.h"
#include "mailbox.h"
#include "mime.h"
#include "mutt_crypt.h"
#include "mutt_curses.h"
#include "mutt_idna.h"
#include "mx.h"
#include "rfc2047.h"
#ifdef USE_NOTMUCH
#include "mutt_notmuch.h"
#endif

static int address_header_decode(char **str);
static int copy_delete_attach(BODY *b, FILE *fpin, FILE *fpout, char *date);

/* Ok, the only reason for not merging this with mutt_copy_header()
 * below is to avoid creating a HEADER structure in message_handler().
 * Also, this one will wrap headers much more aggressively than the other one.
 */
int mutt_copy_hdr(FILE *in, FILE *out, LOFF_T off_start, LOFF_T off_end,
                  int flags, const char *prefix)
{
  int from = 0;
  int this_is_from;
  int ignore = 0;
  char buf[LONG_STRING]; /* should be long enough to get most fields in one pass */
  char *nl = NULL;
  LIST *t = NULL;
  char **headers;
  int hdr_count;
  int x;
  char *this_one = NULL;
  size_t this_one_len = 0;
  int error;

  if (off_start < 0)
    return -1;

  if (ftello(in) != off_start)
    if (fseeko(in, off_start, SEEK_SET) < 0)
      return -1;

  buf[0] = '\n';
  buf[1] = 0;

  if ((flags & (CH_REORDER | CH_WEED | CH_MIME | CH_DECODE | CH_PREFIX | CH_WEED_DELIVERED)) == 0)
  {
    /* Without these flags to complicate things
     * we can do a more efficient line to line copying
     */
    while (ftello(in) < off_end)
    {
      nl = strchr(buf, '\n');

      if ((fgets(buf, sizeof(buf), in)) == NULL)
        break;

      /* Is it the beginning of a header? */
      if (nl && buf[0] != ' ' && buf[0] != '\t')
      {
        ignore = 1;
        if (!from && (mutt_strncmp("From ", buf, 5) == 0))
        {
          if ((flags & CH_FROM) == 0)
            continue;
          from = 1;
        }
        else if (flags & (CH_NOQFROM) && (ascii_strncasecmp(">From ", buf, 6) == 0))
          continue;

        else if (buf[0] == '\n' || (buf[0] == '\r' && buf[1] == '\n'))
          break; /* end of header */

        if ((flags & (CH_UPDATE | CH_XMIT | CH_NOSTATUS)) &&
            ((ascii_strncasecmp("Status:", buf, 7) == 0) ||
             (ascii_strncasecmp("X-Status:", buf, 9) == 0)))
          continue;
        if ((flags & (CH_UPDATE_LEN | CH_XMIT | CH_NOLEN)) &&
            ((ascii_strncasecmp("Content-Length:", buf, 15) == 0) ||
             (ascii_strncasecmp("Lines:", buf, 6) == 0)))
          continue;
        if ((flags & CH_UPDATE_REFS) && (ascii_strncasecmp("References:", buf, 11) == 0))
          continue;
        if ((flags & CH_UPDATE_IRT) && (ascii_strncasecmp("In-Reply-To:", buf, 12) == 0))
          continue;
        if (flags & CH_UPDATE_LABEL && (ascii_strncasecmp("X-Label:", buf, 8) == 0))
          continue;

        ignore = 0;
      }

      if (flags & CH_UPDATE_LABEL)
      {
        if ((mutt_strncasecmp("X-Label:", buf, 8) == 0) ||
            (mutt_strncasecmp("X-Keywords:", buf, 11) == 0) ||
            (mutt_strncasecmp("X-Mozilla-Keys:", buf, 15) == 0) ||
            (mutt_strncasecmp("Keywords:", buf, 9) == 0))
          continue;
      }

      if (!ignore && fputs(buf, out) == EOF)
        return -1;
    }
    return 0;
  }

  hdr_count = 1;
  x = 0;
  error = false;

  /* We are going to read and collect the headers in an array
   * so we are able to do re-ordering.
   * First count the number of entries in the array
   */
  if (flags & CH_REORDER)
  {
    for (t = HeaderOrderList; t; t = t->next)
    {
      mutt_debug(3, "Reorder list: %s\n", t->data);
      hdr_count++;
    }
  }

  mutt_debug(1, "WEED is %s\n", (flags & CH_WEED) ? "Set" : "Not");

  headers = safe_calloc(hdr_count, sizeof(char *));

  /* Read all the headers into the array */
  while (ftello(in) < off_end)
  {
    nl = strchr(buf, '\n');

    /* Read a line */
    if ((fgets(buf, sizeof(buf), in)) == NULL)
      break;

    /* Is it the beginning of a header? */
    if (nl && buf[0] != ' ' && buf[0] != '\t')
    {
      /* Do we have anything pending? */
      if (this_one)
      {
        if (flags & CH_DECODE)
        {
          if (!address_header_decode(&this_one))
            rfc2047_decode(&this_one);
          this_one_len = mutt_strlen(this_one);
        }

        if (!headers[x])
          headers[x] = this_one;
        else
        {
          int hlen = mutt_strlen(headers[x]);

          safe_realloc(&headers[x], hlen + this_one_len + sizeof(char));
          strcat(headers[x] + hlen, this_one);
          FREE(&this_one);
        }

        this_one = NULL;
      }

      ignore = 1;
      this_is_from = 0;
      if (!from && (mutt_strncmp("From ", buf, 5) == 0))
      {
        if ((flags & CH_FROM) == 0)
          continue;
        this_is_from = from = 1;
      }
      else if (buf[0] == '\n' || (buf[0] == '\r' && buf[1] == '\n'))
        break; /* end of header */

      /* note: CH_FROM takes precedence over header weeding. */
      if (!((flags & CH_FROM) && (flags & CH_FORCE_FROM) && this_is_from) &&
          (flags & CH_WEED) && mutt_matches_ignore(buf))
        continue;
      if ((flags & CH_WEED_DELIVERED) && (ascii_strncasecmp("Delivered-To:", buf, 13) == 0))
        continue;
      if ((flags & (CH_UPDATE | CH_XMIT | CH_NOSTATUS)) &&
          ((ascii_strncasecmp("Status:", buf, 7) == 0) ||
           (ascii_strncasecmp("X-Status:", buf, 9) == 0)))
        continue;
      if ((flags & (CH_UPDATE_LEN | CH_XMIT | CH_NOLEN)) &&
          ((ascii_strncasecmp("Content-Length:", buf, 15) == 0) ||
           (ascii_strncasecmp("Lines:", buf, 6) == 0)))
        continue;
      if ((flags & CH_MIME) &&
          (((ascii_strncasecmp("content-", buf, 8) == 0) &&
            ((ascii_strncasecmp("transfer-encoding:", buf + 8, 18) == 0) ||
             (ascii_strncasecmp("type:", buf + 8, 5) == 0))) ||
           (ascii_strncasecmp("mime-version:", buf, 13) == 0)))
        continue;
      if ((flags & CH_UPDATE_REFS) && (ascii_strncasecmp("References:", buf, 11) == 0))
        continue;
      if ((flags & CH_UPDATE_IRT) && (ascii_strncasecmp("In-Reply-To:", buf, 12) == 0))
        continue;

      /* Find x -- the array entry where this header is to be saved */
      if (flags & CH_REORDER)
      {
        for (t = HeaderOrderList, x = 0; (t); t = t->next, x++)
        {
          if (ascii_strncasecmp(buf, t->data, mutt_strlen(t->data)) == 0)
          {
            mutt_debug(2, "Reorder: %s matches %s\n", t->data, buf);
            break;
          }
        }
      }

      ignore = 0;
    } /* If beginning of header */

    if (!ignore)
    {
      mutt_debug(2, "Reorder: x = %d; hdr_count = %d\n", x, hdr_count);
      if (!this_one)
      {
        this_one = safe_strdup(buf);
        this_one_len = mutt_strlen(this_one);
      }
      else
      {
        int blen = mutt_strlen(buf);

        safe_realloc(&this_one, this_one_len + blen + sizeof(char));
        strcat(this_one + this_one_len, buf);
        this_one_len += blen;
      }
    }
  } /* while (ftello (in) < off_end) */

  /* Do we have anything pending?  -- XXX, same code as in above in the loop. */
  if (this_one)
  {
    if (flags & CH_DECODE)
    {
      if (!address_header_decode(&this_one))
        rfc2047_decode(&this_one);
      this_one_len = mutt_strlen(this_one);
    }

    if (!headers[x])
      headers[x] = this_one;
    else
    {
      int hlen = mutt_strlen(headers[x]);

      safe_realloc(&headers[x], hlen + this_one_len + sizeof(char));
      strcat(headers[x] + hlen, this_one);
      FREE(&this_one);
    }

    this_one = NULL;
  }

  /* Now output the headers in order */
  for (x = 0; x < hdr_count; x++)
  {
    if (headers[x])
    {
      /* We couldn't do the prefixing when reading because RFC 2047
       * decoding may have concatenated lines.
       */

      if (flags & (CH_DECODE | CH_PREFIX))
      {
        if (mutt_write_one_header(out, 0, headers[x], flags & CH_PREFIX ? prefix : 0,
                                  mutt_window_wrap_cols(MuttIndexWindow, Wrap), flags) == -1)
        {
          error = true;
          break;
        }
      }
      else
      {
        if (fputs(headers[x], out) == EOF)
        {
          error = true;
          break;
        }
      }
    }
  }

  /* Free in a separate loop to be sure that all headers are freed
   * in case of error. */
  for (x = 0; x < hdr_count; x++)
    FREE(&headers[x]);
  FREE(&headers);

  if (error)
    return -1;
  return 0;
}

/* flags
        CH_DECODE       RFC2047 header decoding
        CH_FROM         retain the "From " message separator
        CH_FORCE_FROM   give CH_FROM precedence over CH_WEED
        CH_MIME         ignore MIME fields
        CH_NOLEN        don't write Content-Length: and Lines:
        CH_NONEWLINE    don't output a newline after the header
        CH_NOSTATUS     ignore the Status: and X-Status:
        CH_PREFIX       quote header with $indent_str
        CH_REORDER      output header in order specified by `hdr_order'
        CH_TXTPLAIN     generate text/plain MIME headers [hack alert.]
        CH_UPDATE       write new Status: and X-Status:
        CH_UPDATE_LEN   write new Content-Length: and Lines:
        CH_XMIT         ignore Lines: and Content-Length:
        CH_WEED         do header weeding
        CH_NOQFROM      ignore ">From " line
        CH_UPDATE_IRT   update the In-Reply-To: header
        CH_UPDATE_REFS  update the References: header
        CH_VIRTUAL      write virtual header lines too

   prefix
        string to use if CH_PREFIX is set
 */
int mutt_copy_header(FILE *in, HEADER *h, FILE *out, int flags, const char *prefix)
{
  char buffer[SHORT_STRING];

  if (h->env)
    flags |= (h->env->irt_changed ? CH_UPDATE_IRT : 0) |
             (h->env->refs_changed ? CH_UPDATE_REFS : 0);

  if (mutt_copy_hdr(in, out, h->offset, h->content->offset, flags, prefix) == -1)
    return -1;

  if (flags & CH_TXTPLAIN)
  {
    char chsbuf[SHORT_STRING];
    fputs("MIME-Version: 1.0\n", out);
    fputs("Content-Transfer-Encoding: 8bit\n", out);
    fputs("Content-Type: text/plain; charset=", out);
    mutt_canonical_charset(chsbuf, sizeof(chsbuf),
                           Charset ? Charset : "us-ascii");
    rfc822_cat(buffer, sizeof(buffer), chsbuf, MimeSpecials);
    fputs(buffer, out);
    fputc('\n', out);
  }

  if ((flags & CH_UPDATE_IRT) && h->env->in_reply_to)
  {
    LIST *listp = h->env->in_reply_to;
    fputs("In-Reply-To:", out);
    for (; listp; listp = listp->next)
    {
      fputc(' ', out);
      fputs(listp->data, out);
    }
    fputc('\n', out);
  }

  if ((flags & CH_UPDATE_REFS) && h->env->references)
  {
    fputs("References:", out);
    mutt_write_references(h->env->references, out, 0);
    fputc('\n', out);
  }

  if ((flags & CH_UPDATE) && (flags & CH_NOSTATUS) == 0)
  {
    if (h->old || h->read)
    {
      fputs("Status: ", out);
      if (h->read)
        fputs("RO", out);
      else if (h->old)
        fputc('O', out);
      fputc('\n', out);
    }

    if (h->flagged || h->replied)
    {
      fputs("X-Status: ", out);
      if (h->replied)
        fputc('A', out);
      if (h->flagged)
        fputc('F', out);
      fputc('\n', out);
    }
  }

  if (flags & CH_UPDATE_LEN && (flags & CH_NOLEN) == 0)
  {
    fprintf(out, "Content-Length: " OFF_T_FMT "\n", h->content->length);
    if (h->lines != 0 || h->content->length == 0)
      fprintf(out, "Lines: %d\n", h->lines);
  }

#ifdef USE_NOTMUCH
  if (flags & CH_VIRTUAL)
  {
    /* Add some fake headers based on notmuch data */
    char *folder = nm_header_get_folder(h);
    if (folder && !(option(OPTWEED) && mutt_matches_ignore("folder")))
    {
      char buf[LONG_STRING];
      strfcpy(buf, folder, sizeof(buf));
      mutt_pretty_mailbox(buf, sizeof(buf));

      fputs("Folder: ", out);
      fputs(buf, out);
      fputc('\n', out);
    }
    char *tags = nm_header_get_tags(h);
    if (tags && !(option(OPTWEED) && mutt_matches_ignore("tags")))
    {
      fputs("Tags: ", out);
      fputs(tags, out);
      fputc('\n', out);
    }
  }
#endif

  if (flags & CH_UPDATE_LABEL)
  {
    h->xlabel_changed = false;
    if (h->env->x_label != NULL)
      if (fprintf(out, "X-Label: %s\n", h->env->x_label) != 10 + strlen(h->env->x_label))
        return -1;
  }

  if ((flags & CH_NONEWLINE) == 0)
  {
    if (flags & CH_PREFIX)
      fputs(prefix, out);
    fputc('\n', out); /* add header terminator */
  }

  if (ferror(out) || feof(out))
    return -1;

  return 0;
}

/* Count the number of lines and bytes to be deleted in this body */
static int count_delete_lines(FILE *fp, BODY *b, LOFF_T *length, size_t datelen)
{
  int dellines = 0;
  long l;
  int ch;

  if (b->deleted)
  {
    fseeko(fp, b->offset, SEEK_SET);
    for (l = b->length; l; l--)
    {
      ch = getc(fp);
      if (ch == EOF)
        break;
      if (ch == '\n')
        dellines++;
    }
    dellines -= 3;
    *length -= b->length - (84 + datelen);
    /* Count the number of digits exceeding the first one to write the size */
    for (l = 10; b->length >= l; l *= 10)
      (*length)++;
  }
  else
  {
    for (b = b->parts; b; b = b->next)
      dellines += count_delete_lines(fp, b, length, datelen);
  }
  return dellines;
}

/* make a copy of a message
 *
 * fpout        where to write output
 * fpin         where to get input
 * hdr          header of message being copied
 * body         structure of message being copied
 * flags
 *      MUTT_CM_NOHEADER        don't copy header
 *      MUTT_CM_PREFIX  quote header and body
 *      MUTT_CM_DECODE  decode message body to text/plain
 *      MUTT_CM_DISPLAY displaying output to the user
 *      MUTT_CM_PRINTING   printing the message
 *      MUTT_CM_UPDATE  update structures in memory after syncing
 *      MUTT_CM_DECODE_PGP      used for decoding PGP messages
 *      MUTT_CM_CHARCONV        perform character set conversion
 * chflags      flags to mutt_copy_header()
 */
int _mutt_copy_message(FILE *fpout, FILE *fpin, HEADER *hdr, BODY *body, int flags, int chflags)
{
  char prefix[SHORT_STRING];
  STATE s;
  LOFF_T new_offset = -1;
  int rc = 0;

  if (flags & MUTT_CM_PREFIX)
  {
    if (option(OPTTEXTFLOWED))
      strfcpy(prefix, ">", sizeof(prefix));
    else
      _mutt_make_string(prefix, sizeof(prefix), NONULL(Prefix), Context, hdr, 0);
  }

  if (hdr->xlabel_changed)
    chflags |= CH_UPDATE_LABEL;

  if ((flags & MUTT_CM_NOHEADER) == 0)
  {
    if (flags & MUTT_CM_PREFIX)
      chflags |= CH_PREFIX;

    else if (hdr->attach_del && (chflags & CH_UPDATE_LEN))
    {
      int new_lines;
      LOFF_T new_length = body->length;
      char date[SHORT_STRING];

      mutt_make_date(date, sizeof(date));
      int dlen = mutt_strlen(date);
      if (dlen == 0)
        return -1;

      date[5] = '\"';
      date[dlen - 1] = '\"';

      /* Count the number of lines and bytes to be deleted */
      fseeko(fpin, body->offset, SEEK_SET);
      new_lines = hdr->lines - count_delete_lines(fpin, body, &new_length, dlen);

      /* Copy the headers */
      if (mutt_copy_header(fpin, hdr, fpout, chflags | CH_NOLEN | CH_NONEWLINE, NULL))
        return -1;
      fprintf(fpout, "Content-Length: " OFF_T_FMT "\n", new_length);
      if (new_lines <= 0)
        new_lines = 0;
      else
        fprintf(fpout, "Lines: %d\n", new_lines);

      putc('\n', fpout);
      if (ferror(fpout) || feof(fpout))
        return -1;
      new_offset = ftello(fpout);

      /* Copy the body */
      if (fseeko(fpin, body->offset, SEEK_SET) < 0)
        return -1;
      if (copy_delete_attach(body, fpin, fpout, date))
        return -1;

#ifdef DEBUG
      {
        LOFF_T fail = ((ftello(fpout) - new_offset) - new_length);

        if (fail)
        {
          mutt_error("The length calculation was wrong by %ld bytes", fail);
          new_length += fail;
          mutt_sleep(1);
        }
      }
#endif

      /* Update original message if we are sync'ing a mailfolder */
      if (flags & MUTT_CM_UPDATE)
      {
        hdr->attach_del = false;
        hdr->lines = new_lines;
        body->offset = new_offset;

        /* update the total size of the mailbox to reflect this deletion */
        Context->size -= body->length - new_length;
        /*
         * if the message is visible, update the visible size of the mailbox
         * as well.
         */
        if (Context->v2r[hdr->msgno] != -1)
          Context->vsize -= body->length - new_length;

        body->length = new_length;
        mutt_free_body(&body->parts);
      }

      return 0;
    }

    if (mutt_copy_header(fpin, hdr, fpout, chflags,
                         (chflags & CH_PREFIX) ? prefix : NULL) == -1)
      return -1;

    new_offset = ftello(fpout);
  }

  if (flags & MUTT_CM_DECODE)
  {
    /* now make a text/plain version of the message */
    memset(&s, 0, sizeof(STATE));
    s.fpin = fpin;
    s.fpout = fpout;
    if (flags & MUTT_CM_PREFIX)
      s.prefix = prefix;
    if (flags & MUTT_CM_DISPLAY)
      s.flags |= MUTT_DISPLAY;
    if (flags & MUTT_CM_PRINTING)
      s.flags |= MUTT_PRINTING;
    if (flags & MUTT_CM_WEED)
      s.flags |= MUTT_WEED;
    if (flags & MUTT_CM_CHARCONV)
      s.flags |= MUTT_CHARCONV;
    if (flags & MUTT_CM_REPLYING)
      s.flags |= MUTT_REPLYING;

    if (WithCrypto && flags & MUTT_CM_VERIFY)
      s.flags |= MUTT_VERIFY;

    rc = mutt_body_handler(body, &s);
  }
  else if (WithCrypto && (flags & MUTT_CM_DECODE_CRYPT) && (hdr->security & ENCRYPT))
  {
    BODY *cur = NULL;
    FILE *fp = NULL;

    if ((WithCrypto & APPLICATION_PGP) && (flags & MUTT_CM_DECODE_PGP) &&
        (hdr->security & APPLICATION_PGP) && hdr->content->type == TYPEMULTIPART)
    {
      if (crypt_pgp_decrypt_mime(fpin, &fp, hdr->content, &cur))
        return -1;
      fputs("MIME-Version: 1.0\n", fpout);
    }

    if ((WithCrypto & APPLICATION_SMIME) && (flags & MUTT_CM_DECODE_SMIME) &&
        (hdr->security & APPLICATION_SMIME) && hdr->content->type == TYPEAPPLICATION)
    {
      if (crypt_smime_decrypt_mime(fpin, &fp, hdr->content, &cur))
        return -1;
    }

    if (!cur)
    {
      mutt_error(_("No decryption engine available for message"));
      return -1;
    }

    mutt_write_mime_header(cur, fpout);
    fputc('\n', fpout);

    if (fseeko(fp, cur->offset, SEEK_SET) < 0)
      return -1;
    if (mutt_copy_bytes(fp, fpout, cur->length) == -1)
    {
      safe_fclose(&fp);
      mutt_free_body(&cur);
      return -1;
    }
    mutt_free_body(&cur);
    safe_fclose(&fp);
  }
  else
  {
    if (fseeko(fpin, body->offset, SEEK_SET) < 0)
      return -1;
    if (flags & MUTT_CM_PREFIX)
    {
      int c;
      size_t bytes = body->length;

      fputs(prefix, fpout);

      while ((c = fgetc(fpin)) != EOF && bytes--)
      {
        fputc(c, fpout);
        if (c == '\n')
        {
          fputs(prefix, fpout);
        }
      }
    }
    else if (mutt_copy_bytes(fpin, fpout, body->length) == -1)
      return -1;
  }

  if ((flags & MUTT_CM_UPDATE) && (flags & MUTT_CM_NOHEADER) == 0 && new_offset != -1)
  {
    body->offset = new_offset;
    mutt_free_body(&body->parts);
  }

  return rc;
}

/* should be made to return -1 on fatal errors, and 1 on non-fatal errors
 * like partial decode, where it is worth displaying as much as possible */
int mutt_copy_message(FILE *fpout, CONTEXT *src, HEADER *hdr, int flags, int chflags)
{
  MESSAGE *msg = NULL;
  int r;

  if ((msg = mx_open_message(src, hdr->msgno)) == NULL)
    return -1;
  if ((r = _mutt_copy_message(fpout, msg->fp, hdr, hdr->content, flags, chflags)) == 0 &&
      (ferror(fpout) || feof(fpout)))
  {
    mutt_debug(1, "_mutt_copy_message failed to detect EOF!\n");
    r = -1;
  }
  mx_close_message(src, &msg);
  return r;
}

/* appends a copy of the given message to a mailbox
 *
 * dest         destination mailbox
 * fpin         where to get input
 * src          source mailbox
 * hdr          message being copied
 * body         structure of message being copied
 * flags        mutt_copy_message() flags
 * chflags      mutt_copy_header() flags
 */
static int _mutt_append_message(CONTEXT *dest, FILE *fpin, CONTEXT *src,
                                HEADER *hdr, BODY *body, int flags, int chflags)
{
  char buf[STRING];
  MESSAGE *msg = NULL;
  int r;

  if (fseeko(fpin, hdr->offset, SEEK_SET) < 0)
    return -1;
  if (fgets(buf, sizeof(buf), fpin) == NULL)
    return -1;

  if ((msg = mx_open_new_message(dest, hdr, is_from(buf, NULL, 0, NULL) ? 0 : MUTT_ADD_FROM)) == NULL)
    return -1;
  if (dest->magic == MUTT_MBOX || dest->magic == MUTT_MMDF)
    chflags |= CH_FROM | CH_FORCE_FROM;
  chflags |= (dest->magic == MUTT_MAILDIR ? CH_NOSTATUS : CH_UPDATE);
  r = _mutt_copy_message(msg->fp, fpin, hdr, body, flags, chflags);
  if (mx_commit_message(msg, dest) != 0)
    r = -1;

#ifdef USE_NOTMUCH
  if (msg->commited_path && dest->magic == MUTT_MAILDIR && src->magic == MUTT_NOTMUCH)
    nm_update_filename(src, NULL, msg->commited_path, hdr);
#endif

  mx_close_message(dest, &msg);
  return r;
}

int mutt_append_message(CONTEXT *dest, CONTEXT *src, HEADER *hdr, int cmflags, int chflags)
{
  MESSAGE *msg = NULL;
  int r;

  if ((msg = mx_open_message(src, hdr->msgno)) == NULL)
    return -1;
  r = _mutt_append_message(dest, msg->fp, src, hdr, hdr->content, cmflags, chflags);
  mx_close_message(src, &msg);
  return r;
}

/*
 * This function copies a message body, while deleting _in_the_copy_
 * any attachments which are marked for deletion.
 * Nothing is changed in the original message -- this is left to the caller.
 *
 * The function will return 0 on success and -1 on failure.
 */
static int copy_delete_attach(BODY *b, FILE *fpin, FILE *fpout, char *date)
{
  BODY *part = NULL;

  for (part = b->parts; part; part = part->next)
  {
    if (part->deleted || part->parts)
    {
      /* Copy till start of this part */
      if (mutt_copy_bytes(fpin, fpout, part->hdr_offset - ftello(fpin)))
        return -1;

      if (part->deleted)
      {
        fprintf(
            fpout,
            "Content-Type: message/external-body; access-type=x-mutt-deleted;\n"
            "\texpiration=%s; length=" OFF_T_FMT "\n"
            "\n",
            date + 5, part->length);
        if (ferror(fpout))
          return -1;

        /* Copy the original mime headers */
        if (mutt_copy_bytes(fpin, fpout, part->offset - ftello(fpin)))
          return -1;

        /* Skip the deleted body */
        fseeko(fpin, part->offset + part->length, SEEK_SET);
      }
      else
      {
        if (copy_delete_attach(part, fpin, fpout, date))
          return -1;
      }
    }
  }

  /* Copy the last parts */
  if (mutt_copy_bytes(fpin, fpout, b->offset + b->length - ftello(fpin)))
    return -1;

  return 0;
}

/*
 * This function is the equivalent of mutt_write_address_list(),
 * but writes to a buffer instead of writing to a stream.
 * mutt_write_address_list could be re-used if we wouldn't store
 * all the decoded headers in a huge array, first.
 *
 * XXX - fix that.
 */
static void format_address_header(char **h, ADDRESS *a)
{
  char buf[HUGE_STRING];
  char cbuf[STRING];
  char c2buf[STRING];
  char *p = NULL;
  int l, linelen, buflen, count, cbuflen, c2buflen, plen;

  linelen = mutt_strlen(*h);
  plen = linelen;
  buflen = linelen + 3;

  safe_realloc(h, buflen);
  for (count = 0; a; a = a->next, count++)
  {
    ADDRESS *tmp = a->next;
    a->next = NULL;
    *buf = *cbuf = *c2buf = '\0';
    l = rfc822_write_address(buf, sizeof(buf), a, 0);
    a->next = tmp;

    if (count && linelen + l > 74)
    {
      strcpy(cbuf, "\n\t");
      linelen = l + 8;
    }
    else
    {
      if (a->mailbox)
      {
        strcpy(cbuf, " ");
        linelen++;
      }
      linelen += l;
    }
    if (!a->group && a->next && a->next->mailbox)
    {
      linelen++;
      buflen++;
      strcpy(c2buf, ",");
    }

    cbuflen = mutt_strlen(cbuf);
    c2buflen = mutt_strlen(c2buf);
    buflen += l + cbuflen + c2buflen;
    safe_realloc(h, buflen);
    p = *h;
    strcat(p + plen, cbuf);
    plen += cbuflen;
    strcat(p + plen, buf);
    plen += l;
    strcat(p + plen, c2buf);
    plen += c2buflen;
  }

  /* Space for this was allocated in the beginning of this function. */
  strcat(p + plen, "\n");
}

static int address_header_decode(char **h)
{
  char *s = *h;
  int l, rp = 0;

  ADDRESS *a = NULL;
  ADDRESS *cur = NULL;

  switch (tolower((unsigned char) *s))
  {
    case 'r':
    {
      if (ascii_strncasecmp(s, "return-path:", 12) == 0)
      {
        l = 12;
        rp = 1;
        break;
      }
      else if (ascii_strncasecmp(s, "reply-to:", 9) == 0)
      {
        l = 9;
        break;
      }
      return 0;
    }
    case 'f':
    {
      if (ascii_strncasecmp(s, "from:", 5) != 0)
        return 0;
      l = 5;
      break;
    }
    case 'c':
    {
      if (ascii_strncasecmp(s, "cc:", 3) != 0)
        return 0;
      l = 3;
      break;
    }
    case 'b':
    {
      if (ascii_strncasecmp(s, "bcc:", 4) != 0)
        return 0;
      l = 4;
      break;
    }
    case 's':
    {
      if (ascii_strncasecmp(s, "sender:", 7) != 0)
        return 0;
      l = 7;
      break;
    }
    case 't':
    {
      if (ascii_strncasecmp(s, "to:", 3) != 0)
        return 0;
      l = 3;
      break;
    }
    case 'm':
    {
      if (ascii_strncasecmp(s, "mail-followup-to:", 17) != 0)
        return 0;
      l = 17;
      break;
    }
    default:
      return 0;
  }

  if ((a = rfc822_parse_adrlist(a, s + l)) == NULL)
    return 0;

  mutt_addrlist_to_local(a);
  rfc2047_decode_adrlist(a);
  for (cur = a; cur; cur = cur->next)
    if (cur->personal)
      rfc822_dequote_comment(cur->personal);

  /* angle brackets for return path are mandated by RfC5322,
   * so leave Return-Path as-is */
  if (rp)
    *h = safe_strdup(s);
  else
  {
    *h = safe_calloc(1, l + 2);
    strfcpy(*h, s, l + 1);
    format_address_header(h, a);
  }

  rfc822_free_address(&a);

  FREE(&s);
  return 1;
}
