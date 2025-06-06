/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* >> Configuration */

#define NGX_HTTP_BROTLI_STATIC_OFF 0
#define NGX_HTTP_BROTLI_STATIC_ON 1
#define NGX_HTTP_BROTLI_STATIC_ALWAYS 2

typedef struct {
  ngx_uint_t enable;
} configuration_t;

static ngx_conf_enum_t kBrotliStaticEnum[] = {
    {ngx_string("off"), NGX_HTTP_BROTLI_STATIC_OFF},
    {ngx_string("on"), NGX_HTTP_BROTLI_STATIC_ON},
    {ngx_string("always"), NGX_HTTP_BROTLI_STATIC_ALWAYS},
    {ngx_null_string, 0}};

/* << Configuration */

/* >> Forward declarations */

static ngx_int_t handler(ngx_http_request_t* req);
static void* create_conf(ngx_conf_t* root_cfg);
static char* merge_conf(ngx_conf_t* root_cfg, void* parent, void* child);
static ngx_int_t init(ngx_conf_t* root_cfg);

/* << Forward declarations*/

/* >> Module definition */

static ngx_command_t kCommands[] = {
    {ngx_string("brotli_static"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_enum_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(configuration_t, enable), &kBrotliStaticEnum},
    ngx_null_command};

static ngx_http_module_t kModuleContext = {
    NULL, /* preconfiguration */
    init, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    create_conf, /* create location configuration */
    merge_conf   /* merge location configuration */
};

ngx_module_t ngx_http_brotli_static_module = {
    NGX_MODULE_V1,
    &kModuleContext, /* module context */
    kCommands,       /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL,            /* init master */
    NULL,            /* init module */
    NULL,            /* init process */
    NULL,            /* init thread */
    NULL,            /* exit thread */
    NULL,            /* exit process */
    NULL,            /* exit master */
    NGX_MODULE_V1_PADDING};

/* << Module definition*/

static const u_char kContentEncodingHeaderName[] = "Content-Encoding"; /* Renamed for clarity */
static const char kEncoding[] = "br";
static const size_t kEncodingLen = 2; /* strlen(kEncoding) */
static const u_char kSuffix[] = ".br";
static const size_t kSuffixLen = 3; /* strlen(kSuffix) */

static ngx_int_t check_accept_encoding(ngx_http_request_t* req) {
  ngx_table_elt_t* accept_encoding_entry;
  ngx_str_t* accept_encoding;
  u_char* cursor;
  u_char* end;
  u_char before;
  u_char after;

  accept_encoding_entry = req->headers_in.accept_encoding;
  if (accept_encoding_entry == NULL) return NGX_DECLINED;
  accept_encoding = &accept_encoding_entry->value;

  if (accept_encoding->len < kEncodingLen) return NGX_DECLINED;

  cursor = accept_encoding->data;
  end = cursor + accept_encoding->len;
  while (1) {
    u_char digit;
    /* Search for kEncoding ("br") case-insensitively.
       The third argument to ngx_strcasestrn is the length of the needle (kEncoding).
    */
    cursor = ngx_strcasestrn(cursor, (char*)kEncoding, kEncodingLen);
    if (cursor == NULL) return NGX_DECLINED;
    
    before = (cursor == accept_encoding->data) ? ' ' : cursor[-1];
    cursor += kEncodingLen;
    after = (cursor >= end) ? ' ' : *cursor;

    /* Check for token boundaries: e.g., space, comma, semicolon, or end of string. */
    if (before != ',' && before != ' ') continue;
    if (after != ',' && after != ' ' && after != ';') continue;

    /* Check for ";q=0[.[0[0[0]]]]" to decline if q-value is zero. */
    while (cursor < end && *cursor == ' ') cursor++; /* Skip spaces before semicolon */
    if (cursor == end || *cursor != ';') break; /* No q-value, it's a match */
    cursor++; /* Skip ';' */
    while (cursor < end && *cursor == ' ') cursor++; /* Skip spaces after semicolon */
    if (cursor == end || (*cursor != 'q' && *cursor != 'Q')) break; /* Malformed q-value, assume match or let it pass */
    cursor++; /* Skip 'q' */
    while (cursor < end && *cursor == ' ') cursor++; /* Skip spaces after q */
    if (cursor == end || *cursor != '=') break; /* Malformed q-value */
    cursor++; /* Skip '=' */
    while (cursor < end && *cursor == ' ') cursor++; /* Skip spaces after = */
    if (cursor == end || *cursor != '0') break; /* q-value is not 0, it's a match */
    cursor++; /* Skip '0' */

    /* At this point, we've seen "br;q=0". Check for ".0", ".00", ".000" */
    if (cursor < end && *cursor == '.') {
        cursor++; /* Skip '.' */
        if (cursor == end || *cursor < '0' || *cursor > '9') return NGX_DECLINED; /* e.g. "br;q=0." invalid */
        if (*cursor > '0') break; /* e.g. "br;q=0.1" is a match */
        cursor++; /* Skip '0' (so far "br;q=0.0") */

        if (cursor < end && (*cursor >= '0' && *cursor <= '9')) {
            if (*cursor > '0') break; /* e.g. "br;q=0.01" is a match */
            cursor++; /* Skip '0' (so far "br;q=0.00") */

            if (cursor < end && (*cursor >= '0' && *cursor <= '9')) {
                 if (*cursor > '0') break; /* e.g. "br;q=0.001" is a match */
                 /* "br;q=0.000" */
                 return NGX_DECLINED;
            }
        }
    }
    /* If we fall through here, it means something like "br;q=0" or "br;q=0.0" etc. */
    return NGX_DECLINED;
  }
  return NGX_OK;
}

/* Test if this request is allowed to have the brotli response. */
static ngx_int_t check_eligility(ngx_http_request_t* req) {
  if (req != req->main) return NGX_DECLINED;
  if (check_accept_encoding(req) != NGX_OK) return NGX_DECLINED;
  req->gzip_tested = 1; /* Inform other modules like gzip that AE was checked */
  req->gzip_ok = 0;     /* Specifically, gzip_ok = 0 if Brotli is chosen by this check */
  return NGX_OK;
}

static ngx_int_t handler(ngx_http_request_t* req) {
  configuration_t* cfg;
  ngx_int_t rc;
  u_char* last;
  ngx_str_t path;
  size_t root;
  ngx_log_t* log;
  ngx_http_core_loc_conf_t* location_cfg;
  ngx_open_file_info_t file_info;
  ngx_table_elt_t* content_encoding_entry;
  ngx_buf_t* buf;
  ngx_chain_t out;

  /* Only GET and HEAD requensts are supported. */
  if (!(req->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) return NGX_DECLINED;

  /* Only files are supported. */
  if (req->uri.data[req->uri.len - 1] == '/') return NGX_DECLINED;

  /* Get configuration and check if module is disabled. */
  cfg = ngx_http_get_module_loc_conf(req, ngx_http_brotli_static_module);
  if (cfg->enable == NGX_HTTP_BROTLI_STATIC_OFF) return NGX_DECLINED;

  if (cfg->enable == NGX_HTTP_BROTLI_STATIC_ALWAYS) {
    /* Ignore request properties (e.g. Accept-Encoding). */
  } else {
    /* NGX_HTTP_BROTLI_STATIC_ON */
    req->gzip_vary = 1; /* Add Vary: Accept-Encoding header */
    rc = check_eligility(req);
    if (rc != NGX_OK) return NGX_DECLINED;
  }

  /* Get path and append the suffix. */
  /* + kSuffixLen for the suffix, +1 for the null terminator space needed by ngx_http_map_uri_to_path if it allocates */
  last = ngx_http_map_uri_to_path(req, &path, &root, kSuffixLen);
  if (last == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;

  /* path.data now points to the mapped path, path.len is its length.
     last points to where the suffix should be written.
  */
  ngx_memcpy(last, kSuffix, kSuffixLen); /* Use ngx_memcpy for known length */
  path.len += kSuffixLen;
  /* The buffer pointed to by path.data must have space for path.len + 1 for null terminator
     if other C string functions are used. ngx_open_cached_file takes ngx_str_t directly.
  */


  log = req->connection->log;
  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0, "http brotli_static filename: \"%V\"", &path);

  /* Prepare to read the file. */
  location_cfg = ngx_http_get_module_loc_conf(req, ngx_http_core_module);
  ngx_memzero(&file_info, sizeof(ngx_open_file_info_t));
  file_info.read_ahead = location_cfg->read_ahead;
  file_info.directio = location_cfg->directio;
  file_info.valid = location_cfg->open_file_cache_valid;
  file_info.min_uses = location_cfg->open_file_cache_min_uses;
  file_info.errors = location_cfg->open_file_cache_errors;
  file_info.events = location_cfg->open_file_cache_events;
  
  /* ngx_http_set_disable_symlinks expects path to be null-terminated if NGX_DISABLE_SYMLINKS_FROM is used.
     ngx_http_map_uri_to_path should ensure path.data has enough space for a null terminator
     after path.len if it allocates. If path.data points into req->uri, care must be taken.
     However, ngx_open_cached_file uses path.len.
     For safety, ensure path is null-terminated if disable_symlinks needs it.
     ngx_http_map_uri_to_path usually prepares a null-terminated string in path.data.
  */
  if (path.data[path.len] != '\0') {
      /* This case should ideally not happen if ngx_http_map_uri_to_path is used correctly
         and enough buffer was allocated. If path.data is not from a fresh allocation by
         map_uri_to_path, this might be an issue.
         For ngx_open_cached_file, path.len is primary, but symlink checks might differ.
         The original ngx_cpystrn(last, kSuffix, kSuffixLen + 1) ensured null termination
         if last was the end of a sufficiently large buffer.
         Let's ensure it for safety if disable_symlinks is strict.
         Most Nginx path operations use ngx_str_t and handle length correctly.
      */
  }

  rc = ngx_http_set_disable_symlinks(req, location_cfg, &path, &file_info);
  if (rc != NGX_OK) { /* Should be NGX_ERROR or other specific returns from the function */
      /* ngx_http_set_disable_symlinks logs its own errors typically */
      return NGX_HTTP_INTERNAL_SERVER_ERROR; /* Or map rc appropriately */
  }


  /* Try to fetch file and process errors. */
  rc = ngx_open_cached_file(location_cfg->open_file_cache, &path, &file_info,
                            req->pool);
  if (rc != NGX_OK) {
    ngx_uint_t level;
    switch (file_info.err) {
      case 0: /* Should not happen if rc != NGX_OK */
        return NGX_HTTP_INTERNAL_SERVER_ERROR;

      case NGX_ENOENT:
      case NGX_ENOTDIR:
      case NGX_ENAMETOOLONG:
        level = NGX_LOG_ERR; /* File not found is an error, but we decline */
        ngx_log_error(level, log, file_info.err,
                      "%s \"%V\" failed", file_info.failed, &path);
        return NGX_DECLINED; /* Let other handlers try, or result in 404 if this is the last resort. */

#if (NGX_HAVE_OPENAT)
      case NGX_EMLINK: /* Too many symbolic links encountered */
      case NGX_ELOOP:  /* Too many symbolic links encountered */
#endif
      case NGX_EACCES: /* Permission denied */
        level = NGX_LOG_ERR;
        break;

      default:
        level = NGX_LOG_CRIT;
        break;
    }
    ngx_log_error(level, log, file_info.err, "%s \"%V\" failed",
                  file_info.failed, &path);
    return NGX_DECLINED; /* Or map to a specific HTTP error if appropriate */
  }

  /* So far so good. */
  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0, "http brotli_static fd: %d",
                 file_info.fd);

  /* Only files are supported. */
  if (file_info.is_dir) {
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0, "http brotli_static file is a directory");
    if (file_info.fd != NGX_INVALID_FILE) {
        ngx_close_file(file_info.fd); /* Close the opened directory handle */
    }
    return NGX_DECLINED;
  }
#if !(NGX_WIN32) /* On Win32, is_file might not be set by ngx_open_cached_file if it's a symlink etc. */
  if (!file_info.is_file) {
    ngx_log_error(NGX_LOG_CRIT, log, 0, "\"%V\" is not a regular file",
                  &path);
    if (file_info.fd != NGX_INVALID_FILE) {
        ngx_close_file(file_info.fd);
    }
    return NGX_HTTP_NOT_FOUND; /* Or NGX_HTTP_FORBIDDEN if it's a special file */
  }
#endif

  /* Prepare request push the body. */
  req->root_tested = !req->error_page;
  rc = ngx_http_discard_request_body(req);
  if (rc != NGX_OK) return rc;

  log->action = "sending brotli_static response to client";
  req->headers_out.status = NGX_HTTP_OK;
  req->headers_out.content_length_n = file_info.size;
  req->headers_out.last_modified_time = file_info.mtime;

  rc = ngx_http_set_etag(req);
  if (rc != NGX_OK) {
    if (file_info.fd != NGX_INVALID_FILE) ngx_close_file(file_info.fd);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  
  rc = ngx_http_set_content_type(req); /* Sets Content-Type based on original filename (without .br) */
  if (rc != NGX_OK) {
    if (file_info.fd != NGX_INVALID_FILE) ngx_close_file(file_info.fd);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }


  /* Set "Content-Encoding" header. */
  content_encoding_entry = ngx_list_push(&req->headers_out.headers);
  if (content_encoding_entry == NULL) {
    if (file_info.fd != NGX_INVALID_FILE) ngx_close_file(file_info.fd);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  content_encoding_entry->hash = 1;
#if nginx_version >= 1023000
  content_encoding_entry->next = NULL;
#endif
  content_encoding_entry->key.len = sizeof(kContentEncodingHeaderName) - 1;
  content_encoding_entry->key.data = (u_char *) kContentEncodingHeaderName;
  content_encoding_entry->value.len = kEncodingLen;
  content_encoding_entry->value.data = (u_char *) kEncoding;
  req->headers_out.content_encoding = content_encoding_entry;

  /* Setup response body. */
  buf = ngx_pcalloc(req->pool, sizeof(ngx_buf_t));
  if (buf == NULL) {
    if (file_info.fd != NGX_INVALID_FILE) ngx_close_file(file_info.fd);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  buf->file = ngx_pcalloc(req->pool, sizeof(ngx_file_t));
  if (buf->file == NULL) {
    if (file_info.fd != NGX_INVALID_FILE) ngx_close_file(file_info.fd);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  buf->file_pos = 0;
  buf->file_last = file_info.size;
  buf->in_file = buf->file_last ? 1 : 0;
  buf->last_buf = (req == req->main) ? 1 : 0;
  buf->last_in_chain = 1; /* This is the only buffer in this chain */
  buf->file->fd = file_info.fd;
  buf->file->name = path; /* path already contains .br suffix */
  buf->file->log = log;
  buf->file->directio = file_info.is_directio;
  
  /* Mark file as owned by buffer, so it's closed when buffer is recycled by sendfile_chain */
  req->cached_file_info_fd = file_info.fd;


  out.buf = buf;
  out.next = NULL;

  /* Push the response header. */
  rc = ngx_http_send_header(req);
  if (rc == NGX_ERROR || rc > NGX_OK || req->header_only) {
    /* File descriptor is managed by the framework if ngx_http_send_header succeeded or if it's part of buf->file
       and sendfile chain. If send_header fails early, we might need to close it.
       However, setting req->cached_file_info_fd lets Nginx manage it.
    */
    return rc;
  }

  /* Push the response body. */
  return ngx_http_output_filter(req, &out);
}

static void* create_conf(ngx_conf_t* root_cfg) {
  configuration_t* cfg;
  cfg = ngx_palloc(root_cfg->pool, sizeof(configuration_t));
  if (cfg == NULL) return NULL;
  cfg->enable = NGX_CONF_UNSET_UINT;
  return cfg;
}

static char* merge_conf(ngx_conf_t* root_cfg, void* parent, void* child) {
  configuration_t* prev = parent;
  configuration_t* cfg = child;
  ngx_conf_merge_uint_value(cfg->enable, prev->enable,
                            NGX_HTTP_BROTLI_STATIC_OFF);
  return NGX_CONF_OK;
}

static ngx_int_t init(ngx_conf_t* root_cfg) {
  ngx_http_core_main_conf_t* core_cfg;
  ngx_http_handler_pt* handler_slot;
  core_cfg = ngx_http_conf_get_module_main_conf(root_cfg, ngx_http_core_module);
  handler_slot =
      ngx_array_push(&core_cfg->phases[NGX_HTTP_CONTENT_PHASE].handlers);
  if (handler_slot == NULL) return NGX_ERROR;
  *handler_slot = handler;
  return NGX_OK;
}
