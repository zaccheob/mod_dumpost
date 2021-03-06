/*******************************************************************************
 * Copyright (c) 2012 Hoang-Vu Dang <danghvu@gmail.com>
 * This file is part of mod_dumpost
 *
 * mod_dumpost is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mod_dumpost is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mod_dumpost. If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include "httpd.h"
#include "http_connection.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_request.h"

#include "apr_strings.h"
#include "mod_dumpost.h"

#define DEBUG(request, format, ...) ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, request, format, __VA_ARGS__);

apr_size_t buffer_append(ap_filter_t *, dumpost_cfg_t *, const char *, apr_size_t);
char buffer_is_full(ap_filter_t *, dumpost_cfg_t *);
void buffer_print(ap_filter_t *, dumpost_cfg_t *);

module AP_MODULE_DECLARE_DATA dumpost_module;

/*Appends string s len bytes long into dumpost request buffer up to buffer available size. Returns number of bytes appended*/
apr_size_t buffer_append(ap_filter_t *f, dumpost_cfg_t *cfg, const char *s, apr_size_t len)
{
	if (buffer_is_full(f, cfg))
		return 0L;

	request_state *state = f->ctx;
	apr_size_t ins_len = min(len, cfg->max_size - state->buffer_used);
	strncpy(state->buffer + state->buffer_used, s, ins_len);
	state->buffer_used += ins_len;
	if (buffer_is_full(f, cfg))
	{
		ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, f->r, "DumPostMaxSize (%ld bytes) reached", (long int) cfg->max_size);
	}
	return ins_len;
}

char buffer_is_full(ap_filter_t *f, dumpost_cfg_t *cfg)
{
	request_state *state = f->ctx;
	return state->buffer_used == cfg->max_size?TRUE:FALSE;
}

/*Prints buffer on Apache error_log*/
void buffer_print(ap_filter_t *f, dumpost_cfg_t *cfg)
{
	request_state *state = f->ctx;
	if (state->buffer_printed)
		return;

	state->buffer[state->buffer_used] = '\0'; //Null terminated string: note that we need buffer max_size + 1 allocated
	// data is truncated to MAX_STRING_LEN ~ 8192 in apache
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, f->r, "%s %s%s %s", f->r->method, f->r->hostname, f->r->uri, state->buffer);
	state->buffer_printed = 1;
}


apr_status_t dumpost_input_filter (ap_filter_t *f, apr_bucket_brigade *bb, ap_input_mode_t mode, apr_read_type_e block, apr_off_t readbytes)
{

	dumpost_cfg_t *cfg = (dumpost_cfg_t *) ap_get_module_config(f->r->per_dir_config, &dumpost_module);

	apr_bucket *b;
	apr_status_t ret;

	/* restoring state */
	request_state *state = f->ctx;
	if (state == NULL)
	{
		/* create state if not yet */
		apr_pool_t *mp;
		if ((ret = apr_pool_create(&mp, f->r->pool)) != APR_SUCCESS)
		{
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, f->r, "mod_dumpost: unable to create memory pool");
			return ret;
		}
		f->ctx = state = (request_state *) apr_palloc(mp, sizeof *state);
		state->mp = mp;
		state->buffer_used = 0L;
		state->header_printed = FALSE;
		state->qs_printed = FALSE;
		state->buffer = apr_palloc(state->mp, cfg->max_size + 1); //1 byte more because string buffer is null terminated
		state->buffer_printed = FALSE;
	}

	char **headers = (cfg->headers->nelts > 0)?(char **) cfg->headers->elts : NULL;

	/* dump header if config */
	if (!buffer_is_full(f, cfg) && headers != NULL && !state->header_printed)
	{
		int i=0;
		for (; i<cfg->headers->nelts; i++)
		{
			const char *s = apr_table_get(f->r->headers_in, headers[i]);
			if (s == NULL) continue;
			buffer_append(f, cfg, s, strlen(s));
			buffer_append(f, cfg, " ", 1);
		}
		state->header_printed = 1;
	}

	/*For GET requests dump separately the QUERY_STRING*/
	if (!buffer_is_full(f, cfg) && !state->qs_printed && f->r->args && f->r->args[0] != '\0')
	{
		buffer_append(f, cfg, f->r->args, strlen(f->r->args));
		buffer_append(f, cfg, " ", 1);
		state->qs_printed = 1;
	}

	if ((ret = ap_get_brigade(f->next, bb, mode, block, readbytes)) != APR_SUCCESS)
		return ret;

	/* dump body */
	DEBUG(f->r, "Start brigade for request: %s", f->r->the_request)
	for (b = APR_BRIGADE_FIRST(bb); b != APR_BRIGADE_SENTINEL(bb); b = APR_BUCKET_NEXT(b))
	{
		if (!APR_BUCKET_IS_METADATA(b))
		{
			DEBUG(f->r, "Data bucket for request %s", f->r->the_request);
			const char * ibuf;
			apr_size_t nbytes;
			/*Read data only if buffer is not full*/
			if (!buffer_is_full(f, cfg))
			{
				if (apr_bucket_read(b, &ibuf, &nbytes, APR_BLOCK_READ) == APR_SUCCESS)
				{
					if (nbytes)
					{
						DEBUG(f->r, "%ld bytes read from bucket for request %s", (long int) nbytes, f->r->the_request);
						buffer_append(f, cfg, ibuf, nbytes);
					}
				}
				else
				{
					ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, f->r, "mod_dumpost: error reading data");
				}
			}
		}
		else
		{
			DEBUG(f->r, "Metadata bucket for request %s", f->r->the_request);
			if (APR_BUCKET_IS_EOS(b))
			{
				DEBUG(f->r, "EOS bucket for request %s", f->r->the_request);
				buffer_print(f, cfg);
			}
		}
	}

	DEBUG(f->r, "End brigade for request: %s", f->r->the_request)

	return APR_SUCCESS;
}

static void dumpost_insert_filter( request_rec *req)
{
	ap_add_input_filter("DUMPOST_IN", NULL, req, req->connection);
}

static void dumpost_register_hooks(apr_pool_t *p)
{
	ap_hook_insert_filter(dumpost_insert_filter, NULL, NULL, APR_HOOK_FIRST);
	ap_register_input_filter("DUMPOST_IN", dumpost_input_filter, NULL, AP_FTYPE_CONTENT_SET);
}

static void *dumpost_create_dconfig(apr_pool_t *mp, char *path)
{
	dumpost_cfg_t *cfg = apr_pcalloc(mp, sizeof(dumpost_cfg_t));
	cfg->max_size = DEFAULT_MAX_SIZE;
	cfg->headers = apr_array_make(mp, 0, sizeof(char *));
	cfg->pool = mp;
	return cfg;
}

static const char *dumpost_set_max_size(cmd_parms *cmd, void *_cfg, const char *arg)
{
	dumpost_cfg_t *cfg = (dumpost_cfg_t *) _cfg; //ap_get_module_config(cmd->server->module_config, &dumpost_module);
	cfg->max_size = atoi(arg);
	if (cfg->max_size == 0)
		cfg->max_size = DEFAULT_MAX_SIZE;
	return NULL;
}

static const char *dumpost_add_header(cmd_parms *cmd, void *_cfg, const char *arg)
{
	dumpost_cfg_t *cfg = (dumpost_cfg_t *) _cfg;
	*(const char**) apr_array_push(cfg->headers) = arg;
	return NULL;
}

static const command_rec dumpost_cmds[] = {
	AP_INIT_TAKE1("DumpPostMaxSize", dumpost_set_max_size, NULL,  RSRC_CONF, "Set maximum data size"),
	AP_INIT_ITERATE("DumpPostHeaderAdd", dumpost_add_header, NULL, RSRC_CONF, "Add header to log"),
	{ NULL }
};

module AP_MODULE_DECLARE_DATA dumpost_module = {
	STANDARD20_MODULE_STUFF,
	dumpost_create_dconfig,
	NULL,
	NULL, //dumpost_create_sconfig,
	NULL,
	dumpost_cmds,
	dumpost_register_hooks
};
