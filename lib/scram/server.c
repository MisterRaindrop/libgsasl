/* server.c --- SASL CRAM-MD5 server side functions.
 * Copyright (C) 2009  Simon Josefsson
 *
 * This file is part of GNU SASL Library.
 *
 * GNU SASL Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * GNU SASL Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GNU SASL Library; if not, write to the Free
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* Get specification. */
#include "scram.h"

/* Get malloc, free, strtoul. */
#include <stdlib.h>

/* Get ULONG_MAX. */
#include <limits.h>

/* Get memcpy, strdup, strlen. */
#include <string.h>

/* Get MAX. */
#include "minmax.h"

#include "tokens.h"
#include "parser.h"
#include "printer.h"
#include "gc.h"
#include "memxor.h"

#define DEFAULT_SALT_BYTES 12
#define SNONCE_ENTROPY_BYTES 18

struct scram_server_state
{
  int step;
  char *cfmb_str; /* copy of client first message bare */
  char *sf_str; /* copy of server first message */
  char *snonce;
  char *clientproof;
  struct scram_client_first cf;
  struct scram_server_first sf;
  struct scram_client_final cl;
  struct scram_server_final sl;
};

int
_gsasl_scram_sha1_server_start (Gsasl_session * sctx, void **mech_data)
{
  struct scram_server_state *state;
  char buf[MAX (SNONCE_ENTROPY_BYTES, DEFAULT_SALT_BYTES)];
  int rc;

  state = (struct scram_server_state *) calloc (sizeof (*state), 1);
  if (state == NULL)
    return GSASL_MALLOC_ERROR;

  rc = gsasl_nonce (buf, SNONCE_ENTROPY_BYTES);
  if (rc != GSASL_OK)
    goto end;

  rc = gsasl_base64_to (buf, SNONCE_ENTROPY_BYTES,
			&state->snonce, NULL);
  if (rc != GSASL_OK)
    goto end;

  rc = gsasl_nonce (buf, DEFAULT_SALT_BYTES);
  if (rc != GSASL_OK)
    goto end;

  rc = gsasl_base64_to (buf, DEFAULT_SALT_BYTES,
			&state->sf.salt, NULL);
  if (rc != GSASL_OK)
    goto end;

  *mech_data = state;

  return GSASL_OK;

 end:
  free (state->sf.salt);
  free (state->snonce);
  free (state);
  return rc;
}

int
_gsasl_scram_sha1_server_step (Gsasl_session * sctx,
			       void *mech_data,
			       const char *input,
			       size_t input_len,
			       char **output, size_t * output_len)
{
  struct scram_server_state *state = mech_data;
  int res = GSASL_MECHANISM_CALLED_TOO_MANY_TIMES;
  int rc;

  *output = NULL;
  *output_len = 0;

  switch (state->step)
    {
    case 0:
      {
	if (strlen (input) != input_len)
	  return GSASL_MECHANISM_PARSE_ERROR;

	if (scram_parse_client_first (input, &state->cf) < 0)
	  return GSASL_MECHANISM_PARSE_ERROR;

	{
	  const char *p;

	  /* Save "bare" for next step. */
	  p = strchr (input, ',');
	  if (!p)
	    return GSASL_AUTHENTICATION_ERROR;
	  p++;
	  p = strchr (p, ',');
	  if (!p)
	    return GSASL_AUTHENTICATION_ERROR;
	  p++;

	  state->cfmb_str = strdup (p);
	  if (!state->cfmb_str)
	    return GSASL_MALLOC_ERROR;
	}

	/* Create new nonce. */
	{
	  size_t cnlen = strlen (state->cf.client_nonce);

	  state->sf.nonce = malloc (cnlen + SNONCE_ENTROPY_BYTES + 1);
	  if (!state->sf.nonce)
	    return GSASL_MALLOC_ERROR;

	  memcpy (state->sf.nonce, state->cf.client_nonce, cnlen);
	  memcpy (state->sf.nonce + cnlen, state->snonce,
		  SNONCE_ENTROPY_BYTES);
	  state->sf.nonce[cnlen + SNONCE_ENTROPY_BYTES] = '\0';
	}

	{
	  const char *p = gsasl_property_get (sctx, GSASL_SCRAM_ITER);
	  if (p)
	    state->sf.iter = strtoul (p, NULL, 10);
	  if (!p || state->sf.iter == 0 || state->sf.iter == ULONG_MAX)
	    state->sf.iter = 4096;
	}

	{
	  const char *p = gsasl_property_get (sctx, GSASL_SCRAM_SALT);
	  if (p)
	    {
	      free (state->sf.salt);
	      state->sf.salt = strdup (p);
	    }
	}

	rc = scram_print_server_first (&state->sf, &state->sf_str);
	if (rc != 0)
	  return GSASL_MALLOC_ERROR;

	*output = strdup (state->sf_str);
	if (!*output)
	  return GSASL_MALLOC_ERROR;
	*output_len = strlen (*output);

	state->step++;
	return GSASL_NEEDS_MORE;
	break;
      }

    case 1:
      {
	if (strlen (input) != input_len)
	  return GSASL_MECHANISM_PARSE_ERROR;

	if (scram_parse_client_final (input, &state->cl) < 0)
	  return GSASL_MECHANISM_PARSE_ERROR;

	if (strcmp (state->cl.nonce, state->sf.nonce) != 0)
	  return GSASL_AUTHENTICATION_ERROR;

	/* Base64 decode client proof and check that length matches
	   SHA-1 size. */
	{
	  size_t len;

	  rc = gsasl_base64_from (state->cl.proof, strlen (state->cl.proof),
				  &state->clientproof, &len);
	  if (rc != 0)
	    return rc;
	  if (len != 20)
	    return GSASL_MECHANISM_PARSE_ERROR;
	}

	{
	  char *storedkey;
	  char *serverkey;
	  const char *p;

	  /* Get StoredKey */
	  if ((p = gsasl_property_get (sctx, GSASL_PASSWORD)))
	    {
	      Gc_rc err;
	      char *salt;
	      size_t saltlen;
	      char saltedpassword[20];
	      char *clientkey;

	      rc = gsasl_base64_from (state->sf.salt, strlen (state->sf.salt),
				      &salt, &saltlen);
	      if (rc != 0)
		return rc;

	      /* SaltedPassword := Hi(password, salt) */
	      err = gc_pbkdf2_sha1 (p, strlen (p),
				    salt, saltlen,
				    state->sf.iter, saltedpassword, 20);
	      free (salt);
	      if (err != GC_OK)
		return GSASL_MALLOC_ERROR;

	      /* ClientKey := HMAC(SaltedPassword, "Client Key") */
#define CLIENT_KEY "Client Key"
	      rc = gsasl_hmac_sha1 (saltedpassword, 20,
				    CLIENT_KEY, strlen (CLIENT_KEY),
				    &clientkey);
	      if (rc != 0)
		return rc;

	      /* StoredKey := H(ClientKey) */
	      rc = gsasl_sha1 (clientkey, 20, &storedkey);
	      free (clientkey);
	      if (rc != 0)
		return rc;

	      /* ServerKey := HMAC(SaltedPassword, "Server Key") */
#define SERVER_KEY "Server Key"
	      rc = gsasl_hmac_sha1 (saltedpassword, 20,
				    SERVER_KEY, strlen (SERVER_KEY),
				    &serverkey);
	      if (rc != 0)
		return rc;
	    }
	  else
	    return GSASL_NO_PASSWORD;

	  /* Check client proof. */
	  {
	    char *authmessage;
	    char *clientsignature;

	    /* Compute AuthMessage */
	    {
	      size_t len;

	      /* Get client-final-message-without-proof. */
	      p = strstr (input, ",p=");
	      if (!p)
		return GSASL_MECHANISM_PARSE_ERROR;
	      len = p - input;

	      asprintf (&authmessage, "%s,%.*s,%.*s",
			state->cfmb_str,
			strlen (state->sf_str), state->sf_str,
			len, input);
	    }

	    /* ClientSignature := HMAC(StoredKey, AuthMessage) */
	    rc = gsasl_hmac_sha1 (storedkey, 20,
				  authmessage, strlen (authmessage),
				  &clientsignature);
	    free (authmessage);
	    if (rc != 0)
	      return rc;

	    /* Derive ClientKey from input and check against
	       StoredKey. */
	    {
	      char *maybe_storedkey;

	      /* ClientKey := ClientProof XOR ClientSignature */
	      memxor (clientsignature, state->clientproof, 20);

	      rc = gsasl_sha1 (clientsignature, 20, &maybe_storedkey);
	      free (clientsignature);
	      if (rc != 0)
		return rc;

	      rc = memcmp (storedkey, maybe_storedkey, 20);
	      free (maybe_storedkey);
	      if (rc != 0)
		return GSASL_AUTHENTICATION_ERROR;
	    }
	  }

	  /* Generate server verifier. */
	  {
	    state->sl.verifier = strdup ("verifier");
	  }

	  free (storedkey);
	  free (serverkey);
	}

	rc = scram_print_server_final (&state->sl, output);
	if (rc != 0)
	  return GSASL_MALLOC_ERROR;
	*output_len = strlen (*output);

	state->step++;
	return GSASL_OK;
	break;
      }

    default:
      break;
    }

  return res;
}

void
_gsasl_scram_sha1_server_finish (Gsasl_session * sctx, void *mech_data)
{
  struct scram_server_state *state = mech_data;

  if (!state)
    return;
  
  free (state->cfmb_str);
  free (state->sf_str);
  free (state->snonce);
  free (state->clientproof);
  scram_free_client_first (&state->cf);
  scram_free_server_first (&state->sf);
  scram_free_client_final (&state->cl);
  scram_free_server_final (&state->sl);

  free (state);
}