/* server.c --- Non-standard SASL mechanism LOGIN, server side.
 * Copyright (C) 2002, 2003, 2004  Simon Josefsson
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
 * Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307 USA
 *
 */

#include "login.h"

struct _Gsasl_login_server_state
{
  int step;
  char *username;
  char *password;
};

#define CHALLENGE_USERNAME "User Name"
#define CHALLENGE_PASSWORD "Password"

int
_gsasl_login_server_start (Gsasl_session_ctx * sctx, void **mech_data)
{
  struct _Gsasl_login_server_state *state;

  state = calloc (1, sizeof (*state));
  if (state == NULL)
    return GSASL_MALLOC_ERROR;

  *mech_data = state;

  return GSASL_OK;
}

int
_gsasl_login_server_step (Gsasl_session_ctx * sctx,
			  void *mech_data,
			  const char *input, size_t input_len,
			  char **output, size_t * output_len)
{
  struct _Gsasl_login_server_state *state = mech_data;
  int res;

  switch (state->step)
    {
    case 0:
      *output = strdup (CHALLENGE_USERNAME);
      if (!*output)
	return GSASL_MALLOC_ERROR;
      *output_len = strlen (CHALLENGE_USERNAME);

      state->step++;
      res = GSASL_NEEDS_MORE;
      break;

    case 1:
      if (input_len == 0)
	return GSASL_MECHANISM_PARSE_ERROR;

      state->username = malloc (input_len + 1);
      if (state->username == NULL)
	return GSASL_MALLOC_ERROR;

      memcpy (state->username, input, input_len);
      state->username[input_len] = '\0';

      *output = strdup (CHALLENGE_PASSWORD);
      if (!*output)
	return GSASL_MALLOC_ERROR;
      *output_len = strlen (CHALLENGE_PASSWORD);

      state->step++;
      res = GSASL_NEEDS_MORE;
      break;

    case 2:
      if (input_len == 0)
	return GSASL_MECHANISM_PARSE_ERROR;

      state->password = malloc (input_len + 1);
      if (state->password == NULL)
	return GSASL_MALLOC_ERROR;

      memcpy (state->password, input, input_len);
      state->password[input_len] = '\0';

      if (input_len != strlen (state->password))
	return GSASL_MECHANISM_PARSE_ERROR;

      gsasl_property_set (sctx, GSASL_AUTHID, state->username);
      gsasl_property_set (sctx, GSASL_PASSWORD, state->password);

      res = gsasl_callback (sctx, GSASL_SERVER_VALIDATE);
      if (res != GSASL_CANNOT_VALIDATE)
	{
	  const char *key;

	  gsasl_property_set (sctx, GSASL_AUTHZID, NULL);
	  gsasl_property_set (sctx, GSASL_PASSWORD, NULL);

	  key = gsasl_property_get (sctx, GSASL_PASSWORD);

	  if (key && strlen (state->password) == strlen (key) &&
	      strcmp (state->password, key) == 0)
	    res = GSASL_OK;
	  else
	    res = GSASL_AUTHENTICATION_ERROR;
	}

      *output_len = 0;
      *output = NULL;
      state->step++;
      break;

    default:
      res = GSASL_MECHANISM_CALLED_TOO_MANY_TIMES;
      break;
    }

  return res;
}

int
_gsasl_login_server_finish (Gsasl_session_ctx * sctx, void *mech_data)
{
  struct _Gsasl_login_server_state *state = mech_data;

  if (state->username)
    free (state->username);
  if (state->password)
    free (state->password);
  free (state);

  return GSASL_OK;
}