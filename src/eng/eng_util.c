/// @file

#include "eng.h"

/* outbound datagrams: queued, released after commit
*/
void
eng_send(mesa_engine* eng, mesa_lane lan_u, c3_y* buf_y, c3_z len_z)
{
  eng_out* out_u;

  if ( MESA_LANE_NONE == lan_u.kind ) {
    c3_free(buf_y);
    return;
  }

  out_u = c3_malloc(sizeof(*out_u));
  out_u->lan_u = lan_u;
  out_u->len_z = len_z;
  out_u->buf_y = buf_y;
  out_u->nex_u = 0;

  *eng->ont_u = out_u;
  eng->ont_u  = &out_u->nex_u;
}

/* eng_trace(): $MESA_TRACE logs every packet's names
*/
static void
_trace_name(const c3_c* lab_c, u3_mesa_name* nam_u)
{
  mesa_log("mesa:   %s %s r%u f%" PRIu64 "%s /%.*s", lab_c,
           eng_ship_text(nam_u->her_u), (unsigned)nam_u->rif_h, nam_u->fra_d,
           ( c3y == nam_u->nit_o ) ? ( c3y == nam_u->aut_o ? " auth" : " init" ) : "",
           (int)c3_min(nam_u->pat_s, 160), nam_u->pat_c);
}

void
eng_trace(const c3_c* dir_c, u3_mesa_pact* pac_u)
{
  static c3_i tra_i = -1;

  if ( tra_i < 0 ) {
    tra_i = getenv("MESA_TRACE") ? 1 : 0;
  }
  if ( !tra_i ) {
    return;
  }
  switch ( pac_u->hed_u.typ_y ) {
    case PACT_PEEK: {
      mesa_log("mesa: %s peek", dir_c);
      _trace_name("name", &pac_u->pek_u.nam_u);
    } break;
    case PACT_PAGE: {
      mesa_log("mesa: %s page (%" PRIu64 " bytes)", dir_c, pac_u->pag_u.dat_u.tob_d);
      _trace_name("name", &pac_u->pag_u.nam_u);
    } break;
    case PACT_POKE: {
      mesa_log("mesa: %s poke (%" PRIu64 " bytes)", dir_c, pac_u->pok_u.dat_u.tob_d);
      _trace_name("ack ", &pac_u->pok_u.nam_u);
      _trace_name("pay ", &pac_u->pok_u.pay_u);
    } break;
    default: break;
  }
}

void
eng_send_pact(mesa_engine* eng, mesa_lane lan_u, u3_mesa_pact* pac_u)
{
  c3_y* buf_y = c3_calloc(PACT_SIZE);
  c3_h  len_h;

  eng_trace("send", pac_u);
  pac_u->hed_u.pro_y = MESA_VER;
  len_h = mesa_etch_pact_to_buf(buf_y, PACT_SIZE, pac_u);
  eng_send(eng, lan_u, buf_y, len_h);
}

static void
_eng_flush(mesa_engine* eng)
{
  eng_out* out_u = eng->out_u;

  eng->out_u = 0;
  eng->ont_u = &eng->out_u;

  while ( out_u ) {
    eng_out* nex_u = out_u->nex_u;
    if ( eng->hok_u.send ) {
      eng->hok_u.send(eng->hok_u.ctx, out_u->lan_u, out_u->buf_y, out_u->len_z);
    }
    c3_free(out_u->buf_y);
    c3_free(out_u);
    out_u = nex_u;
  }
}

static void
_eng_drop(mesa_engine* eng)
{
  eng_out* out_u = eng->out_u;

  eng->out_u = 0;
  eng->ont_u = &eng->out_u;

  while ( out_u ) {
    eng_out* nex_u = out_u->nex_u;
    c3_free(out_u->buf_y);
    c3_free(out_u);
    out_u = nex_u;
  }
}

c3_o
eng_lanes_eq(mesa_lane a, mesa_lane b)
{
  if ( a.kind != b.kind ) return c3n;
  switch ( a.kind ) {
    case MESA_LANE_NONE:   return c3y;
    case MESA_LANE_GALAXY: return __(a.galaxy == b.galaxy);
    case MESA_LANE_IP:     return __((a.ip == b.ip) && (a.port == b.port));
  }
  return c3n;
}

/* transactions
**
**   Engine entry points open a transaction unless the application already
**   has one open; the outermost close commits and releases datagrams.
*/
c3_o
eng_txn_open(mesa_engine* eng)
{
  if ( c3y == eng->bad_o ) {
    return c3n;
  }
  if ( 0 == eng->txn_w++ ) {
    if ( eng->sto_u->begin(eng->sto_u->ctx) ) {
      eng->txn_w = 0;
      eng_txn_fail(eng);
      return c3n;
    }
  }
  return c3y;
}

c3_o
eng_txn_close(mesa_engine* eng)
{
  u3_assert( eng->txn_w );

  if ( --eng->txn_w ) {
    return c3y;
  }
  if ( c3y == eng->bad_o ) {
    eng->sto_u->rollback(eng->sto_u->ctx);
    _eng_drop(eng);
    return c3n;
  }
  if ( eng->sto_u->commit(eng->sto_u->ctx) ) {
    eng_txn_fail(eng);
    _eng_drop(eng);
    return c3n;
  }
  _eng_flush(eng);

  //  something was rolled back under us: rebuild from the store
  //
  if ( c3y == eng->rel_o ) {
    eng->rel_o = c3n;
    eng_reload(eng);
  }
  return c3y;
}

/* eng_txn_fail(): the store failed; our memory no longer matches it, so
**                 stop. The host must reopen the engine from the store.
*/
void
eng_txn_fail(mesa_engine* eng)
{
  if ( c3n == eng->bad_o ) {
    mesa_log("mesa: store failed; engine stopped");
  }
  eng->bad_o = c3y;
}

void
eng_put(mesa_engine* eng, c3_y* key_y, c3_z key_z, mesa_noun val)
{
  c3_z  len_z;
  c3_y* buf_y;

  if ( c3y == eng->bad_o ) {
    mesa_unref(val);
    return;
  }

  buf_y = mesa_jam_bytes(val, &len_z);
  mesa_unref(val);

  if ( eng->sto_u->put(eng->sto_u->ctx, key_y, key_z, buf_y, len_z) ) {
    eng_txn_fail(eng);
  }
  c3_free(buf_y);
}

void
eng_del(mesa_engine* eng, c3_y* key_y, c3_z key_z)
{
  if ( c3y == eng->bad_o ) {
    return;
  }
  if ( eng->sto_u->del(eng->sto_u->ctx, key_y, key_z) ) {
    eng_txn_fail(eng);
  }
}

/* store keys: type, ship (16 bytes big-endian), bone (8), dire (1),
**             seq (8); big-endian so scans come back in order
*/
static void
_put_be(c3_y* buf_y, c3_d val_d)
{
  for ( c3_w i = 0; i < 8; i++ ) {
    buf_y[i] = (c3_y)(val_d >> (56 - (8 * i)));
  }
}

c3_z
eng_key(c3_y typ_y, u3_ship who_u, c3_d bon_d, eng_dire dir_e,
        c3_d seq_d, c3_y key_y[40])
{
  key_y[0] = typ_y;

  if ( KEY_IDENT == typ_y ) {
    return 1;
  }

  _put_be(key_y + 1, who_u[1]);
  _put_be(key_y + 9, who_u[0]);
  if ( KEY_PEER == typ_y ) {
    return 17;
  }

  _put_be(key_y + 17, bon_d);
  key_y[25] = (c3_y)dir_e;
  if ( (KEY_FLOW == typ_y) || (KEY_CORK == typ_y) ) {
    return 26;
  }

  _put_be(key_y + 26, seq_d);
  return 34;
}

/* paths
*/
c3_c*
eng_path_text(mesa_noun pax)
{
  c3_z  cap_z = 64, len_z = 0;
  c3_c* out_c = c3_malloc(cap_z);

  out_c[0] = 0;
  while ( mesa_is_cell(pax) ) {
    mesa_noun i = mesa_head(pax);
    c3_z      met_z = mesa_is_atom(i) ? mesa_met3(i) : 0;

    if ( cap_z < len_z + met_z + 2 ) {
      cap_z = (len_z + met_z + 2) * 2;
      out_c = c3_realloc(out_c, cap_z);
    }
    out_c[len_z++] = '/';
    if ( met_z ) {
      mesa_atom_get_bytes(i, met_z, (c3_y*)out_c + len_z);
      len_z += met_z;
    }
    out_c[len_z] = 0;
    pax = mesa_tail(pax);
  }

  if ( !len_z ) {
    out_c[0] = '/';
    out_c[1] = 0;
  }
  return out_c;
}

mesa_noun
eng_path_noun(const c3_c* pat_c)
{
  knot_path pax_u;
  mesa_noun pax = mesa_atom_u64(0);

  if ( c3n == knot_path_split(strlen(pat_c), pat_c, &pax_u) ) {
    return MESA_NONE;
  }

  for ( c3_w i = pax_u.len_w; i-- > 0; ) {
    pax = mesa_cell(mesa_atom_bytes(pax_u.seg_u[i].len_z,
                                    (const c3_y*)pax_u.seg_u[i].str_c),
                    pax);
  }
  return pax;
}

c3_c*
eng_ship_text(u3_ship who_u)
{
  static c3_c buf_c[64];
  c3_c*       who_c = u3_ship_to_string(who_u);

  snprintf(buf_c, sizeof(buf_c), "%s", who_c);
  c3_free(who_c);
  return buf_c;
}

/* eng_out_cut(): drop datagrams queued since [mar_u] (a saved tail)
*/
void
eng_out_cut(mesa_engine* eng, eng_out** mar_u)
{
  eng_out* out_u = *mar_u;

  *mar_u = 0;
  eng->ont_u = mar_u;

  while ( out_u ) {
    eng_out* nex_u = out_u->nex_u;
    c3_free(out_u->buf_y);
    c3_free(out_u);
    out_u = nex_u;
  }
}
