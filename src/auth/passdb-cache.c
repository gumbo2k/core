/* Copyright (C) 2004 Timo Sirainen */

#include "common.h"
#include "password-scheme.h"
#include "passdb.h"
#include "passdb-cache.h"

#include <stdlib.h>

struct auth_cache *passdb_cache = NULL;

static void list_save(struct auth_request *request, const char *const *list)
{
	const char *name, *value;

	if (*list == NULL)
		return;

	for (; *list != NULL; list++) {
		t_push();
		value = strchr(*list, '=');
		if (value == NULL) {
			name = *list;
			value = "";
		} else {
			name = t_strcut(*list, '=');
			value++;
		}

		auth_request_set_field(request, name, value, NULL);
		t_pop();
	}
}

bool passdb_cache_verify_plain(struct auth_request *request, const char *key,
			       const char *password,
			       enum passdb_result *result_r, int use_expired)
{
	const char *value, *cached_pw, *scheme, *const *list;
	int ret;
	bool expired;

	if (passdb_cache == NULL || key == NULL)
		return FALSE;

	/* value = password \t ... */
	value = auth_cache_lookup(passdb_cache, request, key, &expired);
	if (value == NULL || (expired && !use_expired))
		return FALSE;

	if (*value == '\0') {
		/* negative cache entry */
		auth_request_log_info(request, "cache", "User unknown");
		*result_r = PASSDB_RESULT_USER_UNKNOWN;
		return TRUE;
	}

	list = t_strsplit(value, "\t");
        list_save(request, list + 1);

	cached_pw = list[0];
	if (*cached_pw == '\0') {
		/* NULL password */
		auth_request_log_info(request, "cache", "NULL password access");
		*result_r = PASSDB_RESULT_OK;
		return TRUE;
	}

	scheme = password_get_scheme(&cached_pw);
	i_assert(scheme != NULL);

	ret = password_verify(password, cached_pw, scheme, request->user);
	if (ret < 0) {
		auth_request_log_error(request, "cache",
				       "Unknown password scheme %s", scheme);
	} else if (ret == 0) {
		auth_request_log_info(request, "cache", "Password mismatch");
	}

	*result_r = ret > 0 ? PASSDB_RESULT_OK :
		PASSDB_RESULT_PASSWORD_MISMATCH;
	return TRUE;
}

bool passdb_cache_lookup_credentials(struct auth_request *request,
				     const char *key, const char **password_r,
				     const char **scheme_r,
				     enum passdb_result *result_r,
				     bool use_expired)
{
	const char *value, *const *list;
	bool expired;

	if (passdb_cache == NULL)
		return FALSE;

	value = auth_cache_lookup(passdb_cache, request, key, &expired);
	if (value == NULL || (expired && !use_expired))
		return FALSE;

	if (*value == '\0') {
		/* negative cache entry */
		*result_r = PASSDB_RESULT_USER_UNKNOWN;
		*password_r = NULL;
		*scheme_r = NULL;
		return TRUE;
	}

	list = t_strsplit(value, "\t");
        list_save(request, list + 1);

	*result_r = PASSDB_RESULT_OK;
	*password_r = list[0];
	*scheme_r = password_get_scheme(password_r);
	i_assert(*scheme_r != NULL);
	return TRUE;
}

void passdb_cache_init(void)
{
	const char *env;
	size_t max_size;
	unsigned int cache_ttl;

	env = getenv("CACHE_SIZE");
	if (env == NULL)
		return;

	max_size = (size_t)strtoul(env, NULL, 10) * 1024;
	if (max_size == 0)
		return;

	env = getenv("CACHE_TTL");
	if (env == NULL)
		return;

	cache_ttl = (unsigned int)strtoul(env, NULL, 10);
	if (cache_ttl == 0)
		return;

	passdb_cache = auth_cache_new(max_size, cache_ttl);
}

void passdb_cache_deinit(void)
{
	if (passdb_cache != NULL)
		auth_cache_free(passdb_cache);
}
