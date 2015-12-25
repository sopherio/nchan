/*
 *  Written by Leo Ponomarev 2009-2015
 */

#include <assert.h>
#include <nchan_module.h>

#include <subscribers/longpoll.h>
#include <subscribers/intervalpoll.h>
#include <subscribers/eventsource.h>
#include <subscribers/http-chunked.h>
#include <subscribers/http-multipart-mixed.h>
#include <subscribers/websocket.h>
#include <store/memory/store.h>
#include <store/redis/store.h>
#include <nchan_setup.c>
#include <store/memory/ipc.h>
#include <store/memory/shmem.h>
//#include <store/memory/store-private.h> //for debugging
#include <nchan_output.h>
#include <nchan_websocket_publisher.h>

ngx_int_t           nchan_worker_processes;
ngx_pool_t         *nchan_pool;
ngx_module_t        nchan_module;

//#define DEBUG_LEVEL NGX_LOG_WARN
#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "NCHAN:" fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "NCHAN:" fmt, ##args)

static ngx_int_t nchan_http_publisher_handler(ngx_http_request_t * r);
static ngx_int_t channel_info_callback(ngx_int_t status, void *rptr, ngx_http_request_t *r);
static const ngx_str_t   TEXT_PLAIN = ngx_string("text/plain");

static ngx_int_t validate_id(ngx_http_request_t *r, ngx_str_t *id, nchan_loc_conf_t *cf) {
  if(id->len > (unsigned )cf->max_channel_id_length) {
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "nchan: channel id is too long: should be at most %i, is %i.", cf->max_channel_id_length, id->len);
    return NGX_ERROR;
  }
  return NGX_OK;
}

void nchan_expand_msg_id_multi_tag(nchan_msg_id_t *id, uint8_t in_n, uint8_t out_n, int16_t fill) {
  int16_t v, n = id->tagcount;
  int16_t *tags = n <= NCHAN_MULTITAG_MAX ? id->tag.fixed : id->tag.allocd;
  uint8_t i;
  assert(n > in_n && n > out_n);
  v = tags[in_n];
  
  for(i=0; i < n; i++) {
    tags[i] = (i == out_n) ? v : fill;
  }
}

ngx_int_t nchan_copy_new_msg_id(nchan_msg_id_t *dst, nchan_msg_id_t *src) {
  ngx_memcpy(dst, src, sizeof(*src));
  if(src->tagcount > NCHAN_MULTITAG_MAX) {
    size_t sz = sizeof(*src->tag.allocd) * src->tagcount;
    if((dst->tag.allocd = ngx_alloc(sz, ngx_cycle->log)) == NULL) {
      return NGX_ERROR;
    }
    ngx_memcpy(dst->tag.allocd, src->tag.allocd, sz);
  }
  return NGX_OK; 
}
ngx_int_t nchan_copy_msg_id(nchan_msg_id_t *dst, nchan_msg_id_t *src, int16_t *largetags) {
  uint16_t dst_n = dst->tagcount, src_n = src->tagcount;
  dst->time = src->time;
  
  if(dst_n > NCHAN_MULTITAG_MAX && dst_n > src_n) {
    ngx_free(dst->tag.allocd);
    dst_n = NCHAN_MULTITAG_MAX;
  }
  
  dst->tagcount = src->tagcount;
  dst->tagactive = src->tagactive;
  
  if(src_n <= NCHAN_MULTITAG_MAX) {
    dst->tag = src->tag;
  }
  else {
    if(dst_n < src_n) {
      if(!largetags) {
        if((largetags = ngx_alloc(sizeof(*largetags) * src_n, ngx_cycle->log)) == NULL) {
          return NGX_ERROR;
        }
      }
      dst->tag.allocd = largetags;
    }
    
    ngx_memcpy(dst->tag.allocd, src->tag.allocd, sizeof(*src->tag.allocd) * src_n);
  }
  return NGX_OK;
}
ngx_int_t nchan_free_msg_id(nchan_msg_id_t *id) {
  if(id->tagcount > NCHAN_MULTITAG_MAX) {
    ngx_free(id->tag.allocd);
    id->tag.allocd = NULL;
  }
  return NGX_OK;
}

static u_char *nchan_strsplit(u_char **s1, ngx_str_t *sub, u_char *last_char) {
  u_char   *delim = sub->data;
  size_t    delim_sz = sub->len;
  u_char   *last = last_char - delim_sz;
  u_char   *cur;
  
  for(cur = *s1; cur < last; cur++) {
    if(ngx_strncmp(cur, delim, delim_sz) == 0) {
      *s1 = cur + delim_sz;
      return cur;
    }
  }
  *s1 = last_char;
  if(cur == last) {
    return last_char;
  }
  else if(cur > last) {
    return NULL;
  }
  assert(0);
  return NULL;
}

static ngx_int_t nchan_process_multi_channel_id(ngx_http_request_t *r, nchan_complex_value_arr_t *idcf, nchan_loc_conf_t *cf, ngx_str_t **ret_id) {
  ngx_int_t                   i, n = idcf->n, n_out = 0;
  ngx_str_t                   id[255];
  ngx_str_t                  *id_out;
  ngx_str_t                  *group = &cf->channel_group;
  size_t                      sz = 0, grouplen = group->len;
  u_char                     *cur;
  
  //static ngx_str_t            empty_string = ngx_string("");
  
  nchan_request_ctx_t        *ctx = ngx_http_get_module_ctx(r, nchan_module);
  
  for(i=0; i < n && n_out < 255; i++) {
    ngx_http_complex_value(r, idcf->cv[i], &id[n_out]);
    if(validate_id(r, &id[n_out], cf) != NGX_OK) {
      *ret_id = NULL;
      return NGX_DECLINED;
    }
    
    if(cf->channel_id_split_delimiter.len > 0) {
      ngx_str_t  *delim = &cf->channel_id_split_delimiter;
      u_char     *cur_last, *last;
      cur = id[n_out].data;
      last = cur + id[n_out].len;
      
      u_char     *cur_first = cur;
      while ((cur_last = nchan_strsplit(&cur, delim, last)) != NULL) {
        id[n_out].data = cur_first;
        id[n_out].len = cur_last - cur_first;
        cur_first = cur;
        sz += id[n_out].len + 1 + grouplen; // "group/<channel-id>"
        if(n_out < NCHAN_MULTITAG_MAX) {
          ctx->channel_id[n_out] = id[n_out];
        }
        n_out++;
      }
      
    }
    else {
      sz += id[n_out].len + 1 + grouplen; // "group/<channel-id>"
      if(n_out < NCHAN_MULTITAG_MAX) {
        ctx->channel_id[n_out] = id[n_out];
      }
      n_out++;
    }
  }
  if(n_out>1) {
    sz += 3 + n_out; //space for null-separators and "m/<SEP>" prefix for multi-chid
  }
  if(ctx) {
    ctx->channel_id_count = n_out;
    //for(; i < NCHAN_MULTITAG_MAX; i++) {
    //  ctx->channel_id[i] = empty_string;
    //}
  }
  
  
  
  if((id_out = ngx_palloc(r->pool, sizeof(*id_out) + sz)) == NULL) {
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "nchan: can't allocate space for channel id");
    *ret_id = NULL;
    return NGX_ERROR;
  }
  id_out->len = sz;
  id_out->data = (u_char *)&id_out[1];
  cur = id_out->data;
  
  if(n_out > 1) {
    cur[0]='m';
    cur[1]='/';
    cur[2]=NCHAN_MULTI_SEP_CHR;
    cur+=3;
  }
  
  for(i = 0; i < n_out; i++) {
    ngx_memcpy(cur, group->data, grouplen);
    cur += grouplen;
    cur[0] = '/';
    cur++;
    ngx_memcpy(cur, id[i].data, id[i].len);
    cur += id[i].len;
    if(n_out>1) {
      cur[0] = NCHAN_MULTI_SEP_CHR;
      cur++;
    }
  }
  *ret_id = id_out;
  return NGX_OK;
}


ngx_int_t nchan_maybe_send_channel_event_message(ngx_http_request_t *r, channel_event_type_t event_type) {
  static nchan_loc_conf_t            evcf_data;
  static nchan_loc_conf_t           *evcf = NULL;
  
  static ngx_str_t group =           ngx_string("meta");
  
  static ngx_str_t evt_sub_enqueue = ngx_string("subscriber_enqueue");
  static ngx_str_t evt_sub_dequeue = ngx_string("subscriber_dequeue");
  static ngx_str_t evt_sub_recvmsg = ngx_string("subscriber_receive_message");
  static ngx_str_t evt_sub_recvsts = ngx_string("subscriber_receive_status");
  static ngx_str_t evt_chan_publish= ngx_string("channel_publish");
  static ngx_str_t evt_chan_delete = ngx_string("channel_delete");

  struct timeval             tv;
  
  nchan_loc_conf_t          *cf = ngx_http_get_module_loc_conf(r, nchan_module);
  ngx_http_complex_value_t  *cv = cf->channel_events_channel_id;
  if(cv==NULL) {
    //nothing to send
    return NGX_OK;
  }
  
  nchan_request_ctx_t       *ctx = ngx_http_get_module_ctx(r, nchan_module);
  ngx_str_t                  tmpid;
  size_t                     sz;
  ngx_str_t                 *id;
  u_char                    *cur;
  ngx_str_t                  evstr;
  ngx_buf_t                  buf;
  nchan_msg_t                msg;
  
  switch(event_type) {
    case SUB_ENQUEUE:
      ctx->channel_event_name = &evt_sub_enqueue;
      break;
    case SUB_DEQUEUE:
      ctx->channel_event_name = &evt_sub_dequeue;
      break;
    case SUB_RECEIVE_MESSAGE:
      ctx->channel_event_name = &evt_sub_recvmsg;
      break;
    case SUB_RECEIVE_STATUS:
      ctx->channel_event_name = &evt_sub_recvsts;
      break;
    case CHAN_PUBLISH:
      ctx->channel_event_name = &evt_chan_publish;
      break;
    case CHAN_DELETE:
      ctx->channel_event_name = &evt_chan_delete;
      break;
  }
  
  //the id
  ngx_http_complex_value(r, cv, &tmpid); 
  sz = group.len + 1 + tmpid.len;
  if((id = ngx_palloc(r->pool, sizeof(*id) + sz)) == NULL) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: can't allocate space for legacy channel id");
    return NGX_ERROR;
  }
  id->len = sz;
  id->data = (u_char *)&id[1];
  cur = id->data;  
  ngx_memcpy(cur, group.data, group.len);
  cur += group.len;
  cur[0]='/';
  cur++;
  ngx_memcpy(cur, tmpid.data, tmpid.len);
  
  
  //the event message
  ngx_http_complex_value(r, cf->channel_event_string, &evstr);
  ngx_memzero(&buf, sizeof(buf)); //do we really need this?...
  buf.temporary = 1;
  buf.memory = 1;
  buf.last_buf = 1;
  buf.pos = evstr.data;
  buf.last = evstr.data + evstr.len;
  buf.start = buf.pos;
  buf.end = buf.last;
  
  ngx_memzero(&msg, sizeof(msg));
  ngx_gettimeofday(&tv);
  msg.id.time = tv.tv_sec;
  msg.id.tagcount = 1;
  msg.buf = &buf;
  
  
  if(evcf == NULL) {
    evcf = &evcf_data;
    ngx_memzero(evcf, sizeof(*evcf));
    evcf->buffer_timeout = 10;
    evcf->max_messages = NGX_MAX_INT_T_VALUE;
    evcf->subscriber_start_at_oldest_message = 0;
    evcf->channel_timeout = 30;
  }
  evcf->storage_engine = cf->storage_engine;
  evcf->use_redis = cf->use_redis;
  
  evcf->storage_engine->publish(id, &msg, evcf, NULL, NULL);
  
  return NGX_OK;
}

static ngx_int_t nchan_process_legacy_channel_id(ngx_http_request_t *r, nchan_loc_conf_t *cf, ngx_str_t **ret_id) {
  static ngx_str_t            channel_id_var_name = ngx_string("push_channel_id");
  ngx_uint_t                  key = ngx_hash_key(channel_id_var_name.data, channel_id_var_name.len);
  ngx_http_variable_value_t  *vv = NULL;
  ngx_str_t                  *group = &cf->channel_group;
  ngx_str_t                   tmpid;
  ngx_str_t                  *id;
  size_t                      sz;
  u_char                     *cur;
  nchan_request_ctx_t        *ctx = ngx_http_get_module_ctx(r, nchan_module);
  
  ctx->channel_id_count = 0;
  
  vv = ngx_http_get_variable(r, &channel_id_var_name, key);
  if (vv == NULL || vv->not_found || vv->len == 0) {
    //ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "nchan: the legacy $push_channel_id variable is not set");
    return NGX_ABORT;
  }
  else {
    tmpid.len = vv->len;
    tmpid.data = vv->data;
  }
  
  if(validate_id(r, &tmpid, cf) != NGX_OK) {
    *ret_id = NULL;
    return NGX_DECLINED;
  }
  
  sz = group->len + 1 + tmpid.len;
  if((id = ngx_palloc(r->pool, sizeof(*id) + sz)) == NULL) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: can't allocate space for legacy channel id");
    *ret_id = NULL;
    return NGX_ERROR;
  }
  id->len = sz;
  id->data = (u_char *)&id[1];
  cur = id->data;
  
  ngx_memcpy(cur, group->data, group->len);
  cur += group->len;
  cur[0]='/';
  cur++;
  ngx_memcpy(cur, tmpid.data, tmpid.len);
  
  ctx->channel_id_count = 1;
  ctx->channel_id[0] = *id;
  
  *ret_id = id;
  return NGX_OK;
}

ngx_str_t *nchan_get_channel_id(ngx_http_request_t *r, pub_or_sub_t what, ngx_int_t fail_hard) {
  static const ngx_str_t          NO_CHANNEL_ID_MESSAGE = ngx_string("No channel id provided.");
  nchan_loc_conf_t               *cf = ngx_http_get_module_loc_conf(r, nchan_module);
  ngx_int_t                       rc;
  ngx_str_t                      *id = NULL;
  nchan_complex_value_arr_t          *chid_conf;
  
  chid_conf = what == PUB ? &cf->pub_chid : &cf->sub_chid;
  if(chid_conf->n == 0) {
    chid_conf = &cf->pubsub_chid;
  }
  
  if(chid_conf->n > 0) {
    rc = nchan_process_multi_channel_id(r, chid_conf, cf, &id);
  }
  else {
    //fallback to legacy $push_channel_id
    rc = nchan_process_legacy_channel_id(r, cf, &id);
  }
  
  if(id == NULL && fail_hard) {
    assert(rc != NGX_OK);
    switch(rc) {
      case NGX_ERROR:
        nchan_respond_status(r, NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, 0);
        break;
      
      case NGX_DECLINED:
        nchan_respond_status(r, NGX_HTTP_FORBIDDEN, NULL, 0);
        break;
      
      case NGX_ABORT:
        nchan_respond_string(r, NGX_HTTP_NOT_FOUND, &TEXT_PLAIN, &NO_CHANNEL_ID_MESSAGE, 0);
        break;
    }
    DBG("%s channel id NULL", what == PUB ? "pub" : "sub");
  }
  else {
    DBG("%s channel id %V", what == PUB ? "pub" : "sub", id);
  }
  
  return id;
}

ngx_str_t * nchan_get_header_value(ngx_http_request_t * r, ngx_str_t header_name) {
  ngx_uint_t                       i;
  ngx_list_part_t                 *part = &r->headers_in.headers.part;
  ngx_table_elt_t                 *header= part->elts;
  
  for (i = 0; /* void */ ; i++) {
    if (i >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      header = part->elts;
      i = 0;
    }
    if (header[i].key.len == header_name.len
      && ngx_strncasecmp(header[i].key.data, header_name.data, header[i].key.len) == 0) {
      return &header[i].value;
      }
  }
  return NULL;
}

ngx_str_t * nchan_subscriber_get_etag(ngx_http_request_t * r) {
  ngx_uint_t                       i;
  ngx_list_part_t                 *part = &r->headers_in.headers.part;
  ngx_table_elt_t                 *header= part->elts;
  
  for (i = 0; /* void */ ; i++) {
    if (i >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      header = part->elts;
      i = 0;
    }
    if (header[i].key.len == NCHAN_HEADER_IF_NONE_MATCH.len
      && ngx_strncasecmp(header[i].key.data, NCHAN_HEADER_IF_NONE_MATCH.data, header[i].key.len) == 0) {
      return &header[i].value;
      }
  }
  return NULL;
}

static void nchan_parse_msg_tag(u_char *first, u_char *last, nchan_msg_id_t *mid) {
  u_char           *cur = first;
  u_char            c;
  int16_t           i = 0;
  int8_t            sign = 1;
  int16_t           val = 0;
  static int16_t    tags[255];
  
  while(cur <= last && i < 255) {
    if(cur == last) {
      tags[i]=(val == 0 && sign == -1) ? -1 : val * sign; //shorthand "-" for "-1";
      i++;
      break;
    }
    
    c = *cur;
    if(c == '-') {
      sign = -1;
    }
    else if (c >= '0' && c <= '9') {
      val = 10 * val + (c - '0');
    }
    else if (c == '[') {
      mid->tagactive = i;
    }
    else if (c == ',') {
      tags[i]=(val == 0 && sign == -1) ? -1 : val * sign; //shorthand "-" for "-1"
      sign=1;
      val=0;
      i++;
    }
    cur++;
  }
  mid->tagcount = i;
  
  if(i <= NCHAN_MULTITAG_MAX) {
    ngx_memcpy(mid->tag.fixed, tags, sizeof(mid->tag.fixed));
  }
  else {
    
    mid->tag.allocd=tags;
  }
}


static ngx_int_t nchan_parse_compound_msgid(nchan_msg_id_t *id, ngx_str_t *str){
  u_char       *split, *last;
  ngx_int_t     time;
  //"<msg_time>:<msg_tag>"
  last = str->data + str->len;
  if((split = ngx_strlchr(str->data, last, ':')) != NULL) {
    time = ngx_atoi(str->data, split - str->data);
    split++;
    if(time != NGX_ERROR) {
      id->time = time;
      nchan_parse_msg_tag(split, last, id);
      return NGX_OK;
    }
    else {
      return NGX_ERROR;
    }
  }
  return NGX_DECLINED;
}


static ngx_int_t ngx_http_complex_value_noalloc(ngx_http_request_t *r, ngx_http_complex_value_t *val, ngx_str_t *value, size_t maxlen) {
  size_t                        len;
  ngx_http_script_code_pt       code;
  ngx_http_script_len_code_pt   lcode;
  ngx_http_script_engine_t      e;

  if (val->lengths == NULL) {
    *value = val->value;
    return NGX_OK;
  }

  ngx_http_script_flush_complex_value(r, val);

  ngx_memzero(&e, sizeof(ngx_http_script_engine_t));

  e.ip = val->lengths;
  e.request = r;
  e.flushed = 1;

  len = 0;

  while (*(uintptr_t *) e.ip) {
    lcode = *(ngx_http_script_len_code_pt *) e.ip;
    len += lcode(&e);
  }
  
  if(len > maxlen) {
    return NGX_ERROR;
  }
  
  value->len = len;

  e.ip = val->values;
  e.pos = value->data;
  e.buf = *value;

  while (*(uintptr_t *) e.ip) {
    code = *(ngx_http_script_code_pt *) e.ip;
    code((ngx_http_script_engine_t *) &e);
  }

  *value = e.buf;

  return NGX_OK;
}

static nchan_msg_id_t *nchan_subscriber_get_msg_id(ngx_http_request_t *r) {
  static nchan_msg_id_t           id = NCHAN_ZERO_MSGID;
  ngx_str_t                      *if_none_match;
  nchan_loc_conf_t               *cf = ngx_http_get_module_loc_conf(r, nchan_module);
  int                             i;
  
  if(!cf->msg_in_etag_only && r->headers_in.if_modified_since != NULL) {
    id.time=ngx_http_parse_time(r->headers_in.if_modified_since->value.data, r->headers_in.if_modified_since->value.len);
    if_none_match = nchan_subscriber_get_etag(r);
    
    if(if_none_match==NULL) {
      id.tagcount=1;
      id.tagactive=0;
    }
    else {
      nchan_parse_msg_tag(if_none_match->data, if_none_match->data + if_none_match->len, &id);
    }
    return &id;
  }
  else if(cf->msg_in_etag_only && (if_none_match = nchan_subscriber_get_etag(r)) != NULL) {
    if(nchan_parse_compound_msgid(&id, if_none_match) == NGX_OK) {
      return &id;
    }
  }
  else {
    nchan_complex_value_arr_t   *alt_msgid_cv_arr = &cf->last_message_id;
    u_char                       buf[128];
    ngx_str_t                    str;
    ngx_int_t                    rc;
    int                          n = alt_msgid_cv_arr->n;
    
    str.len = 0;
    str.data = buf;
    
    for(i=0; i < n; i++) {
      rc = ngx_http_complex_value_noalloc(r, alt_msgid_cv_arr->cv[i], &str, 128);
      if(str.len > 0 && rc == NGX_OK) {
        if(nchan_parse_compound_msgid(&id, &str) == NGX_OK) {
          return &id;
        }
      }
    }
  }
  
  //eh, we didn't find a valid alt_msgid value from variables. use the defaults
  id.time = cf->subscriber_start_at_oldest_message ? 0 : -1;
  id.tagcount=1;
  id.tagactive=0;
  id.tag.fixed[0] = 0;
  return &id;
}


static void nchan_match_channel_info_subtype(size_t off, u_char *cur, size_t rem, u_char **priority, const ngx_str_t **format, ngx_str_t *content_type) {
  static nchan_content_subtype_t subtypes[] = {
    { "json"  , 4, &NCHAN_CHANNEL_INFO_JSON },
    { "yaml"  , 4, &NCHAN_CHANNEL_INFO_YAML },
    { "xml"   , 3, &NCHAN_CHANNEL_INFO_XML  },
    { "x-json", 6, &NCHAN_CHANNEL_INFO_JSON },
    { "x-yaml", 6, &NCHAN_CHANNEL_INFO_YAML }
  };
  u_char                         *start = cur + off;
  ngx_uint_t                      i;
  
  for(i=0; i<(sizeof(subtypes)/sizeof(nchan_content_subtype_t)); i++) {
    if(ngx_strncmp(start, subtypes[i].subtype, rem<subtypes[i].len ? rem : subtypes[i].len)==0) {
      if(*priority>start) {
        *format = subtypes[i].format;
        *priority = start;
        content_type->data=cur;
        content_type->len= off + 1 + subtypes[i].len;
      }
    }
  }
}

ngx_buf_t                       channel_info_buf;
u_char                          channel_info_buf_str[512]; //big enough
ngx_str_t                       channel_info_content_type;
ngx_buf_t *nchan_channel_info_buf(ngx_str_t *accept_header, ngx_uint_t messages, ngx_uint_t subscribers, time_t last_seen, nchan_msg_id_t *last_msgid, ngx_str_t **generated_content_type) {
  ngx_buf_t                      *b = &channel_info_buf;
  ngx_uint_t                      len;
  const ngx_str_t                *format = &NCHAN_CHANNEL_INFO_PLAIN;
  time_t                          time_elapsed = ngx_time() - last_seen;
  static nchan_msg_id_t           zero_msgid = NCHAN_ZERO_MSGID;
  if(!last_msgid) {
    last_msgid = &zero_msgid;
  }
 
  ngx_memcpy(&channel_info_content_type, &TEXT_PLAIN, sizeof(TEXT_PLAIN));;
  
  b->start = channel_info_buf_str;
  b->pos = b->start;
  b->last_buf = 1;
  b->last_in_chain = 1;
  b->flush = 1;
  b->memory = 1;
  
  if(accept_header) {
    //lame content-negotiation (without regard for qvalues)
    u_char                    *accept = accept_header->data;
    len = accept_header->len;
    size_t                     rem;
    u_char                    *cur = accept;
    u_char                    *priority=&accept[len-1];
    
    for(rem=len; (cur = ngx_strnstr(cur, "text/", rem))!=NULL; cur += sizeof("text/")-1) {
      rem=len - ((size_t)(cur-accept)+sizeof("text/")-1);
      if(ngx_strncmp(cur+sizeof("text/")-1, "plain", rem<5 ? rem : 5)==0) {
        if(priority) {
          format = &NCHAN_CHANNEL_INFO_PLAIN;
          priority = cur+sizeof("text/")-1;
          //content-type is already set by default
        }
      }
      nchan_match_channel_info_subtype(sizeof("text/")-1, cur, rem, &priority, &format, &channel_info_content_type);
    }
    cur = accept;
    for(rem=len; (cur = ngx_strnstr(cur, "application/", rem))!=NULL; cur += sizeof("application/")-1) {
      rem=len - ((size_t)(cur-accept)+sizeof("application/")-1);
      nchan_match_channel_info_subtype(sizeof("application/")-1, cur, rem, &priority, &format, &channel_info_content_type);
    }
  }
  
  if(generated_content_type) {
    *generated_content_type = &channel_info_content_type;
  }
  
  len = format->len - 8 - 1 + 3*NGX_INT_T_LEN; //minus 8 sprintf
  
  if(len > 512) {
    ERR("Channel info string too long: max: 512, is: %i",len);
    len = 512;
  }
  
  b->last = ngx_sprintf(b->start, (char *)format->data, messages, last_seen==0 ? -1 : (ngx_int_t) time_elapsed, subscribers, msgid_to_str(last_msgid));
  b->end = b->last;
  
  return b;
}

//print information about a channel
static ngx_int_t nchan_channel_info(ngx_http_request_t *r, ngx_uint_t messages, ngx_uint_t subscribers, time_t last_seen, nchan_msg_id_t *msgid) {
  ngx_buf_t                      *b;
  ngx_str_t                      *content_type;
  ngx_str_t                      *accept_header = NULL;
  
  if(r->headers_in.accept) {
    accept_header = &r->headers_in.accept->value;
  }
  
  b = nchan_channel_info_buf(accept_header, messages, subscribers, last_seen, msgid, &content_type);
  
  //not sure why this is needed, but content-type directly from the request can't be reliably used in the response 
  //(it probably can, but i'm just doing it wrong)
  /*if(content_type != &TEXT_PLAIN) {
    ERR("WTF why must i do this %p %V", content_type, content_type);
    content_type_copy.len = content_type->len;
    content_type_copy.data = ngx_palloc(r->pool, content_type_copy.len);
    assert(content_type_copy.data);
    ngx_memcpy(content_type_copy.data, content_type->data, content_type_copy.len);
    content_type = &content_type_copy;
  }*/
  
  return nchan_respond_membuf(r, NGX_HTTP_OK, content_type, b, 0);
}

// this function adapted from push stream module. thanks Wandenberg Peixoto <wandenberg@gmail.com> and Rogério Carvalho Schneider <stockrt@gmail.com>
static ngx_buf_t * nchan_request_body_to_single_buffer(ngx_http_request_t *r) {
  ngx_buf_t *buf = NULL;
  ngx_chain_t *chain;
  ssize_t n;
  off_t len;

  chain = r->request_body->bufs;
  if (chain->next == NULL) {
    return chain->buf;
  }
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "nchan: multiple buffers in request, need memcpy :(");
  if (chain->buf->in_file) {
    if (ngx_buf_in_memory(chain->buf)) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: can't handle a buffer in a temp file and in memory ");
    }
    if (chain->next != NULL) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: error reading request body with multiple ");
    }
    return chain->buf;
  }
  buf = ngx_create_temp_buf(r->pool, r->headers_in.content_length_n + 1);
  if (buf != NULL) {
    ngx_memset(buf->start, '\0', r->headers_in.content_length_n + 1);
    while ((chain != NULL) && (chain->buf != NULL)) {
      len = ngx_buf_size(chain->buf);
      // if buffer is equal to content length all the content is in this buffer
      if (len >= r->headers_in.content_length_n) {
        buf->start = buf->pos;
        buf->last = buf->pos;
        len = r->headers_in.content_length_n;
      }
      if (chain->buf->in_file) {
        n = ngx_read_file(chain->buf->file, buf->start, len, 0);
        if (n == NGX_FILE_ERROR) {
          ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: cannot read file with request body");
          return NULL;
        }
        buf->last = buf->last + len;
        ngx_delete_file(chain->buf->file->name.data);
        chain->buf->file->fd = NGX_INVALID_FILE;
      } else {
        buf->last = ngx_copy(buf->start, chain->buf->pos, len);
      }

      chain = chain->next;
      buf->start = buf->last;
    }
    buf->last_buf = 1;
  }
  return buf;
}

static ngx_int_t nchan_response_channel_ptr_info(nchan_channel_t *channel, ngx_http_request_t *r, ngx_int_t status_code) {
  static const ngx_str_t CREATED_LINE = ngx_string("201 Created");
  static const ngx_str_t ACCEPTED_LINE = ngx_string("202 Accepted");
  
  time_t             last_seen = 0;
  ngx_uint_t         subscribers = 0;
  ngx_uint_t         messages = 0;
  nchan_msg_id_t    *msgid = NULL;
  if(channel!=NULL) {
    subscribers = channel->subscribers;
    last_seen = channel->last_seen;
    messages  = channel->messages;
    msgid = &channel->last_published_msg_id;
    r->headers_out.status = status_code == (ngx_int_t) NULL ? NGX_HTTP_OK : status_code;
    if (status_code == NGX_HTTP_CREATED) {
      ngx_memcpy(&r->headers_out.status_line, &CREATED_LINE, sizeof(ngx_str_t));
    }
    else if (status_code == NGX_HTTP_ACCEPTED) {
      ngx_memcpy(&r->headers_out.status_line, &ACCEPTED_LINE, sizeof(ngx_str_t));
    }
    nchan_channel_info(r, messages, subscribers, last_seen, msgid);
  }
  else {
    //404!
    nchan_respond_status(r, NGX_HTTP_NOT_FOUND, NULL, 0);
  }
  return NGX_OK;
}

static void memstore_sub_debug_start() {
#if FAKESHARD  
  #ifdef SUB_FAKE_WORKER
  memstore_fakeprocess_push(SUB_FAKE_WORKER);
  #else
  memstore_fakeprocess_push_random();
  #endif
#endif   
}
static void memstore_sub_debug_end() {
#if FAKESHARD
  memstore_fakeprocess_pop();
#endif
}

static void memstore_pub_debug_start() {
#if FAKESHARD
  #ifdef PUB_FAKE_WORKER
  memstore_fakeprocess_push(PUB_FAKE_WORKER);
  #else
  memstore_fakeprocess_push_random();
  #endif
#endif
}
static void memstore_pub_debug_end() {
#if FAKESHARD
  memstore_fakeprocess_pop();
#endif
}

ngx_int_t nchan_pubsub_handler(ngx_http_request_t *r) {
  nchan_loc_conf_t       *cf = ngx_http_get_module_loc_conf(r, nchan_module);
  ngx_str_t              *channel_id;
  subscriber_t           *sub;
  nchan_msg_id_t         *msg_id;
  ngx_int_t               rc = NGX_DONE;
  nchan_request_ctx_t    *ctx;
  ngx_str_t              *origin_header;
  
#if NCHAN_BENCHMARK
  struct timeval          tv;
  ngx_gettimeofday(&tv);
#endif
  
  if((ctx = ngx_pcalloc(r->pool, sizeof(nchan_request_ctx_t))) == NULL) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  ngx_http_set_ctx(r, ctx, nchan_module);

#if NCHAN_BENCHMARK
  ctx->start_tv = tv;
#endif
  
  if((origin_header = nchan_get_header_value(r, NCHAN_HEADER_ORIGIN)) != NULL) {
    ctx->request_origin_header = *origin_header;
    if(!(cf->allow_origin.len == 1 && cf->allow_origin.data[0] == '*')) {
      if(!(origin_header->len == cf->allow_origin.len && ngx_strnstr(origin_header->data, (char *)cf->allow_origin.data, origin_header->len) != NULL)) {
        //CORS origin match failed! return a 403 forbidden
        goto forbidden;
      }
    }
  }
  else {
    ctx->request_origin_header.len=0;
    ctx->request_origin_header.data=NULL;
  }
  
  if((channel_id = nchan_get_channel_id(r, SUB, 1)) == NULL) {
    //just get the subscriber_channel_id for now. the publisher one is handled elsewhere
    return r->headers_out.status ? NGX_OK : NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  if(nchan_detect_websocket_request(r)) {
    //want websocket?
    if(cf->sub.websocket) {
      //we prefer to subscribe
      memstore_sub_debug_start();
      msg_id = nchan_subscriber_get_msg_id(r);
      if((sub = websocket_subscriber_create(r, msg_id)) == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "unable to create websocket subscriber");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
      }
      sub->fn->subscribe(sub, channel_id);
      
      memstore_sub_debug_end();
    }
    else if(cf->pub.websocket) {
      //no need to subscribe, but keep a connection open for publishing
      //not yet implemented
      nchan_create_websocket_publisher(r);
    }
    else goto forbidden;
    return NGX_DONE;
  }
  else {
    subscriber_t *(*sub_create)(ngx_http_request_t *r, nchan_msg_id_t *msg_id) = NULL;
    
    switch(r->method) {
      case NGX_HTTP_GET:
        if(cf->sub.eventsource && nchan_detect_eventsource_request(r)) {
          sub_create = eventsource_subscriber_create;
        }
        else if(cf->sub.http_chunked && nchan_detect_chunked_subscriber_request(r)) {
          sub_create = http_chunked_subscriber_create;
        }
        else if(cf->sub.http_multipart && nchan_detect_multipart_subscriber_request(r)) {
          sub_create = http_multipart_subscriber_create;
        }
        else if(cf->sub.poll) {
          sub_create = intervalpoll_subscriber_create;
        }
        else if(cf->sub.longpoll) {
          sub_create = longpoll_subscriber_create;
        }
        else if(cf->pub.http) {
          nchan_http_publisher_handler(r);
        }
        else {
          goto forbidden;
        }
        
        if(sub_create) {
          memstore_sub_debug_start();
          
          msg_id = nchan_subscriber_get_msg_id(r);
          if((sub = sub_create(r, msg_id)) == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "unable to create subscriber");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
          }
          
          sub->fn->subscribe(sub, channel_id);
          
          memstore_sub_debug_end();
        }
        
        break;
      
      case NGX_HTTP_POST:
      case NGX_HTTP_PUT:
        if(cf->pub.http) {
          nchan_http_publisher_handler(r);
        }
        else goto forbidden;
        break;
      
      case NGX_HTTP_DELETE:
        if(cf->pub.http) {
          nchan_http_publisher_handler(r);
        }
        else goto forbidden;
        break;
      
      case NGX_HTTP_OPTIONS:
        if(cf->pub.http) {
          nchan_OPTIONS_respond(r, &cf->allow_origin, &NCHAN_ACCESS_CONTROL_ALLOWED_PUBLISHER_HEADERS, &NCHAN_ALLOW_GET_POST_PUT_DELETE_OPTIONS);
        }
        else if(cf->sub.poll || cf->sub.longpoll || cf->sub.eventsource || cf->sub.websocket) {
          nchan_OPTIONS_respond(r, &cf->allow_origin, &NCHAN_ACCESS_CONTROL_ALLOWED_SUBSCRIBER_HEADERS, &NCHAN_ALLOW_GET_OPTIONS);
        }
        else goto forbidden;
        break;
    }
  }
  
  return rc;
  
forbidden:
  nchan_respond_status(r, NGX_HTTP_FORBIDDEN, NULL, 0);
  return NGX_OK;
}

static ngx_int_t channel_info_callback(ngx_int_t status, void *rptr, ngx_http_request_t *r) {
  ngx_http_finalize_request(r, nchan_response_channel_ptr_info( (nchan_channel_t *)rptr, r, 0));
  return NGX_OK;
}

static ngx_int_t publish_callback(ngx_int_t status, void *rptr, ngx_http_request_t *r) {
  nchan_channel_t       *ch = rptr;
  nchan_request_ctx_t   *ctx = ngx_http_get_module_ctx(r, nchan_module);
  static nchan_msg_id_t  empty_msgid = NCHAN_ZERO_MSGID;
  //DBG("publish_callback %V owner %i status %i", ch_id, memstore_channel_owner(ch_id), status);
  switch(status) {
    case NCHAN_MESSAGE_QUEUED:
      //message was queued successfully, but there were no subscribers to receive it.
      ctx->prev_msg_id = ctx->msg_id;
      ctx->msg_id = ch != NULL ? ch->last_published_msg_id : empty_msgid;
      
      nchan_maybe_send_channel_event_message(r, CHAN_PUBLISH);
      ngx_http_finalize_request(r, nchan_response_channel_ptr_info(ch, r, NGX_HTTP_ACCEPTED));
      return NGX_OK;
      
    case NCHAN_MESSAGE_RECEIVED:
      //message was queued successfully, and it was already sent to at least one subscriber
      ctx->prev_msg_id = ctx->msg_id;
      ctx->msg_id = ch != NULL ? ch->last_published_msg_id : empty_msgid;
      
      nchan_maybe_send_channel_event_message(r, CHAN_PUBLISH);
      ngx_http_finalize_request(r, nchan_response_channel_ptr_info(ch, r, NGX_HTTP_CREATED));
      return NGX_OK;
      
    case NGX_ERROR:
    case NGX_HTTP_INTERNAL_SERVER_ERROR:
      //WTF?
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: error publishing message");
      ctx->prev_msg_id = empty_msgid;;
      ctx->msg_id = empty_msgid;
      ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
      return NGX_ERROR;
      
    default:
      //for debugging, mostly. I don't expect this branch to behit during regular operation
      ctx->prev_msg_id = empty_msgid;;
      ctx->msg_id = empty_msgid;
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: TOTALLY UNEXPECTED error publishing message, status code %i", status);
      ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
      return NGX_ERROR;
  }
}

#define NGX_REQUEST_VAL_CHECK(val, fail, r, errormessage)                 \
if (val == fail) {                                                        \
  ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, errormessage);      \
  ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);           \
  return;                                                                 \
  }

static void nchan_publisher_body_handler_continued(ngx_http_request_t *r, ngx_str_t *channel_id, nchan_loc_conf_t *cf) {
  ngx_buf_t                      *buf;
  size_t                          content_type_len;
  nchan_msg_t                    *msg;
  struct timeval                  tv;
  
  switch(r->method) {
    case NGX_HTTP_GET:
      cf->storage_engine->find_channel(channel_id, (callback_pt) &channel_info_callback, (void *)r);
      break;
    
    case NGX_HTTP_PUT:
    case NGX_HTTP_POST:
      memstore_pub_debug_start();
      
      msg = ngx_pcalloc(r->pool, sizeof(*msg));
      msg->shared = 0;
      NGX_REQUEST_VAL_CHECK(msg, NULL, r, "nchan: can't allocate msg in request pool");
      //buf = ngx_create_temp_buf(r->pool, 0);
      //NGX_REQUEST_VAL_CHECK(buf, NULL, r, "nchan: can't allocate buf in request pool");
      
      //content type
      content_type_len = (r->headers_in.content_type!=NULL ? r->headers_in.content_type->value.len : 0);
      if(content_type_len > 0) {
        msg->content_type.len = content_type_len;
        msg->content_type.data = r->headers_in.content_type->value.data;
      }
      
      if(r->headers_in.content_length_n == -1 || r->headers_in.content_length_n == 0) {
        buf = ngx_create_temp_buf(r->pool, 0);
      }
      else if(r->request_body->bufs!=NULL) {
        buf = nchan_request_body_to_single_buffer(r);
      }
      else {
        ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, "nchan: unexpected publisher message request body buffer location. please report this to the nchan developers.");
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
      }
      
      ngx_gettimeofday(&tv);
      msg->id.time = tv.tv_sec;
      msg->id.tag.fixed[0] = 0;
      msg->id.tagactive = 0;
      msg->id.tagcount = 1;
      
      msg->buf = buf;
#if NCHAN_MSG_LEAK_DEBUG
      msg->lbl = r->uri;
#endif
#if NCHAN_BENCHMARK
      nchan_request_ctx_t            *ctx = ngx_http_get_module_ctx(r, nchan_module);
      msg->start_tv = ctx->start_tv;
#endif
      
      cf->storage_engine->publish(channel_id, msg, cf, (callback_pt) &publish_callback, r);
      
      memstore_pub_debug_end();
      break;
      
    case NGX_HTTP_DELETE:
      cf->storage_engine->delete_channel(channel_id, (callback_pt) &channel_info_callback, (void *)r);
      nchan_maybe_send_channel_event_message(r, CHAN_DELETE);
      break;
      
    default: 
      nchan_respond_status(r, NGX_HTTP_FORBIDDEN, NULL, 0);
  }
  
}

typedef struct {
  ngx_str_t       *ch_id;
} nchan_pub_subrequest_data_t;

typedef struct {
  ngx_http_post_subrequest_t    psr;
  nchan_pub_subrequest_data_t   psr_data;
} nchan_pub_subrequest_stuff_t;


static ngx_int_t nchan_publisher_body_authorize_handler(ngx_http_request_t *r, void *data, ngx_int_t rc) {
  nchan_pub_subrequest_data_t  *d = data;
  
  if(rc == NGX_OK) {
    nchan_loc_conf_t    *cf = ngx_http_get_module_loc_conf(r->main, nchan_module);
    ngx_int_t            code = r->headers_out.status;
    if(code >= 200 && code <299) {
      //authorized. proceed as planned
      nchan_publisher_body_handler_continued(r->main, d->ch_id, cf);
    }
    else { //anything else means forbidden
      ngx_http_finalize_request(r->main, NGX_HTTP_FORBIDDEN);
    }
  }
  else {
    ngx_http_finalize_request(r->main, NGX_HTTP_INTERNAL_SERVER_ERROR);
  }
  return NGX_OK;
}

static void nchan_publisher_body_handler(ngx_http_request_t *r) {
  ngx_str_t                      *channel_id;
  nchan_loc_conf_t               *cf = ngx_http_get_module_loc_conf(r, nchan_module);

  ngx_http_complex_value_t       *authorize_request_url_ccv = cf->authorize_request_url;
  
  if((channel_id = nchan_get_channel_id(r, PUB, 1))==NULL) {
    ngx_http_finalize_request(r, r->headers_out.status ? NGX_OK : NGX_HTTP_INTERNAL_SERVER_ERROR);
    return;
  }
  
  if(!authorize_request_url_ccv) {
    nchan_publisher_body_handler_continued(r, channel_id, cf);
  }
  else {
    nchan_pub_subrequest_stuff_t   *psr_stuff;
    
    if((psr_stuff = ngx_palloc(r->pool, sizeof(*psr_stuff))) == NULL) {
      ERR("can't allocate memory for publisher auth subrequest");
      ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
      return;
    }
    
    ngx_http_post_subrequest_t    *psr = &psr_stuff->psr;
    nchan_pub_subrequest_data_t   *psrd = &psr_stuff->psr_data;
    ngx_http_request_t            *sr;
    ngx_str_t                      auth_request_url;
    
    ngx_http_complex_value(r, authorize_request_url_ccv, &auth_request_url);
    
    psr->handler = nchan_publisher_body_authorize_handler;
    psr->data = psrd;
    
    psrd->ch_id = channel_id;
    
    ngx_http_subrequest(r, &auth_request_url, NULL, &sr, psr, 0);
    if((sr->request_body = ngx_pcalloc(r->pool, sizeof(ngx_http_request_body_t))) == NULL) {
      ERR("can't allocate memory for publisher auth subrequest body");
      ngx_http_finalize_request(r, r->headers_out.status ? NGX_OK : NGX_HTTP_INTERNAL_SERVER_ERROR);
      return;
    }
    sr->header_only = 1;
  }
}


static ngx_int_t nchan_http_publisher_handler(ngx_http_request_t * r) {
  ngx_int_t                       rc;
  nchan_request_ctx_t            *ctx = ngx_http_get_module_ctx(r, nchan_module);
  
  static ngx_str_t                publisher_name = ngx_string("http");
  
  if(ctx) ctx->publisher_type = &publisher_name;
  
  /* Instruct ngx_http_read_subscriber_request_body to store the request
     body entirely in a memory buffer or in a file */
  r->request_body_in_single_buf = 1;
  r->request_body_in_persistent_file = 1;
  r->request_body_in_clean_file = 0;
  r->request_body_file_log_level = 0;
  
  //don't buffer the request body --send it right on through
  //r->request_body_no_buffering = 1;

  rc = ngx_http_read_client_request_body(r, nchan_publisher_body_handler);
  if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
    return rc;
  }
  return NGX_DONE;
}

#if NCHAN_BENCHMARK
int nchan_timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y) {
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y->tv_usec) {
    int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    y->tv_usec += 1000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}
#endif

static ngx_int_t verify_msg_id(nchan_msg_id_t *id1, nchan_msg_id_t *id2, nchan_msg_id_t *msgid) {
  int16_t  *tags1 = id1->tagcount <= NCHAN_MULTITAG_MAX ? id1->tag.fixed : id1->tag.allocd;
  int16_t  *tags2 = id2->tagcount <= NCHAN_MULTITAG_MAX ? id2->tag.fixed : id2->tag.allocd;
  if(id1->time > 0 && id2->time > 0) {
    if(id1->time != id2->time) {
      //is this a missed message, or just a multi msg?
      
      if(id2->tagcount > 1) {
        int       i = -1, j, max = id2->tagcount;  
        int16_t  *msgidtags = msgid->tagcount <= NCHAN_MULTITAG_MAX ? msgid->tag.fixed : msgid->tag.allocd;
        
        for(j=0; j < max; j++) {
          if(tags2[j] != -1) {
            if( i != -1) {
              ERR("verify_msg_id: more than one tag set to something besides -1. that means this isn't a single channel's forwarded multi msg. fail.");
              return NGX_ERROR;
            }
            else {
              i = j;
            }
          }
        }
        if(msgidtags[i] != 0) {
          ERR("verify_msg_id: only the first message in a given second is ok. anything else means a missed message.");
          return NGX_ERROR;
        }
        //ok, it's just the first-per-second message of a channel from a multi-channel
        //this is a rather convoluted description... but basically this is ok.
        return NGX_OK;
      }
      else {
        ERR("verify_msg_id: not a multimsg tag, different times. could be a missed message.");
        return NGX_ERROR;
      }
    }
    
    if(id1->tagcount == 1) {
      if(tags1[0] != tags2[0]){
        ERR("verify_msg_id: tag mismatch. missed message?");
        return NGX_ERROR;
      }
    }
    else {
      int   i, max = id1->tagcount;
      for(i=0; i < max; i++) {
        if(tags2[i] != -1 && tags1[i] != tags2[i]) {
          ERR("verify_msg_id: multitag mismatch. missed message?");
          return NGX_ERROR;
        }
      }
    }
  }
  return NGX_OK;
}

void nchan_update_multi_msgid(nchan_msg_id_t *oldid, nchan_msg_id_t *newid) {
  if(newid->tagcount == 1) {
    //nice and simple
    *oldid = *newid;
  }
  else {
    //DBG("======= updating multi_msgid ======");
    //DBG("======= old: %V", msgid_to_str(oldid));
    //DBG("======= new: %V", msgid_to_str(newid));
    if(newid->tagcount > NCHAN_MULTITAG_MAX && oldid->tagcount < newid->tagcount) {
      int16_t       *oldtags, *old_largetags = NULL;
      int            i;
      size_t         sz = sizeof(*oldid->tag.allocd) * newid->tagcount;
      if(oldid->tagcount > NCHAN_MULTITAG_MAX) {
        old_largetags = oldid->tag.allocd;
        oldtags = old_largetags;
      }
      else {
        oldtags = oldid->tag.fixed;
      }
      oldid->tag.allocd = ngx_alloc(sz, ngx_cycle->log);
      for(i=0; i < newid->tagcount; i++) {
        oldid->tag.allocd[i] = (i < oldid->tagcount) ? oldtags[i] : -1;
      }
      if(old_largetags) {
        ngx_free(old_largetags);
      }
    }
    
    if(oldid->time != newid->time) {
      nchan_copy_msg_id(oldid, newid, NULL);
    }
    else {
      int i, max = newid->tagcount;
      int16_t  *oldtags = oldid->tagcount <= NCHAN_MULTITAG_MAX ? oldid->tag.fixed : oldid->tag.allocd;
      int16_t  *newtags = newid->tagcount <= NCHAN_MULTITAG_MAX ? newid->tag.fixed : newid->tag.allocd;
      
      assert(max == oldid->tagcount);
      
      for(i=0; i< max; i++) {
        
        //DEBUG CHECK -- REMOVE BEFORE RELEASE
        if(newid->tagactive == i && newtags[i] != -1 && oldtags[i] != -1) {
          assert(newtags[i] > oldtags[i]);
        }
        
        
        if (newtags[i] != -1) {
          oldtags[i] = newtags[i];
        }
      }
      oldid->tagactive = newid->tagactive;
    }
    //DBG("=== updated: %V", msgid_to_str(oldid));
  }
}

ngx_int_t update_subscriber_last_msg_id(subscriber_t *sub, nchan_msg_t *msg) {
  if(msg) {
    if(verify_msg_id(&sub->last_msgid, &msg->prev_id, &msg->id) == NGX_ERROR) {
      struct timeval    tv;
      time_t            time;
      int               ttl = msg->expires - msg->id.time;
      ngx_gettimeofday(&tv);
      time = tv.tv_sec;
      
      if(sub->last_msgid.time + ttl <= time) {
        ERR("missed a message because it probably expired");
      }
      else {
        ERR("missed a message for an unknown reason. Maybe it's a bug or maybe the message queue length is too small.");
      }
    }
    
    nchan_update_multi_msgid(&sub->last_msgid, &msg->id);
  }
  
  return NGX_OK;
}



#if NCHAN_SUBSCRIBER_LEAK_DEBUG

subscriber_t *subdebug_head = NULL;

void subscriber_debug_add(subscriber_t *sub) {
  if(subdebug_head == NULL) {
    sub->dbg_next = NULL;
    sub->dbg_prev = NULL;
  }
  else {
    sub->dbg_next = subdebug_head;
    sub->dbg_prev = NULL;
    assert(subdebug_head->dbg_prev == NULL);
    subdebug_head->dbg_prev = sub;
  }
  subdebug_head = sub;
}
void subscriber_debug_remove(subscriber_t *sub) {
  subscriber_t *prev, *next;
  prev = sub->dbg_prev;
  next = sub->dbg_next;
  if(subdebug_head == sub) {
    assert(sub->dbg_prev == NULL);
    if(next) {
      next->dbg_prev = NULL;
    }
    subdebug_head = next;
  }
  else {
    if(prev) {
      prev->dbg_next = next;
    }
    if(next) {
      next->dbg_prev = prev;
    }
  }
  
  sub->dbg_next = NULL;
  sub->dbg_prev = NULL;
}
void subscriber_debug_assert_isempty(void) {
  assert(subdebug_head == NULL);
}
#endif
