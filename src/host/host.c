/// @file
///
/// The libuv host: see mesa/host.h.

#include "compat.h"
#include "mesa/host.h"
#include "mesa/azimuth.h"
#include "mesa/ident.h"

#include <curl/curl.h>
#include <sys/time.h>
#include <uv.h>

#define HOST_DNS_MS   (5 * 60 * 1000)   //  refresh galaxy addresses

typedef struct _host_need {
  mesa_ship          who;
  struct _host_need* nex_u;
} _host_need;

typedef struct _host_work {
  uv_work_t   wok_u;
  mesa_host*  hos_u;
  mesa_ship   who;
  mesa_point  pot_u;
  c3_i        ret_i;
} _host_work;

typedef struct _host_dns {
  uv_getaddrinfo_t gai_u;
  mesa_host*       hos_u;
  c3_y             gal_y;
} _host_dns;

struct mesa_host {
  mesa_hooks      app_u;      //  the application's hooks
  mesa_engine*    eng_u;
  uv_loop_t*      lup_u;
  uv_udp_t        wax_u;
  uv_timer_t      tim_u;
  uv_idle_t       idl_u;
  c3_o            fak_o;
  c3_c*           azi_c;
  c3_c*           dns_c;
  _host_need*     ned_u;
  //  galaxy addresses
  c3_h            gip_h[256];  //  host order, 0: unknown
  c3_d            gat_d[256];  //  when resolved (ms)
  c3_o            gaq_o[256];  //  lookup in flight
};

static c3_d
_now(void)
{
  struct timeval tim_u;
  gettimeofday(&tim_u, 0);
  return ((c3_d)tim_u.tv_sec * 1000) + ((c3_d)tim_u.tv_usec / 1000);
}

/* timers
*/
static void
_host_wake_cb(uv_timer_t* tim_u)
{
  mesa_host* hos_u = tim_u->data;
  mesa_engine_wake(hos_u->eng_u, _now());
  mesa_host_kick(hos_u);
}

void
mesa_host_kick(mesa_host* hos_u)
{
  c3_d dea_d = mesa_engine_deadline(hos_u->eng_u);
  c3_d now_d = _now();

  if ( UINT64_MAX == dea_d ) {
    uv_timer_stop(&hos_u->tim_u);
    return;
  }
  uv_timer_start(&hos_u->tim_u, _host_wake_cb,
                 ( dea_d <= now_d ) ? 0 : (dea_d - now_d), 0);
}

/* galaxy addresses
*/
static void
_host_dns_cb(uv_getaddrinfo_t* gai_u, c3_i sas_i, struct addrinfo* res_u)
{
  _host_dns* dns_u = gai_u->data;
  mesa_host* hos_u = dns_u->hos_u;

  hos_u->gaq_o[dns_u->gal_y] = c3n;

  if ( !sas_i && res_u && (AF_INET == res_u->ai_family) ) {
    struct sockaddr_in* add_u = (struct sockaddr_in*)res_u->ai_addr;
    hos_u->gip_h[dns_u->gal_y] = ntohl(add_u->sin_addr.s_addr);
    hos_u->gat_d[dns_u->gal_y] = _now();
  }
  else {
    mesa_log("mesa: can't resolve galaxy %u", dns_u->gal_y);
  }
  uv_freeaddrinfo(res_u);
  c3_free(dns_u);
}

static void
_host_resolve(mesa_host* hos_u, c3_y gal_y)
{
  _host_dns*      dns_u;
  struct addrinfo hin_u = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
  c3_c            nam_c[256];
  mesa_ship       who = { .lo = gal_y, .hi = 0 };
  c3_c*           gal_c;

  if ( c3y == hos_u->gaq_o[gal_y] ) {
    return;
  }
  hos_u->gaq_o[gal_y] = c3y;

  gal_c = mesa_ship_render(who);
  snprintf(nam_c, sizeof(nam_c), "%s.%s", gal_c + 1, hos_u->dns_c);
  c3_free(gal_c);

  dns_u = c3_calloc(sizeof(*dns_u));
  dns_u->hos_u = hos_u;
  dns_u->gal_y = gal_y;
  dns_u->gai_u.data = dns_u;

  if ( uv_getaddrinfo(hos_u->lup_u, &dns_u->gai_u, _host_dns_cb,
                      nam_c, 0, &hin_u) )
  {
    hos_u->gaq_o[gal_y] = c3n;
    c3_free(dns_u);
  }
}

/* sending
*/
static void
_host_send(void* ctx, mesa_lane lan_u, const uint8_t* buf_y, size_t len_z)
{
  mesa_host*         hos_u = ctx;
  struct sockaddr_in add_u = { .sin_family = AF_INET };
  uv_buf_t           buf_u = uv_buf_init((c3_c*)buf_y, (unsigned)len_z);

  if ( MESA_LANE_GALAXY == lan_u.kind ) {
    if ( c3y == hos_u->fak_o ) {
      add_u.sin_addr.s_addr = htonl(0x7f000001);
      add_u.sin_port = htons(31337 + lan_u.galaxy);
    }
    else {
      c3_y gal_y = lan_u.galaxy;

      if ( !hos_u->gip_h[gal_y] || (_now() > hos_u->gat_d[gal_y] + HOST_DNS_MS) ) {
        _host_resolve(hos_u, gal_y);
      }
      if ( !hos_u->gip_h[gal_y] ) {
        return;   //  the engine will retry once we know
      }
      add_u.sin_addr.s_addr = htonl(hos_u->gip_h[gal_y]);
      add_u.sin_port = htons(13337 + gal_y);
    }
  }
  else if ( MESA_LANE_IP == lan_u.kind ) {
    add_u.sin_addr.s_addr = htonl(( c3y == hos_u->fak_o ) ? 0x7f000001 : lan_u.ip);
    add_u.sin_port = htons(lan_u.port);
    if ( !lan_u.ip || (0xffffffff == lan_u.ip) || !lan_u.port ) {
      return;
    }
  }
  else {
    return;
  }

  //  datagrams are small and the engine retransmits; never block
  //
  uv_udp_try_send(&hos_u->wax_u, &buf_u, 1, (const struct sockaddr*)&add_u);
}

static void
_host_alloc_cb(uv_handle_t* han_u, size_t len_z, uv_buf_t* buf_u)
{
  static c3_c buf_c[2048];
  *buf_u = uv_buf_init(buf_c, sizeof(buf_c));
}

static void
_host_recv_cb(uv_udp_t* wax_u, ssize_t nrd_i, const uv_buf_t* buf_u,
              const struct sockaddr* add_u, unsigned flg_i)
{
  mesa_host* hos_u = wax_u->data;

  if ( (nrd_i > 0) && add_u && !(flg_i & UV_UDP_PARTIAL)
     && (AF_INET == add_u->sa_family) )
  {
    const struct sockaddr_in* sin_u = (const struct sockaddr_in*)add_u;
    mesa_lane lan_u = { .kind = MESA_LANE_IP,
                        .ip   = ntohl(sin_u->sin_addr.s_addr),
                        .port = ntohs(sin_u->sin_port) };

    mesa_engine_hear(hos_u->eng_u, lan_u, (const uint8_t*)buf_u->base,
                     (size_t)nrd_i, _now());
    mesa_host_kick(hos_u);
  }
}

/* keys
*/
static size_t
_curl_write(void* ptr, size_t siz, size_t nmb, void* arg)
{
  uv_buf_t* buf_u = arg;
  size_t    len_z = siz * nmb;

  if ( buf_u->len + len_z > (1u << 20) ) {
    return 0;
  }
  buf_u->base = c3_realloc(buf_u->base, buf_u->len + len_z + 1);
  memcpy(buf_u->base + buf_u->len, ptr, len_z);
  buf_u->len += len_z;
  buf_u->base[buf_u->len] = 0;
  return len_z;
}

mesa_azimuth_err
mesa_azimuth_fetch(const char* url, mesa_ship who, mesa_point* out)
{
  CURL*              cul_u = curl_easy_init();
  struct curl_slist* hed_u = 0;
  uv_buf_t           buf_u = { .base = 0, .len = 0 };
  c3_c*              req_c = mesa_azimuth_point_request(who);
  mesa_azimuth_err   ret_e = MESA_AZIMUTH_BAD;
  CURLcode           res_e;

  if ( !cul_u ) {
    c3_free(req_c);
    return MESA_AZIMUTH_BAD;
  }

  hed_u = curl_slist_append(hed_u, "Content-Type: application/json");
  curl_easy_setopt(cul_u, CURLOPT_URL, url ? url : MESA_AZIMUTH_URL);
  curl_easy_setopt(cul_u, CURLOPT_HTTPHEADER, hed_u);
  curl_easy_setopt(cul_u, CURLOPT_POSTFIELDS, req_c);
  curl_easy_setopt(cul_u, CURLOPT_WRITEFUNCTION, _curl_write);
  curl_easy_setopt(cul_u, CURLOPT_WRITEDATA, &buf_u);
  curl_easy_setopt(cul_u, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(cul_u, CURLOPT_NOSIGNAL, 1L);

  res_e = curl_easy_perform(cul_u);
  if ( (CURLE_OK == res_e) && buf_u.base ) {
    ret_e = mesa_azimuth_point_parse(who, buf_u.len, buf_u.base, out);
  }
  else {
    mesa_log("mesa: azimuth: %s", curl_easy_strerror(res_e));
  }

  curl_slist_free_all(hed_u);
  curl_easy_cleanup(cul_u);
  c3_free(buf_u.base);
  c3_free(req_c);
  return ret_e;
}

static void
_host_work_cb(uv_work_t* wok_u)
{
  _host_work* wor_u = wok_u->data;
  wor_u->ret_i = mesa_azimuth_fetch(wor_u->hos_u->azi_c, wor_u->who, &wor_u->pot_u);
}

static void
_host_work_done_cb(uv_work_t* wok_u, c3_i sas_i)
{
  _host_work* wor_u = wok_u->data;
  mesa_host*  hos_u = wor_u->hos_u;

  if ( MESA_AZIMUTH_OK == wor_u->ret_i ) {
    mesa_engine_point(hos_u->eng_u, &wor_u->pot_u);
  }
  else {
    mesa_engine_point_missing(hos_u->eng_u, wor_u->who);
  }
  mesa_host_kick(hos_u);
  c3_free(wor_u);
}

static void
_host_idle_cb(uv_idle_t* idl_u)
{
  mesa_host* hos_u = idl_u->data;

  uv_idle_stop(idl_u);

  while ( hos_u->ned_u ) {
    _host_need* ned_u = hos_u->ned_u;
    mesa_ship   who   = ned_u->who;

    hos_u->ned_u = ned_u->nex_u;
    c3_free(ned_u);

    if ( c3y == hos_u->fak_o ) {
      mesa_ident ide;
      mesa_point pot_u = { .who = who, .rift = 0, .life = 1,
                           .has_sponsor = true,
                           .sponsor = mesa_ship_sein(who) };
      mesa_ident_fake(who, &ide);
      mesa_ident_pass(&ide, pot_u.pass);
      mesa_engine_point(hos_u->eng_u, &pot_u);
    }
    else if ( (MESA_EARL == mesa_ship_rank(who)) || (MESA_PAWN == mesa_ship_rank(who)) ) {
      //  not on azimuth
      //
      mesa_engine_point_missing(hos_u->eng_u, who);
    }
    else {
      _host_work* wor_u = c3_calloc(sizeof(*wor_u));
      wor_u->hos_u = hos_u;
      wor_u->who   = who;
      wor_u->wok_u.data = wor_u;
      uv_queue_work(hos_u->lup_u, &wor_u->wok_u, _host_work_cb, _host_work_done_cb);
    }
  }
  mesa_host_kick(hos_u);
}

/* need_point() is called inside the engine; answer from the loop
*/
static void
_host_need_point(void* ctx, mesa_ship who)
{
  mesa_host*  hos_u = ctx;
  _host_need* ned_u = c3_calloc(sizeof(*ned_u));

  ned_u->who   = who;
  ned_u->nex_u = hos_u->ned_u;
  hos_u->ned_u = ned_u;
  uv_idle_start(&hos_u->idl_u, _host_idle_cb);
}

/* trampolines: the engine has one hook context (the host); hand the
**              application its own
*/
static mesa_plea_result
_tr_plea(void* ctx, mesa_ship her, uint64_t bone, mesa_noun vane,
         mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  mesa_host* hos_u = ctx;
  return hos_u->app_u.plea(hos_u->app_u.ctx, her, bone, vane, path, payload, error);
}

static void
_tr_boon(void* ctx, mesa_ship her, uint64_t bone, mesa_noun payload)
{
  mesa_host* hos_u = ctx;
  hos_u->app_u.boon(hos_u->app_u.ctx, her, bone, payload);
}

static void
_tr_done(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error)
{
  mesa_host* hos_u = ctx;
  hos_u->app_u.done(hos_u->app_u.ctx, her, bone, error);
}

static void
_tr_corked(void* ctx, mesa_ship her, uint64_t bone)
{
  mesa_host* hos_u = ctx;
  hos_u->app_u.corked(hos_u->app_u.ctx, her, bone);
}

static void
_tr_sage(void* ctx, mesa_ship her, const char* path, mesa_noun gage)
{
  mesa_host* hos_u = ctx;
  hos_u->app_u.sage(hos_u->app_u.ctx, her, path, gage);
}

static mesa_noun
_tr_peek(void* ctx, const mesa_ship* her, const char* path)
{
  mesa_host* hos_u = ctx;
  return hos_u->app_u.peek(hos_u->app_u.ctx, her, path);
}

static void
_tr_breach(void* ctx, mesa_ship her)
{
  mesa_host* hos_u = ctx;
  hos_u->app_u.breach(hos_u->app_u.ctx, her);
}

/* lifecycle
*/
int
mesa_host_open(const mesa_host_config* cfg, mesa_host** out)
{
  mesa_host*         hos_u = c3_calloc(sizeof(*hos_u));
  mesa_config        con_u = cfg->engine;
  mesa_ship          our   = con_u.ident.who;
  mesa_rank          ran_e = mesa_ship_rank(our);
  struct sockaddr_in add_u = { .sin_family = AF_INET };
  c3_s               por_s = cfg->port;
  int                ret_i;

  static c3_o cur_o = c3n;
  if ( c3n == cur_o ) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    cur_o = c3y;
  }

  hos_u->app_u = con_u.hooks;
  hos_u->fak_o = cfg->fake ? c3y : c3n;
  hos_u->azi_c = strdup(cfg->azimuth_url ? cfg->azimuth_url : MESA_AZIMUTH_URL);
  hos_u->dns_c = strdup(cfg->dns_domain ? cfg->dns_domain : "urbit.org");
  hos_u->lup_u = cfg->loop ? cfg->loop : uv_default_loop();
  for ( c3_w i = 0; i < 256; i++ ) hos_u->gaq_o[i] = c3n;

  //  our own point: keys must match, and it names our sponsor
  //
  if ( (c3n == hos_u->fak_o) && (MESA_PAWN != ran_e) && (MESA_EARL != ran_e) ) {
    mesa_point pot_u;
    c3_y       pas_y[65];

    if ( MESA_AZIMUTH_OK != mesa_azimuth_fetch(hos_u->azi_c, our, &pot_u) ) {
      ret_i = MESA_HOST_E_AZIMUTH;
      goto fail;
    }
    mesa_ident_pass(&con_u.ident, pas_y);
    if ( (pot_u.life != con_u.ident.life) || memcmp(pot_u.pass, pas_y, 65) ) {
      mesa_log("mesa: our keys don't match azimuth (life %" PRIu64 " there, "
               "%" PRIu64 " here)", pot_u.life, con_u.ident.life);
      ret_i = MESA_HOST_E_KEYS;
      goto fail;
    }
    if ( MESA_RIFT_UNKNOWN == con_u.ident.rift ) {
      con_u.ident.rift = pot_u.rift;
    }
    if ( pot_u.has_sponsor ) {
      con_u.our_sponsor = pot_u.sponsor;
    }
  }

  con_u.hooks.ctx        = hos_u;
  con_u.hooks.send       = _host_send;
  con_u.hooks.need_point = _host_need_point;
  //  the application's hooks, behind trampolines that restore its ctx
  //
  con_u.hooks.plea   = hos_u->app_u.plea   ? _tr_plea   : 0;
  con_u.hooks.boon   = hos_u->app_u.boon   ? _tr_boon   : 0;
  con_u.hooks.done   = hos_u->app_u.done   ? _tr_done   : 0;
  con_u.hooks.corked = hos_u->app_u.corked ? _tr_corked : 0;
  con_u.hooks.sage   = hos_u->app_u.sage   ? _tr_sage   : 0;
  con_u.hooks.peek   = hos_u->app_u.peek   ? _tr_peek   : 0;
  con_u.hooks.breach = hos_u->app_u.breach ? _tr_breach : 0;
  con_u.now = _now();

  if ( MESA_OK != (ret_i = mesa_engine_open(&con_u, &hos_u->eng_u)) ) {
    goto fail;
  }

  //  the socket
  //
  if ( !por_s && (MESA_CZAR == ran_e) ) {
    por_s = ( c3y == hos_u->fak_o ) ? (31337 + our.lo) : (13337 + our.lo);
  }
  add_u.sin_addr.s_addr = htonl(( c3y == hos_u->fak_o ) ? 0x7f000001 : INADDR_ANY);
  add_u.sin_port = htons(por_s);

  uv_udp_init(hos_u->lup_u, &hos_u->wax_u);
  hos_u->wax_u.data = hos_u;
  if (  uv_udp_bind(&hos_u->wax_u, (const struct sockaddr*)&add_u, 0)
     || uv_udp_recv_start(&hos_u->wax_u, _host_alloc_cb, _host_recv_cb) )
  {
    mesa_log("mesa: can't bind udp port %u", por_s);
    mesa_engine_close(hos_u->eng_u);
    uv_close((uv_handle_t*)&hos_u->wax_u, 0);
    uv_run(hos_u->lup_u, UV_RUN_NOWAIT);
    ret_i = MESA_HOST_E_SOCKET;
    goto fail;
  }

  uv_timer_init(hos_u->lup_u, &hos_u->tim_u);
  hos_u->tim_u.data = hos_u;
  uv_idle_init(hos_u->lup_u, &hos_u->idl_u);
  hos_u->idl_u.data = hos_u;

  mesa_host_kick(hos_u);
  *out = hos_u;
  return MESA_OK;

fail:
  c3_free(hos_u->azi_c);
  c3_free(hos_u->dns_c);
  c3_free(hos_u);
  return ret_i;
}

mesa_engine*
mesa_host_engine(mesa_host* hos_u)
{
  return hos_u->eng_u;
}

void*
mesa_host_loop(mesa_host* hos_u)
{
  return hos_u->lup_u;
}

int
mesa_host_run(mesa_host* hos_u)
{
  return uv_run(hos_u->lup_u, UV_RUN_DEFAULT);
}

void
mesa_host_stop(mesa_host* hos_u)
{
  uv_stop(hos_u->lup_u);
}

void
mesa_host_close(mesa_host* hos_u)
{
  mesa_engine_close(hos_u->eng_u);
  uv_udp_recv_stop(&hos_u->wax_u);
  uv_close((uv_handle_t*)&hos_u->wax_u, 0);
  uv_close((uv_handle_t*)&hos_u->tim_u, 0);
  uv_close((uv_handle_t*)&hos_u->idl_u, 0);
  uv_run(hos_u->lup_u, UV_RUN_NOWAIT);

  while ( hos_u->ned_u ) {
    _host_need* nex_u = hos_u->ned_u->nex_u;
    c3_free(hos_u->ned_u);
    hos_u->ned_u = nex_u;
  }
  c3_free(hos_u->azi_c);
  c3_free(hos_u->dns_c);
  c3_free(hos_u);
}
