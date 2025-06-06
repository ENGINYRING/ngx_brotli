/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_HAVE_BROTLI_ENC_ENCODE_H)
#include <brotli/enc/encode.h>
#else
#include <brotli/encode.h>
#endif

/* Brotli and GZip modules never stack, i.e. when one of them sets
   "Content-Encoding" the other becomes a pass-through filter. Consequently,
   it is almost legal to reuse this "buffered" bit.
   IIUC, buffered == some data passed to filter has not been pushed further. */
#define NGX_HTTP_BROTLI_BUFFERED NGX_HTTP_GZIP_BUFFERED

/* Module configuration. */
typedef struct {
  ngx_flag_t enable;

  /* Supported MIME types. */
  ngx_hash_t types;
  ngx_array_t* types_keys;

  /* Minimal required length for compression (if known). */
  ssize_t min_length;

  ngx_bufs_t deprecated_unused_bufs;

  /* Brotli encoder parameter: quality */
  ngx_int_t quality;

  /* Brotli encoder parameter: (max) lg_win */
  size_t lg_win;
} ngx_http_brotli_conf_t;

/* Instance context. */
typedef struct {
  /* Brotli encoder instance. */
  BrotliEncoderState* encoder;

  /* Payload length; -1, if unknown. */
  off_t content_length;

  /* (uncompressed) bytes pushed to encoder. */
  size_t bytes_in;
  /* (compressed) bytes pulled from encoder. */
  size_t bytes_out;

  /* Input buffer chain. */
  ngx_chain_t* in;

  /* Output chain. */
  ngx_chain_t* out_chain;

  /* Output buffer. */
  ngx_buf_t* out_buf;

  /* Various state flags. */

  /* 1 if encoder is initialized, output chain and buffer are allocated. */
  unsigned initialized : 1;
  /* 1 if compression is finished / failed. */
  unsigned closed : 1;
  /* 1 if compression is finished. */
  unsigned success : 1;

  /* 1 if out_chain is ready to be committed, 0 otherwise. */
  unsigned output_ready : 1;
  /* 1 if output buffer is committed to the next filter and not yet fully used.
     0 otherwise. */
  unsigned output_busy : 1;

  unsigned end_of_input : 1;
  unsigned end_of_block : 1;

  ngx_http_request_t* request;
} ngx_http_brotli_ctx_t;

/* Forward declarations. */

/* Initializes encoder, output chain and buffer, if necessary. Returns NGX_OK
   if encoder is successfully initialized (have been already initialized),
   and requires objects are allocated. Returns NGX_ERROR otherwise. */
static ngx_int_t ngx_http_brotli_filter_ensure_stream_initialized(
    ngx_http_request_t* r, ngx_http_brotli_ctx_t* ctx);
/* Marks instance as closed and performs cleanup. */
static void ngx_http_brotli_filter_close(ngx_http_brotli_ctx_t* ctx);

static void* ngx_http_brotli_filter_alloc(void* opaque, size_t size);
static void ngx_http_brotli_filter_free(void* opaque, void* address);

static ngx_int_t ngx_http_brotli_check_request(ngx_http_request_t* r);

static ngx_int_t ngx_http_brotli_add_variables(ngx_conf_t* cf);
static ngx_int_t ngx_http_brotli_ratio_variable(ngx_http_request_t* r,
                                                ngx_http_variable_value_t* v,
                                                uintptr_t data);

static void* ngx_http_brotli_create_conf(ngx_conf_t* cf);
static char* ngx_http_brotli_merge_conf(ngx_conf_t* cf, void* parent,
                                        void* child);
static ngx_int_t ngx_http_brotli_filter_init(ngx_conf_t* cf);

static char* ngx_http_brotli_parse_wbits(ngx_conf_t* cf, void* post,
                                         void* data);

/* Configuration literals. */

static ngx_conf_num_bounds_t ngx_http_brotli_comp_level_bounds = {
    ngx_conf_check_num_bounds, BROTLI_MIN_QUALITY, BROTLI_MAX_QUALITY};

static ngx_conf_post_handler_pt ngx_http_brotli_parse_wbits_p =
    ngx_http_brotli_parse_wbits;

static ngx_command_t ngx_http_brotli_filter_commands[] = {
    {ngx_string("brotli"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_HTTP_LIF_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_brotli_conf_t, enable), NULL},

    /* Deprecated, unused. */
    {ngx_string("brotli_buffers"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE2,
     ngx_conf_set_bufs_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_brotli_conf_t, deprecated_unused_bufs), NULL},

    {ngx_string("brotli_types"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_1MORE,
     ngx_http_types_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_brotli_conf_t, types_keys),
     &ngx_http_html_default_types[0]},

    {ngx_string("brotli_comp_level"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_brotli_conf_t, quality),
     &ngx_http_brotli_comp_level_bounds},

    {ngx_string("brotli_window"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_size_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_brotli_conf_t, lg_win), &ngx_http_brotli_parse_wbits_p},

    {ngx_string("brotli_min_length"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_size_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_brotli_conf_t, min_length), NULL},

    ngx_null_command};

/* Module context hooks. */
static ngx_http_module_t ngx_http_brotli_filter_module_ctx = {
    ngx_http_brotli_add_variables, /* pre-configuration */
    ngx_http_brotli_filter_init,   /* post-configuration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_brotli_create_conf, /* create location configuration */
    ngx_http_brotli_merge_conf   /* merge location configuration */
};

/* Module descriptor. */
ngx_module_t ngx_http_brotli_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_brotli_filter_module_ctx, /* module context */
    ngx_http_brotli_filter_commands,    /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING};

/* Variable names. */
static ngx_str_t ngx_http_brotli_ratio = ngx_string("brotli_ratio");

/* Next filter in the filter chain. */
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_next_body_filter;

static const char kEncoding[] = "br";
static const size_t kEncodingLen = 2; /* strlen(kEncoding) */

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

/* Process headers and decide if request is eligible for brotli compression. */
static ngx_int_t ngx_http_brotli_header_filter(ngx_http_request_t* r) {
  ngx_table_elt_t* h;
  ngx_http_brotli_ctx_t* ctx;
  ngx_http_brotli_conf_t* conf;

  conf = ngx_http_get_module_loc_conf(r, ngx_http_brotli_filter_module);

  /* Filter only if enabled. */
  if (!conf->enable) {
    return ngx_http_next_header_filter(r);
  }

  /* Only compress OK / forbidden / not found responses. */
  if (r->headers_out.status != NGX_HTTP_OK &&
      r->headers_out.status != NGX_HTTP_FORBIDDEN &&
      r->headers_out.status != NGX_HTTP_NOT_FOUND) {
    return ngx_http_next_header_filter(r);
  }

  /* Bypass "header only" responses. */
  if (r->header_only) {
    return ngx_http_next_header_filter(r);
  }

  /* Bypass already compressed responses. */
  if (r->headers_out.content_encoding &&
      r->headers_out.content_encoding->value.len) {
    return ngx_http_next_header_filter(r);
  }

  /* If response size is known, do not compress tiny responses. */
  if (r->headers_out.content_length_n != -1 &&
      r->headers_out.content_length_n < conf->min_length) {
    return ngx_http_next_header_filter(r);
  }

  /* Compress only certain MIME-typed responses. */
  if (ngx_http_test_content_type(r, &conf->types) == NULL) {
    return ngx_http_next_header_filter(r);
  }

  r->gzip_vary = 1;

  /* Check if client support brotli encoding. */
  if (ngx_http_brotli_check_request(r) != NGX_OK) {
    return ngx_http_next_header_filter(r);
  }

  /* Prepare instance context. */
  ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_brotli_ctx_t));
  if (ctx == NULL) {
    return NGX_ERROR;
  }
  ctx->request = r;
  ctx->content_length = r->headers_out.content_length_n;
  ngx_http_set_ctx(r, ctx, ngx_http_brotli_filter_module);

  /* Prepare response headers, so that following filters in the chain will
     notice that response body is compressed. */
  h = ngx_list_push(&r->headers_out.headers);
  if (h == NULL) {
    return NGX_ERROR;
  }

  h->hash = 1;
#if nginx_version >= 1023000
  h->next = NULL;
#endif
  ngx_str_set(&h->key, "Content-Encoding");
  ngx_str_set(&h->value, "br");
  r->headers_out.content_encoding = h;

  r->main_filter_need_in_memory = 1;

  ngx_http_clear_content_length(r);
  ngx_http_clear_accept_ranges(r);
  ngx_http_weak_etag(r);

  return ngx_http_next_header_filter(r);
}

/* Response body filtration (compression). */
static ngx_int_t ngx_http_brotli_body_filter(ngx_http_request_t* r,
                                             ngx_chain_t* in) {
  int rc;
  ngx_http_brotli_ctx_t* ctx;
  size_t available_output;
  ptrdiff_t available_busy_output;
  size_t input_size;
  size_t available_input;
  const uint8_t* next_input_byte;
  size_t consumed_input;
  BROTLI_BOOL ok;
  u_char* out_ptr; /* Renamed from out to avoid conflict with ngx_chain_t *out */
  ngx_chain_t* link;

  ctx = ngx_http_get_module_ctx(r, ngx_http_brotli_filter_module);

  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "http brotli filter");

  if (ctx == NULL || ctx->closed || r->header_only) {
    return ngx_http_next_body_filter(r, in);
  }

  if (ngx_http_brotli_filter_ensure_stream_initialized(r, ctx) != NGX_OK) {
    ngx_http_brotli_filter_close(ctx);
    return NGX_ERROR;
  }

  /* If more input is provided - append it to our input chain. */
  if (in) {
    if (ngx_chain_add_copy(r->pool, &ctx->in, in) != NGX_OK) {
      ngx_http_brotli_filter_close(ctx);
      return NGX_ERROR;
    }
    r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;
  }

  /* Main loop:
     - if output is not yet consumed - stop; encoder should not be touched,
       until all the output is consumed
     - if encoder has output - wrap it and send to consumer
     - if encoder is finished (and all output is consumed) - stop
     - if there is more input - push it to encoder */
  for (;;) {
    if (ctx->output_busy || ctx->output_ready) {
      if (ctx->output_busy) {
        available_busy_output = ngx_buf_size(ctx->out_buf);
      } else {
        available_busy_output = 0;
      }

      rc = ngx_http_next_body_filter(r,
                                     ctx->output_ready ? ctx->out_chain : NULL);
      if (ctx->output_ready) {
        ctx->output_ready = 0;
        ctx->output_busy = 1;
      }
      if (ngx_buf_size(ctx->out_buf) == 0) {
        ctx->output_busy = 0;
      }
      if (rc == NGX_OK) {
        if (ctx->output_busy &&
            available_busy_output == ngx_buf_size(ctx->out_buf)) {
          r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;
          return NGX_AGAIN;
        }
        continue;
      } else if (rc == NGX_AGAIN) {
        if (ctx->output_busy) {
          /* Can't continue compression, let the outer filer decide. */
          if (ctx->in != NULL) {
            r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED;
          }
          return NGX_AGAIN;
        } else {
          /* Inner filter has given up, but we can continue processing. */
          continue;
        }
      } else {
        ngx_http_brotli_filter_close(ctx);
        return NGX_ERROR;
      }
    }

    if (BrotliEncoderHasMoreOutput(ctx->encoder)) {
      available_output = 0;
      out_ptr = (u_char*)BrotliEncoderTakeOutput(ctx->encoder, &available_output);
      if (out_ptr == NULL || available_output == 0) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "BrotliEncoderTakeOutput() failed or returned no data when HasMoreOutput was true");
        ngx_http_brotli_filter_close(ctx);
        return NGX_ERROR;
      }
      ctx->out_buf->start = out_ptr;
      ctx->out_buf->pos = out_ptr;
      ctx->out_buf->last = out_ptr + available_output;
      ctx->out_buf->end = out_ptr + available_output;
      ctx->bytes_out += available_output;
      ctx->out_buf->last_buf = 0;
      ctx->out_buf->flush = 0;
      if (ctx->end_of_input && BrotliEncoderIsFinished(ctx->encoder)) {
        ctx->out_buf->last_buf = 1;
        r->connection->buffered &= ~NGX_HTTP_BROTLI_BUFFERED;
      } else if (ctx->end_of_block) {
        ctx->out_buf->flush = 1;
        r->connection->buffered &= ~NGX_HTTP_BROTLI_BUFFERED;
      }
      ctx->end_of_block = 0;
      ctx->output_ready = 1;
      ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                     "brotli out: %p, size:%uz", ctx->out_buf,
                     ngx_buf_size(ctx->out_buf));
      continue;
    }

    if (BrotliEncoderIsFinished(ctx->encoder)) {
      ctx->success = 1;
      r->connection->buffered &= ~NGX_HTTP_BROTLI_BUFFERED;
      ngx_http_brotli_filter_close(ctx);
      return NGX_OK;
    }

    if (ctx->end_of_input) {
      // Ask the encoder to dump the leftover.
      available_input = 0;
      available_output = 0; /* Encoder might still produce output */
      next_input_byte = NULL;
      ok = BrotliEncoderCompressStream(ctx->encoder, BROTLI_OPERATION_FINISH,
                                       &available_input, &next_input_byte,
                                       &available_output, NULL, NULL);
      r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED; /* May still buffer output */
      if (!ok) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "BrotliEncoderCompressStream(FINISH) failed");
        ngx_http_brotli_filter_close(ctx);
        return NGX_ERROR;
      }
      continue;
    }

    if (ctx->in == NULL) {
      return NGX_OK;
    }

    /* TODO: coalesce tiny inputs, if they are not last/flush. */
    input_size = ngx_buf_size(ctx->in->buf);
    if (input_size == 0) {
      if (!ctx->in->buf->last_buf && !ctx->in->buf->flush) {
        link = ctx->in;
        ctx->in = ctx->in->next;
        ngx_free_chain(r->pool, link);
        continue;
      }
    }

    available_input = input_size;
    next_input_byte = (const uint8_t*)ctx->in->buf->pos;
    available_output = 0; /* Encoder might still produce output */
    ok = BrotliEncoderCompressStream(
        ctx->encoder,
        ctx->in->buf->last_buf ? BROTLI_OPERATION_FINISH
                               : ctx->in->buf->flush ? BROTLI_OPERATION_FLUSH
                                                     : BROTLI_OPERATION_PROCESS,
        &available_input, &next_input_byte, &available_output, NULL, NULL);
    r->connection->buffered |= NGX_HTTP_BROTLI_BUFFERED; /* May still buffer output */
    if (!ok) {
      ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                    "BrotliEncoderCompressStream(PROCESS/FLUSH/FINISH) failed");
      ngx_http_brotli_filter_close(ctx);
      return NGX_ERROR;
    }

    consumed_input = input_size - available_input;
    ctx->bytes_in += consumed_input;
    ctx->in->buf->pos += consumed_input;

    if (consumed_input == input_size) {
      if (ctx->in->buf->last_buf) {
        ctx->end_of_input = 1;
      } else if (ctx->in->buf->flush) {
        ctx->end_of_block = 1;
      }
      link = ctx->in;
      ctx->in = ctx->in->next;
      ngx_free_chain(r->pool, link);
      continue;
    }

    /* Should only happen if available_input was not fully consumed but no error.
       Could be due to internal buffering in Brotli or if output buffer is needed.
       The loop continues and will try to take output or feed more input.
       If no input was consumed and no output was produced and not finished, it might be an issue.
    */
    if (consumed_input == 0 && !BrotliEncoderHasMoreOutput(ctx->encoder) && !ctx->end_of_input) {
        /* This case might lead to an infinite loop if Brotli is stuck.
           However, BrotliEncoderCompressStream should normally consume input
           or produce output or finish.
           If input is not consumed, it might be waiting for more input to form a block,
           or internal state prevents immediate processing.
           The outer loop structure should handle this by checking HasMoreOutput.
        */
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "brotli filter: consumed 0 bytes of input, but not finished and no output yet");
    }
  }

  /* unreachable */
  ngx_http_brotli_filter_close(ctx);
  return NGX_ERROR;
}

static ngx_int_t ngx_http_brotli_filter_ensure_stream_initialized(
    ngx_http_request_t* r, ngx_http_brotli_ctx_t* ctx) {
  ngx_http_brotli_conf_t* conf;
  BROTLI_BOOL ok;
  size_t wbits;

  if (ctx->initialized) {
    return NGX_OK;
  }
  ctx->initialized = 1;

  conf = ngx_http_get_module_loc_conf(r, ngx_http_brotli_filter_module);

  /* Tune lg_win, if size is known. */
  if (ctx->content_length > 0 && ctx->content_length <= (1 << BROTLI_MAX_WINDOW_BITS)) {
    wbits = BROTLI_MIN_WINDOW_BITS;
    /* Find smallest window that is still >= content_length, up to conf->lg_win */
    while ( (1u << wbits) < (size_t)ctx->content_length && wbits < BROTLI_MAX_WINDOW_BITS) {
        wbits++;
    }
    if (wbits > conf->lg_win) { /* respect configured max window */
        wbits = conf->lg_win;
    }
  } else {
    wbits = conf->lg_win;
  }
  /* Ensure wbits is within Brotli's valid range, just in case. */
  if (wbits < BROTLI_MIN_WINDOW_BITS) wbits = BROTLI_MIN_WINDOW_BITS;
  if (wbits > BROTLI_MAX_WINDOW_BITS) wbits = BROTLI_MAX_WINDOW_BITS;


  ctx->encoder = BrotliEncoderCreateInstance(
      ngx_http_brotli_filter_alloc, ngx_http_brotli_filter_free, r->pool);
  if (ctx->encoder == NULL) {
    ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                  "OOM / BrotliEncoderCreateInstance");
    return NGX_ERROR;
  }

  ok = BrotliEncoderSetParameter(ctx->encoder, BROTLI_PARAM_QUALITY,
                                 (uint32_t)conf->quality);
  if (!ok) {
    ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                  "BrotliEncoderSetParameter(QUALITY, %uD) failed",
                  (uint32_t)conf->quality);
    return NGX_ERROR;
  }

  ok = BrotliEncoderSetParameter(ctx->encoder, BROTLI_PARAM_LGWIN,
                                 (uint32_t)wbits);
  if (!ok) {
    ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                  "BrotliEncoderSetParameter(LGWIN, %uD) failed",
                  (uint32_t)wbits);
    return NGX_ERROR;
  }

  ctx->out_buf = ngx_calloc_buf(r->pool);
  if (ctx->out_buf == NULL) {
    return NGX_ERROR;
  }
  ctx->out_buf->temporary = 1;

  ctx->out_chain = ngx_alloc_chain_link(r->pool);
  if (ctx->out_chain == NULL) {
    return NGX_ERROR;
  }
  ctx->out_chain->buf = ctx->out_buf;
  ctx->out_chain->next = NULL;

  ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "brotli encoder initialized: lvl:%i win:%uz (derived from content_length %O)", conf->quality,
                 wbits, ctx->content_length);

  return NGX_OK;
}

static void* ngx_http_brotli_filter_alloc(void* opaque, size_t size) {
  ngx_pool_t* pool = opaque;
  void* p;

  p = ngx_palloc(pool, size);

#if (NGX_DEBUG)
  ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pool->log, 0, "brotli alloc: %p, size:%uz",
                 p, size);
#endif

  return p;
}

static void ngx_http_brotli_filter_free(void* opaque, void* address) {
  ngx_pool_t* pool = opaque;

#if (NGX_DEBUG)
  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pool->log, 0, "brotli free: %p", address);
#endif

  ngx_pfree(pool, address);
}

static void ngx_http_brotli_filter_close(ngx_http_brotli_ctx_t* ctx) {
  if (ctx->closed) {
      return;
  }
  ctx->closed = 1;
  if (ctx->encoder) {
    BrotliEncoderDestroyInstance(ctx->encoder);
    ctx->encoder = NULL;
  }
  /* Output chain and buffer are pool allocated, will be freed with the pool.
     No explicit free here unless they were allocated differently or need
     special handling beyond pool cleanup. ngx_free_chain and ngx_pfree
     are generally for objects that might need freeing before pool destruction,
     or if allocated outside the request pool.
     Given they are from r->pool, explicit free might be redundant
     but doesn't hurt if done carefully.
  */
  if (ctx->out_chain) {
    /* ngx_alloc_chain_link allocates from pool, no explicit free needed here usually */
    ctx->out_chain = NULL; /* Just nullify, pool will clean up */
  }
  if (ctx->out_buf) {
    /* ngx_calloc_buf allocates from pool */
    ctx->out_buf = NULL;
  }
}

static ngx_int_t ngx_http_brotli_check_request(ngx_http_request_t* req) {
  if (req != req->main) return NGX_DECLINED;
  if (check_accept_encoding(req) != NGX_OK) return NGX_DECLINED;
  req->gzip_tested = 1; /* Inform other modules like gzip that AE was checked */
  req->gzip_ok = 0;     /* Specifically, gzip_ok = 0 if Brotli is chosen by this check */
  return NGX_OK;
}

static ngx_int_t ngx_http_brotli_add_variables(ngx_conf_t* cf) {
  ngx_http_variable_t* var;

  var = ngx_http_add_variable(cf, &ngx_http_brotli_ratio, 0);
  if (var == NULL) {
    return NGX_ERROR;
  }

  var->get_handler = ngx_http_brotli_ratio_variable;

  return NGX_OK;
}

static ngx_int_t ngx_http_brotli_ratio_variable(ngx_http_request_t* r,
                                                ngx_http_variable_value_t* v,
                                                uintptr_t data) {
  ngx_uint_t ratio_int;
  ngx_uint_t ratio_frac;
  ngx_http_brotli_ctx_t* ctx;

  v->valid = 1;
  v->no_cacheable = 0;
  v->not_found = 0;

  ctx = ngx_http_get_module_ctx(r, ngx_http_brotli_filter_module);

  /* Only report variable on non-failing streams. */
  if (ctx == NULL || !ctx->success || ctx->bytes_out == 0) { /* Avoid division by zero */
    v->not_found = 1;
    return NGX_OK;
  }

  v->data = ngx_pnalloc(r->pool, NGX_INT32_LEN + 3);
  if (v->data == NULL) {
    return NGX_ERROR;
  }

  /* Calculate ratio: original_size / compressed_size */
  /* To avoid floating point, scale by 1000 for 3 decimal places, then round */
  uint64_t scaled_ratio = (uint64_t)ctx->bytes_in * 1000 / ctx->bytes_out;
  ratio_int = scaled_ratio / 1000;
  /* Get two decimal places for fraction */
  ratio_frac = (scaled_ratio / 10) % 100;

  /* Rounding for the second decimal place based on the third */
  if (scaled_ratio % 10 >= 5) {
    ratio_frac++;
    if (ratio_frac >= 100) {
      ratio_frac = 0;
      ratio_int++;
    }
  }

  v->len = ngx_sprintf(v->data, "%ui.%02ui", ratio_int, ratio_frac) - v->data;

  return NGX_OK;
}

static void* ngx_http_brotli_create_conf(ngx_conf_t* cf) {
  ngx_http_brotli_conf_t* conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_brotli_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  /* ngx_pcalloc fills result with zeros ->
       conf->bufs.num = 0;
       conf->types = { NULL };
       conf->types_keys = NULL; */

  conf->enable = NGX_CONF_UNSET;

  conf->quality = NGX_CONF_UNSET;
  conf->lg_win = NGX_CONF_UNSET_SIZE;
  conf->min_length = NGX_CONF_UNSET;

  return conf;
}

static char* ngx_http_brotli_merge_conf(ngx_conf_t* cf, void* parent,
                                        void* child) {
  ngx_http_brotli_conf_t* prev = parent;
  ngx_http_brotli_conf_t* conf = child;
  char* rc;

  ngx_conf_merge_value(conf->enable, prev->enable, 0);

  ngx_conf_merge_value(conf->quality, prev->quality, 6); /* Default quality 6 */
  /* Default lg_win: Brotli default is 22. Nginx default was 19 (512k).
     BrotliEncoderDEFAULT_WINDOW is 22.
     Let's align with a common default or make it explicit.
     The original code used 19 (512k).
     BROTLI_DEFAULT_WINDOW is usually 22. Max is 24. Min is 10.
  */
  ngx_conf_merge_size_value(conf->lg_win, prev->lg_win, BROTLI_DEFAULT_WINDOW);
  ngx_conf_merge_value(conf->min_length, prev->min_length, 20); /* Default min_length 20 bytes */

  rc = ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                            &prev->types_keys, &prev->types,
                            ngx_http_html_default_types);
  if (rc != NGX_CONF_OK) {
    return NGX_CONF_ERROR;
  }

  return NGX_CONF_OK;
}

/* Prepend to filter chain. */
static ngx_int_t ngx_http_brotli_filter_init(ngx_conf_t* cf) {
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_http_brotli_header_filter;

  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_http_brotli_body_filter;

  return NGX_OK;
}

/* Translate "window size" to window bits (log2), and check bounds. */
static char* ngx_http_brotli_parse_wbits(ngx_conf_t* cf, void* post,
                                         void* data) {
  size_t* parameter = data; /* This is where lg_win (which is actually size in bytes) is stored by ngx_conf_set_size_slot */
  size_t wsize_bytes = *parameter;
  size_t bits;

  /* Find the closest power of 2 for window bits that matches the size */
  for (bits = BROTLI_MIN_WINDOW_BITS; bits <= BROTLI_MAX_WINDOW_BITS; bits++) {
    if (wsize_bytes == (1u << bits)) {
      *parameter = bits; /* Store the bits value */
      return NGX_CONF_OK;
    }
  }
  
  /* If an exact power-of-2 match isn't found from the typical list */
  ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid brotli_window value \"%uz\", must be a power of 2 between 1k (for 10 bits) and 16m (for 24 bits)", wsize_bytes);
  return "must be 1k, 2k, 4k, 8k, 16k, 32k, 64k, 128k, 256k, 512k, 1m, 2m, 4m, 8m or 16m";
}
