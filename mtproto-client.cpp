/*
    This file is part of tgl-library

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    Copyright Nikolay Durov, Andrey Lopatin 2012-2013
              Vitaly Valtman 2013-2015
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define        _FILE_OFFSET_BITS        64

#include <algorithm>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#ifdef WIN32
#include <winsock2.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
//#include <poll.h>
#endif
#include "crypto/rand.h"
#include "crypto/rsa_pem.h"
#include "crypto/sha.h"

#include "queries.h"
#include "tgl-structures.h"
#include "tgl-binlog.h"
#include "tgl.h"
#include "mtproto-client.h"
#include "updates.h"
#include "mtproto-utils.h"
extern "C" {
#include "tools.h"
#include "auto.h"
#include "auto/auto-types.h"
#include "auto/auto-skip.h"
#include "mtproto-common.h"
}
#include "tgl-methods-in.h"

#define MAX_NET_RES        (1L << 16)
//extern int log_level;

static long long generate_next_msg_id(struct tgl_dc *DC, struct tgl_session *S);
static double get_server_time (struct tgl_dc *DC);

// for statistic only
static int total_packets_sent;
static long long total_data_sent;


static int rpc_execute (struct connection *c, int op, int len);
static int rpc_becomes_ready (struct connection *c);
static int rpc_close (struct connection *c);

static double get_utime (int clock_id) {
    struct timespec T;
    tgl_my_clock_gettime (clock_id, &T);
    return T.tv_sec + (double) T.tv_nsec * 1e-9;
}


#define MAX_RESPONSE_SIZE        (1L << 24)

static TGLC_rsa *rsa_load_public_key (const char *public_key) {
  TGLC_rsa *res = TGLC_pem_read_RSAPublicKey (public_key);
  if (res == NULL) {
    TGL_WARNING("TGLC_pem_read_RSAPublicKey returns NULL.\n");
    return NULL;
  }

  return res;
}




/*
 *
 *        UNAUTHORIZED (DH KEY EXCHANGE) PROTOCOL PART
 *
 */

#define ENCRYPT_BUFFER_INTS        16384
static int encrypt_buffer[ENCRYPT_BUFFER_INTS];

#define DECRYPT_BUFFER_INTS        16384
static int decrypt_buffer[ENCRYPT_BUFFER_INTS];

int tgl_pad_rsa_encrypt (char *from, int from_len, char *to, int size, BIGNUM *N, BIGNUM *E) {
    int pad = (255000 - from_len - 32) % 255 + 32;
    int chunks = (from_len + pad) / 255;
    int bits = BN_num_bits (N);
    assert (bits >= 2041 && bits <= 2048);
    assert (from_len > 0 && from_len <= 2550);
    assert (size >= chunks * 256);
    assert (RAND_pseudo_bytes ((unsigned char *) from + from_len, pad) >= 0);
    int i;
    BIGNUM x, y;
    BN_init (&x);
    BN_init (&y);
    for (i = 0; i < chunks; i++) {
        BN_bin2bn ((unsigned char *) from, 255, &x);
        assert (BN_mod_exp (&y, &x, E, N, tgl_state::instance()->BN_ctx) == 1);
        unsigned l = 256 - BN_num_bytes (&y);
        assert (l <= 256);
        memset (to, 0, l);
        BN_bn2bin (&y, (unsigned char *) to + l);
        to += 256;
    }
    BN_free (&x);
    BN_free (&y);
    return chunks * 256;
}

static int encrypt_packet_buffer (struct tgl_dc *DC) {
  TGLC_rsa *key = (TGLC_rsa *)tgl_state::instance()->rsa_key_loaded[DC->rsa_key_idx];
  return tgl_pad_rsa_encrypt ((char *) packet_buffer, (packet_ptr - packet_buffer) * 4, (char *) encrypt_buffer, ENCRYPT_BUFFER_INTS * 4, TGLC_rsa_n (key), TGLC_rsa_e (key));
}

static int encrypt_packet_buffer_aes_unauth (const unsigned char server_nonce[16], const unsigned char hidden_client_nonce[32]) {
  tgl_init_aes_unauth (server_nonce, hidden_client_nonce, AES_ENCRYPT);
  return tgl_pad_aes_encrypt ((unsigned char *) packet_buffer, (packet_ptr - packet_buffer) * 4, (unsigned char *) encrypt_buffer, ENCRYPT_BUFFER_INTS * 4);
}

//
// Used in unauthorized part of protocol
//
static int rpc_send_packet (struct connection *c) {
    static struct {
        long long auth_key_id;
        long long out_msg_id;
        int msg_len;
    } unenc_msg_header;

    int len = (packet_ptr - packet_buffer) * 4;
    tgl_state::instance()->net_methods->incr_out_packet_num (c);

    struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
    struct tgl_session *S = tgl_state::instance()->net_methods->get_session (c);

    unenc_msg_header.out_msg_id = generate_next_msg_id (DC, S);
    unenc_msg_header.msg_len = len;

    int total_len = len + 20;
    assert (total_len > 0 && !(total_len & 0xfc000003));
    total_len >>= 2;
    TGL_DEBUG("writing packet: total_len = " << total_len << ", len = " << len);
    if (total_len < 0x7f) {
        assert (tgl_state::instance()->net_methods->write_out (c, &total_len, 1) == 1);
    } else {
        total_len = (total_len << 8) | 0x7f;
        assert (tgl_state::instance()->net_methods->write_out (c, &total_len, 4) == 4);
    }
    tgl_state::instance()->net_methods->write_out (c, &unenc_msg_header, 20);
    tgl_state::instance()->net_methods->write_out (c, packet_buffer, len);
    tgl_state::instance()->net_methods->flush_out (c);

    total_packets_sent ++;
    total_data_sent += total_len;
    return 1;
}

static int rpc_send_message (struct connection *c, void *data, int len) {
    assert (len > 0 && !(len & 0xfc000003));

    int total_len = len >> 2;
    if (total_len < 0x7f) {
        assert (tgl_state::instance()->net_methods->write_out (c, &total_len, 1) == 1);
    } else {
        total_len = (total_len << 8) | 0x7f;
        assert (tgl_state::instance()->net_methods->write_out (c, &total_len, 4) == 4);
    }

    tgl_state::instance()->net_methods->incr_out_packet_num (c);
    assert (tgl_state::instance()->net_methods->write_out (c, data, len) == len);
    tgl_state::instance()->net_methods->flush_out (c);

    total_packets_sent ++;
    total_data_sent += total_len;
    return 1;
}

//
// State machine. See description at
// https://core.telegram.org/mtproto/auth_key
//


static int check_unauthorized_header () {
    long long auth_key_id = fetch_long ();
    if (auth_key_id) {
        TGL_ERROR("ERROR: auth_key_id should be NULL\n");
        return -1;
    }
    fetch_long (); // msg_id
    int len = fetch_int ();
    if (len != 4 * (in_end - in_ptr)) {
        TGL_ERROR("ERROR: length mismatch\n");
        return -1;
    }
    return 0;
}

/* {{{ REQ_PQ */
// req_pq#60469778 nonce:int128 = ResPQ
static int send_req_pq_packet (struct connection *c) {
    struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
    assert (DC->state == st_init);

    tglt_secure_random (DC->nonce, 16);
    clear_packet ();
    out_int (CODE_req_pq);
    out_ints ((int *)DC->nonce, 4);
    rpc_send_packet(c);

    DC->state = st_reqpq_sent;
    return 1;
}

// req_pq#60469778 nonce:int128 = ResPQ
static int send_req_pq_temp_packet (struct connection *c) {
    struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
    assert (DC->state == st_authorized);

    tglt_secure_random (DC->nonce, 16);
    clear_packet ();
    out_int (CODE_req_pq);
    out_ints ((int *)DC->nonce, 4);
    rpc_send_packet(c);

    DC->state = st_reqpq_sent_temp;
    return 1;
}
/* }}} */

/* {{{ REQ DH */
// req_DH_params#d712e4be nonce:int128 server_nonce:int128 p:string q:string public_key_fingerprint:long encrypted_data:string = Server_DH_Params;
// p_q_inner_data#83c95aec pq:string p:string q:string nonce:int128 server_nonce:int128 new_nonce:int256 = P_Q_inner_data;
// p_q_inner_data_temp#3c6a84d4 pq:string p:string q:string nonce:int128 server_nonce:int128 new_nonce:int256 expires_in:int = P_Q_inner_data;
static void send_req_dh_packet (struct connection *c, TGLC_bn *pq, int temp_key) {
  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);

  TGLC_bn *p = TGLC_bn_new ();
  TGLC_bn *q = TGLC_bn_new ();
  assert (bn_factorize (pq, p, q) >= 0);

  clear_packet ();
  packet_ptr += 5;
  out_int (temp_key ? CODE_p_q_inner_data_temp : CODE_p_q_inner_data);

  out_bignum (pq);
  out_bignum (p);
  out_bignum (q);

  out_ints ((int *) DC->nonce, 4);
  out_ints ((int *) DC->server_nonce, 4);
  tglt_secure_random (DC->new_nonce, 32);
  out_ints ((int *) DC->new_nonce, 8);
  if (temp_key) {
    out_int (tgl_state::instance()->temp_key_expire_time);
  }
  TGLC_sha1 ((unsigned char *) (packet_buffer + 5), (packet_ptr - packet_buffer - 5) * 4, (unsigned char *) packet_buffer);

  int l = encrypt_packet_buffer (DC);

  clear_packet ();
  out_int (CODE_req_DH_params);
  out_ints ((int *) DC->nonce, 4);
  out_ints ((int *) DC->server_nonce, 4);
  out_bignum (p);
  out_bignum (q);

  out_long (tgl_state::instance()->rsa_key_fingerprint[DC->rsa_key_idx]);
  out_cstring ((char *) encrypt_buffer, l);

  TGLC_bn_free (p);
  TGLC_bn_free (q);
  DC->state = temp_key ? st_reqdh_sent_temp : st_reqdh_sent;
  rpc_send_packet (c);
}
/* }}} */

/* {{{ SEND DH PARAMS */
// set_client_DH_params#f5045f1f nonce:int128 server_nonce:int128 encrypted_data:string = Set_client_DH_params_answer;
// client_DH_inner_data#6643b654 nonce:int128 server_nonce:int128 retry_id:long g_b:string = Client_DH_Inner_Data
static void send_dh_params (struct connection *c, TGLC_bn *dh_prime, TGLC_bn *g_a, int g, int temp_key) {
  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);

  clear_packet ();
  packet_ptr += 5;
  out_int (CODE_client_DH_inner_data);
  out_ints ((int *) DC->nonce, 4);
  out_ints ((int *) DC->server_nonce, 4);
  out_long (0);

  TGLC_bn *dh_g = TGLC_bn_new ();
  ensure (TGLC_bn_set_word (dh_g, g));

  static unsigned char s_power[256];
  tglt_secure_random (s_power, 256);
  TGLC_bn *dh_power = TGLC_bn_bin2bn ((unsigned char *)s_power, 256, 0);
  ensure_ptr (dh_power);

  TGLC_bn *y = TGLC_bn_new ();
  ensure_ptr (y);
  ensure (TGLC_bn_mod_exp (y, dh_g, dh_power, dh_prime, tgl_state::instance()->TGLC_bn_ctx));
  out_bignum (y);
  TGLC_bn_free (y);

  TGLC_bn *auth_key_num = TGLC_bn_new ();
  ensure (TGLC_bn_mod_exp (auth_key_num, g_a, dh_power, dh_prime, tgl_state::instance()->TGLC_bn_ctx));
  int l = TGLC_bn_num_bytes (auth_key_num);
  assert (l >= 250 && l <= 256);
  assert (TGLC_bn_bn2bin (auth_key_num, (unsigned char *)(temp_key ? DC->temp_auth_key : DC->auth_key)));
  if (l < 256) {
    unsigned char *key = temp_key ? DC->temp_auth_key : DC->auth_key;
    memmove (key + 256 - l, key, l);
    memset (key, 0, 256 - l);
  }

  TGLC_bn_free (dh_power);
  TGLC_bn_free (auth_key_num);
  TGLC_bn_free (dh_g);

  TGLC_sha1 ((unsigned char *) (packet_buffer + 5), (packet_ptr - packet_buffer - 5) * 4, (unsigned char *) packet_buffer);

  l = encrypt_packet_buffer_aes_unauth (DC->server_nonce, DC->new_nonce);

  clear_packet ();
  out_int (CODE_set_client_DH_params);
  out_ints ((int *) DC->nonce, 4);
  out_ints ((int *) DC->server_nonce, 4);
  out_cstring ((char *) encrypt_buffer, l);

  DC->state = temp_key ? st_client_dh_sent_temp : st_client_dh_sent;;
  rpc_send_packet (c);
}
/* }}} */

/* {{{ RECV RESPQ */
// resPQ#05162463 nonce:int128 server_nonce:int128 pq:string server_public_key_fingerprints:Vector long = ResPQ
static int process_respq_answer (struct connection *c, char *packet, int len, int temp_key) {
  assert (!(len & 3));
  in_ptr = (int *)packet;
  in_end = in_ptr + (len / 4);
  if (check_unauthorized_header () < 0) {
    return -1;
  }

  int *in_save = in_ptr;
  struct paramed_type type = TYPE_TO_PARAM (res_p_q);
  if (skip_type_any (&type) < 0 || in_ptr != in_end) {
    TGL_ERROR("can not parse req_p_q answer\n");
    return -1;
  }
  in_ptr = in_save;

  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);

  assert (fetch_int() == CODE_res_p_q);

  static int tmp[4];
  fetch_ints (tmp, 4);
  if (memcmp (tmp, DC->nonce, 16)) {
    TGL_ERROR("nonce mismatch\n");
    return -1;
  }
  fetch_ints (DC->server_nonce, 4);

  TGLC_bn *pq = TGLC_bn_new ();
  assert (fetch_bignum (pq) >= 0);

  assert (fetch_int ()  == CODE_vector);
  int fingerprints_num = fetch_int ();
  assert (fingerprints_num >= 0);
  DC->rsa_key_idx = -1;

  int i;
  for (i = 0; i < fingerprints_num; i++) {
    int j;
    long long fprint = fetch_long ();
    for (j = 0; j < tgl_state::instance()->rsa_key_loaded.size(); j++) {
      if (tgl_state::instance()->rsa_key_loaded[j]) {
        if (fprint == tgl_state::instance()->rsa_key_fingerprint[j]) {
          DC->rsa_key_idx = j;
          break;
        }
    }
    assert (in_ptr == in_end);
    if (DC->rsa_key_idx == -1) {
        TGL_ERROR("fatal: don't have any matching keys\n");
        return -1;
    }

    send_req_dh_packet(c, pq, temp_key);

  TGLC_bn_free (pq);
  return 1;
}
/* }}} */

/* {{{ RECV DH */
// server_DH_params_fail#79cb045d nonce:int128 server_nonce:int128 new_nonce_hash:int128 = Server_DH_Params;
// server_DH_params_ok#d0e8075c nonce:int128 server_nonce:int128 encrypted_answer:string = Server_DH_Params;
// server_DH_inner_data#b5890dba nonce:int128 server_nonce:int128 g:int dh_prime:string g_a:string server_time:int = Server_DH_inner_data;
static int process_dh_answer (struct connection *c, char *packet, int len, int temp_key) {
  assert (!(len & 3));
  in_ptr = (int *)packet;
  in_end = in_ptr + (len / 4);
  if (check_unauthorized_header () < 0) {
    return -1;
  }

  int *in_save = in_ptr;
  struct paramed_type type = TYPE_TO_PARAM (server_d_h_params);
  if (skip_type_any (&type) < 0 || in_ptr != in_end) {
    TGL_ERROR("can not parse server_DH_params answer");
    return -1;
  }
  in_ptr = in_save;

  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);

  unsigned op = fetch_int ();
  assert (op == CODE_server__d_h_params_ok || op == CODE_server__d_h_params_fail);

  int tmp[4];
  fetch_ints (tmp, 4);
  if (memcmp (tmp, DC->nonce, 16)) {
    TGL_ERROR("nonce mismatch");
    return -1;
  }
  assert (!memcmp (tmp, DC->nonce, 16));
  fetch_ints (tmp, 4);
  if (memcmp (tmp, DC->server_nonce, 16)) {
    TGL_ERROR("nonce mismatch");
    return -1;
  }
  assert (!memcmp (tmp, DC->server_nonce, 16));

  if (op == CODE_server__d_h_params_fail) {
    TGL_ERROR("DH params fail");
    return -1;
  }

  tgl_init_aes_unauth (DC->server_nonce, DC->new_nonce, AES_DECRYPT);

  int l = prefetch_strlen ();
  assert (l >= 0);
  if (!l) {
    vlogprintf (E_ERROR, "non-empty encrypted part expected\n");
    return -1;
  }
  l = tgl_pad_aes_decrypt((unsigned char *)fetch_str (l), l, (unsigned char *) decrypt_buffer, DECRYPT_BUFFER_INTS * 4 - 16);
  assert (in_ptr == in_end);

  in_ptr = decrypt_buffer + 5;
  in_end = decrypt_buffer + (l >> 2);
  struct paramed_type type2 = TYPE_TO_PARAM (server_d_h_inner_data);
  if (skip_type_any (&type2) < 0) {
    TGL_ERROR("can not parse server_DH_inner_data answer\n");
    return -1;
  }
  in_ptr = decrypt_buffer + 5;

  TGLC_bn *dh_prime = TGLC_bn_new ();
  TGLC_bn *g_a = TGLC_bn_new ();
  assert (fetch_bignum (dh_prime) > 0);
  assert (fetch_bignum (g_a) > 0);

  if (tglmp_check_DH_params (tgl_state::instance()->BN_ctx, dh_prime, g) < 0) {
    TGL_ERROR("bad DH params");
    return -1;
  }
  if (tglmp_check_g_a (dh_prime, g_a) < 0) {
    TGL_ERROR("bad dh_prime");
    return -1;
  }

  static char sha1_buffer[20];
  TGLC_sha1 ((unsigned char *) decrypt_buffer + 20, (in_ptr - decrypt_buffer - 5) * 4, (unsigned char *) sha1_buffer);
  if (memcmp (decrypt_buffer, sha1_buffer, 20)) {
    TGL_ERROR("bad encrypted message SHA1");
    return -1;
  }
  if ((char *) in_end - (char *) in_ptr >= 16) {
    TGL_ERROR("too much padding");
    return -1;
  }

  DC->server_time_delta = server_time - get_utime (CLOCK_REALTIME);
  DC->server_time_udelta = server_time - get_utime (CLOCK_MONOTONIC);

  send_dh_params(c, dh_prime, g_a, g, temp_key);

  TGLC_bn_free (dh_prime);
  TGLC_bn_free (g_a);

  return 1;
}
/* }}} */

static void create_temp_auth_key (struct connection *c) {
  assert(tgl_state::instance()->pfs_enabled());
  send_req_pq_temp_packet(c);
}

int tglmp_encrypt_inner_temp (struct connection *c, int *msg, int msg_ints, int useful, void *data, long long msg_id);
static long long msg_id_override;
static void mpc_on_get_config (void *extra, int success);
static void bind_temp_auth_key (struct connection *c);

/* {{{ RECV AUTH COMPLETE */

// dh_gen_ok#3bcbf734 nonce:int128 server_nonce:int128 new_nonce_hash1:int128 = Set_client_DH_params_answer;
// dh_gen_retry#46dc1fb9 nonce:int128 server_nonce:int128 new_nonce_hash2:int128 = Set_client_DH_params_answer;
// dh_gen_fail#a69dae02 nonce:int128 server_nonce:int128 new_nonce_hash3:int128 = Set_client_DH_params_answer;
static int process_auth_complete (struct connection *c, char *packet, int len, int temp_key) {
  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);

  assert (!(len & 3));
  in_ptr = (int *)packet;
  in_end = in_ptr + (len / 4);
  if (check_unauthorized_header () < 0) {
    return -1;
  }

  int *in_save = in_ptr;
  struct paramed_type type = TYPE_TO_PARAM (set_client_d_h_params_answer);
  if (skip_type_any (&type) < 0 || in_ptr != in_end) {
    TGL_ERROR("can not parse server_DH_params answer\n");
    return -1;
  }
  in_ptr = in_save;

  unsigned op = fetch_int ();
  assert (op == CODE_dh_gen_ok || op == CODE_dh_gen_retry || op == CODE_dh_gen_fail);

  int tmp[4];
  fetch_ints (tmp, 4);
  if (memcmp (DC->nonce, tmp, 16)) {
    TGL_ERROR("nonce mismatch\n");
    return -1;
  }
  fetch_ints (tmp, 4);
  if (memcmp (DC->server_nonce, tmp, 16)) {
    TGL_ERROR("nonce mismatch\n");
    return -1;
  }
  if (op != CODE_dh_gen_ok) {
    TGL_ERROR("DH failed. Retry\n");
    return -1;
  }

  fetch_ints (tmp, 4);

  static unsigned char th[44], sha1_buffer[20];
  memcpy (th, DC->new_nonce, 32);
  th[32] = 1;
  if (!temp_key) {
    TGLC_sha1 ((unsigned char *)DC->auth_key, 256, sha1_buffer);
  } else {
    TGLC_sha1 ((unsigned char *)DC->temp_auth_key, 256, sha1_buffer);
  }
  memcpy (th + 33, sha1_buffer, 8);
  TGLC_sha1 (th, 41, sha1_buffer);
  if (memcmp (tmp, sha1_buffer + 4, 16)) {
    TGL_ERROR("hash mismatch\n");
    return -1;
  }

  if (!temp_key) {
    //bl_do_set_auth_key (DC->id, (unsigned char *)DC->auth_key);
    tgl_state::instance()->set_auth_key(DC->id, NULL);
    TGLC_sha1 ((unsigned char *)DC->auth_key, 256, sha1_buffer);
  } else {
    TGLC_sha1 ((unsigned char *)DC->temp_auth_key, 256, sha1_buffer);
    DC->temp_auth_key_id = *(long long *)(sha1_buffer + 12);
  }

  DC->server_salt = *(long long *)DC->server_nonce ^ *(long long *)DC->new_nonce;

  DC->state = st_authorized;

  TGL_DEBUG("Auth success");
  if (temp_key) {
    bind_temp_auth_key (c);
  } else {
    DC->flags |= 1;
    if (tgl_state::instance()->pfs_enabled()) {
      create_temp_auth_key (c);
    } else {
        DC->temp_auth_key_id = DC->auth_key_id;
        memcpy (DC->temp_auth_key, DC->auth_key, 256);
        DC->flags |= 2;
        if (!(DC->flags & TGLDCF_CONFIGURED)) {
          tgl_do_help_get_config_dc(DC, mpc_on_get_config, DC);
        }
      }
    }

    return 1;
}
/* }}} */

static void bind_temp_auth_key (struct connection *c) {
    struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
    if (DC->temp_auth_key_bind_query_id) {
        tglq_query_delete(DC->temp_auth_key_bind_query_id);
    }
    struct tgl_session *S = tgl_state::instance()->net_methods->get_session (c);
    long long msg_id = generate_next_msg_id(DC, S);

    clear_packet ();
    out_int (CODE_bind_auth_key_inner);
    long long rand;
    tglt_secure_random ((unsigned char*)&rand, 8);
    out_long (rand);
    out_long (DC->temp_auth_key_id);
    out_long (DC->auth_key_id);

    if (!S->session_id) {
        tglt_secure_random ((unsigned char*)&S->session_id, 8);
    }
    out_long (S->session_id);
    int expires = time (0) + DC->server_time_delta + tgl_state::instance()->temp_key_expire_time;
    out_int (expires);

    static int data[1000];
    int len = tglmp_encrypt_inner_temp(c, packet_buffer, packet_ptr - packet_buffer, 0, data, msg_id);
    msg_id_override = msg_id;
    DC->temp_auth_key_bind_query_id = msg_id;
    tgl_do_send_bind_temp_key(DC, rand, expires, (void *)data, len, msg_id);
    msg_id_override = 0;
}

/*
 *
 *                AUTHORIZED (MAIN) PROTOCOL PART
 *
 */

static struct encrypted_message enc_msg;

static double get_server_time (struct tgl_dc *DC) {
    //if (!DC->server_time_udelta) {
    //  DC->server_time_udelta = get_utime (CLOCK_REALTIME) - get_utime (CLOCK_MONOTONIC);
    //}
    return get_utime (CLOCK_MONOTONIC) + DC->server_time_udelta;
}

static long long generate_next_msg_id (struct tgl_dc *DC, struct tgl_session *S) {
    long long next_id = (long long) (get_server_time (DC) * (1LL << 32)) & -4;
    if (next_id <= S->last_msg_id) {
        next_id = S->last_msg_id  += 4;
    } else {
        S->last_msg_id = next_id;
    }
    return next_id;
}

static void init_enc_msg (struct tgl_session *S, int useful) {
  struct tgl_dc *DC = S->dc;
  assert (DC->state == st_authorized);
  assert (DC->temp_auth_key_id);
  TGL_DEBUG("temp_auth_key_id = " << std::hex << DC->temp_auth_key_id << ", auth_key_id = " << DC->auth_key_id);
  enc_msg.auth_key_id = DC->temp_auth_key_id;
  enc_msg.server_salt = DC->server_salt;
  if (!S->session_id) {
    tglt_secure_random ((unsigned char*)&S->session_id, 8);
  }
  enc_msg.session_id = S->session_id;
  enc_msg.msg_id = msg_id_override ? msg_id_override : generate_next_msg_id (DC, S);
  enc_msg.seq_no = S->seq_no;
  if (useful) {
    enc_msg.seq_no |= 1;
  }
  S->seq_no += 2;
};

static void init_enc_msg_inner_temp (struct tgl_dc *DC, long long msg_id) {
    enc_msg.auth_key_id = DC->auth_key_id;
    tglt_secure_random ((unsigned char*)&enc_msg.server_salt, 8);
    tglt_secure_random ((unsigned char*)&enc_msg.session_id, 8);
    enc_msg.msg_id = msg_id;
    enc_msg.seq_no = 0;
};


static int aes_encrypt_message (unsigned char *key, struct encrypted_message *enc) {
  unsigned char sha1_buffer[20];
  const int MINSZ = offsetof (struct encrypted_message, message);
  const int UNENCSZ = offsetof (struct encrypted_message, server_salt);

  int enc_len = (MINSZ - UNENCSZ) + enc->msg_len;
  assert (enc->msg_len >= 0 && enc->msg_len <= MAX_MESSAGE_INTS * 4 - 16 && !(enc->msg_len & 3));
  TGLC_sha1 ((unsigned char *) &enc->server_salt, enc_len, sha1_buffer);
  TGL_DEBUG("sending message with sha1 " << *(int *)sha1_buffer);
  memcpy (enc->msg_key, sha1_buffer + 4, 16);
  tgl_init_aes_auth (key, enc->msg_key, AES_ENCRYPT);
  return tgl_pad_aes_encrypt ((unsigned char *) &enc->server_salt, enc_len, (unsigned char *) &enc->server_salt, MAX_MESSAGE_INTS * 4 + (MINSZ - UNENCSZ));
}

long long tglmp_encrypt_send_message (struct connection *c, int *msg, int msg_ints, int flags) {
    struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
    struct tgl_session *S = tgl_state::instance()->net_methods->get_session (c);
    assert (S);
    if (!(DC->flags & TGLDCF_CONFIGURED) && !(flags & QUERY_FORCE_SEND)) {
        //vlogprintf(E_NOTICE, "generate next msg ID...request not sent\n");
        return generate_next_msg_id(DC, S);
    }

    const int UNENCSZ = offsetof (struct encrypted_message, server_salt);
    if (msg_ints <= 0 || msg_ints > MAX_MESSAGE_INTS - 4) {
        TGL_NOTICE("message too long");
        return -1;
    }
    if (msg) {
        memcpy (enc_msg.message, msg, msg_ints * 4);
        enc_msg.msg_len = msg_ints * 4;
    } else {
        if ((enc_msg.msg_len & 0x80000003) || enc_msg.msg_len > MAX_MESSAGE_INTS * 4 - 16) {
            TGL_NOTICE("message too long");
            return -1;
        }
    }
    init_enc_msg (S, flags & 1);

    int l = aes_encrypt_message(DC->temp_auth_key, &enc_msg);
    assert (l > 0);
    rpc_send_message (c, &enc_msg, l + UNENCSZ);

    return S->last_msg_id;
}

int tglmp_encrypt_inner_temp (struct connection *c, int *msg, int msg_ints, int useful, void *data, long long msg_id) {
    TGL_UNUSED(useful);
    struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
    struct tgl_session *S = tgl_state::instance()->net_methods->get_session (c);
    assert (S);

    const int UNENCSZ = offsetof (struct encrypted_message, server_salt);
    if (msg_ints <= 0 || msg_ints > MAX_MESSAGE_INTS - 4) {
        return -1;
    }
    memcpy (enc_msg.message, msg, msg_ints * 4);
    enc_msg.msg_len = msg_ints * 4;

    init_enc_msg_inner_temp (DC, msg_id);

    int l = aes_encrypt_message(DC->auth_key, &enc_msg);
    assert (l > 0);
    //rpc_send_message (c, &enc_msg, l + UNENCSZ);
    memcpy (data, &enc_msg, l + UNENCSZ);

    return l + UNENCSZ;
}

static int rpc_execute_answer (struct connection *c, long long msg_id);

static int work_container (struct connection *c, long long msg_id) {
  TGL_DEBUG("work_container: msg_id = " << msg_id);
  assert (fetch_int () == CODE_msg_container);
  int n = fetch_int ();
  int i;
  for (i = 0; i < n; i++) {
    long long id = fetch_long ();
    //int seqno = fetch_int ();
    fetch_int (); // seq_no
    if (id & 1) {
      tgln_insert_msg_id(tgl_state::instance()->net_methods->get_session (c), id);
    }
    int bytes = fetch_int ();
    int *t = in_end;
    in_end = in_ptr + (bytes / 4);
    int r = rpc_execute_answer (c, id);
    if (r < 0) { return -1; }
    assert (in_ptr == in_end);
    in_end = t;
  }
  return 0;
}

static int work_new_session_created (struct connection *c, long long msg_id) {
  struct tgl_session *S = tgl_state::instance()->net_methods->get_session (c);
  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);

  vlogprintf (E_NOTICE, "work_new_session_created: msg_id = %" INT64_PRINTF_MODIFIER "d, dc = %d\n", msg_id, DC->id);
  assert (fetch_int () == (int)CODE_new_session_created);
  fetch_long (); // first message id
  fetch_long (); // unique_id
  tgl_state::instance()->net_methods->get_dc (c)->server_salt = fetch_long ();

  tglq_regen_queries_from_old_session (DC, S);

  if (tgl_state::instance()->started && !(tgl_state::instance()->locks & TGL_LOCK_DIFF) && (tgl_state::instance()->DC_working->flags & TGLDCF_LOGGED_IN)) {
    tgl_do_get_difference (0, 0, 0);
  }
  return 0;

    TGL_DEBUG("work_new_session_created: msg_id = " << msg_id);
    assert (fetch_int () == (int)CODE_new_session_created);
    fetch_long (); // first message id
    fetch_long (); // unique_id
    tgl_state::instance()->net_methods->get_dc (c)->server_salt = fetch_long ();

    tglq_regen_queries_from_old_session (DC, S);

    if (tgl_state::instance()->started && !(tgl_state::instance()->locks & TGL_LOCK_DIFF) && (tgl_state::instance()->DC_working->flags & TGLDCF_LOGGED_IN)) {
        tgl_do_get_difference(0, 0, 0);
    }
    return 0;
}

static int work_msgs_ack (struct connection *c, long long msg_id) {
  TGL_UNUSED(c);
  TGL_DEBUG("work_msgs_ack: msg_id = " << msg_id);
  assert (fetch_int () == CODE_msgs_ack);
  assert (fetch_int () == CODE_vector);
  int n = fetch_int ();
  int i;
  for (i = 0; i < n; i++) {
    long long id = fetch_long ();
    TGL_DEBUG("ack for " << id);
    tglq_query_ack (id);
  }
  return 0;
}

static int work_rpc_result (struct connection *c, long long msg_id) {
  TGL_UNUSED(c);
  TGL_DEBUG("work_rpc_result: msg_id = " << msg_id);
  assert (fetch_int () == (int)CODE_rpc_result);
  long long id = fetch_long ();
  int op = prefetch_int ();
  if (op == CODE_rpc_error) {
    return tglq_query_error (id);
  } else {
    return tglq_query_result (id);
  }
}

#define MAX_PACKED_SIZE (1 << 24)
static int work_packed (struct connection *c, long long msg_id) {
    assert (fetch_int () == CODE_gzip_packed);
    static int in_gzip;
    static int buf[MAX_PACKED_SIZE >> 2];
    assert (!in_gzip);
    in_gzip = 1;

    int l = prefetch_strlen ();
    char *s = fetch_str (l);

    int total_out = tgl_inflate (s, l, buf, MAX_PACKED_SIZE);
    int *end = in_ptr;
    int *eend = in_end;
    //assert (total_out % 4 == 0);
    in_ptr = buf;
    in_end = in_ptr + total_out / 4;
    int r = rpc_execute_answer(c, msg_id);
    in_ptr = end;
    in_end = eend;
    in_gzip = 0;
    return r;
}

static int work_bad_server_salt (struct connection *c, long long msg_id) {
  TGL_UNUSED(msg_id);
  assert (fetch_int () == (int)CODE_bad_server_salt);
  long long id = fetch_long ();
  fetch_int (); // seq_no
  fetch_int (); // error_code
  long long new_server_salt = fetch_long ();
  tgl_state::instance()->net_methods->get_dc (c)->server_salt = new_server_salt;
  tglq_query_restart (id);
  return 0;
}

static int work_pong () {
    assert (fetch_int () == CODE_pong);
    fetch_long (); // msg_id
    fetch_long (); // ping_id
    return 0;
}

static int work_detailed_info () {
    assert (fetch_int () == CODE_msg_detailed_info);
    fetch_long (); // msg_id
    fetch_long (); // answer_msg_id
    fetch_int (); // bytes
    fetch_int (); // status
    return 0;
}

static int work_new_detailed_info () {
    assert (fetch_int () == (int)CODE_msg_new_detailed_info);
    fetch_long (); // answer_msg_id
    fetch_int (); // bytes
    fetch_int (); // status
    return 0;
}

static int work_bad_msg_notification (struct connection *c, long long msg_id) {
  TGL_UNUSED(msg_id);
  TGL_UNUSED(c);
  assert (fetch_int () == (int)CODE_bad_msg_notification);
  long long m1 = fetch_long ();
  int s = fetch_int ();
  int e = fetch_int ();
  TGL_NOTICE("bad_msg_notification: msg_id = " << m1 << ", seq = " << s << ", error = " << e);
  switch (e) {
  // Too low msg id
  case 16:
    tglq_regen_query (m1);
    break;
  // Too high msg id
  case 17:
    tglq_regen_query (m1);
    break;
  // Bad container
  case 64:
    TGL_NOTICE("bad_msg_notification: msg_id = " << m1 << ", seq = " << s << ", error = " << e);
    tglq_regen_query (m1);
    break;
  default:
    TGL_NOTICE("bad_msg_notification: msg_id = " << m1 << ", seq = " << s << ", error = " << e);
    break;
  }

  return -1;
}

static int rpc_execute_answer (struct connection *c, long long msg_id) {
  unsigned int op = prefetch_int ();
  switch (op) {
  case CODE_msg_container:
    return work_container (c, msg_id);
  case CODE_new_session_created:
    return work_new_session_created (c, msg_id);
  case CODE_msgs_ack:
    return work_msgs_ack (c, msg_id);
  case CODE_rpc_result:
    return work_rpc_result (c, msg_id);
  case CODE_update_short:
  case CODE_updates:
  case CODE_update_short_message:
  case CODE_update_short_chat_message:
  case CODE_updates_too_long:
    tglu_work_any_updates_buf ();
    return 0;
  case CODE_gzip_packed:
    return work_packed (c, msg_id);
  case CODE_bad_server_salt:
    return work_bad_server_salt (c, msg_id);
  case CODE_pong:
    return work_pong ();
  case CODE_msg_detailed_info:
    return work_detailed_info();
  case CODE_msg_new_detailed_info:
    return work_new_detailed_info();
  case CODE_bad_msg_notification:
    return work_bad_msg_notification (c, msg_id);
  }
  TGL_WARNING("Unknown message: " << op);
  in_ptr = in_end; // Will not fail due to assertion in_ptr == in_end
  return 0;
}

//static struct mtproto_methods mtproto_methods;
void tgls_free_session (struct tgl_session *S);
/*
static char *get_ipv6 (int num) {
  static char res[1<< 10];
  if (TLS->test_mode) {
    switch (num) {
      case 1:
        strcpy (res, TG_SERVER_TEST_IPV6_1);
        break;
      case 2:
        strcpy (res, TG_SERVER_TEST_IPV6_2);
        break;
      case 3:
        strcpy (res, TG_SERVER_TEST_IPV6_3);
        break;
      default:
        assert (0);
    }
  } else {
    switch (num) {
      case 1:
        strcpy (res, TG_SERVER_IPV6_1);
        break;
      case 2:
        strcpy (res, TG_SERVER_IPV6_2);
        break;
      case 3:
        strcpy (res, TG_SERVER_IPV6_3);
        break;
      case 4:
        strcpy (res, TG_SERVER_IPV6_4);
        break;
      case 5:
        strcpy (res, TG_SERVER_IPV6_5);
        break;
      default:
        assert (0);
    }
  }
  return res;
}
*/

static int rpc_becomes_ready (struct connection *c) {
    return tc_becomes_ready(c);
}

static int tc_close (struct connection *c, int who) {
    TGL_DEBUG("outbound rpc connection from dc #" << tgl_state::instance()->net_methods->get_dc(c)->id << " : closing by " << who);
    return 0;
}

static struct mtproto_methods mtproto_methods = {
    .ready = rpc_becomes_ready,
    .close = rpc_close,
    .execute = rpc_execute,
};

static void create_session_connect (struct tgl_session *S) {
    struct tgl_dc *DC = S->dc;

    if (tgl_state::instance()->ipv6_enabled()) {
        S->c = tgl_state::instance()->net_methods->create_connection (DC->options[1]->ip, DC->options[1]->port, S, DC, &mtproto_methods);
    } else {
        S->c = tgl_state::instance()->net_methods->create_connection (DC->options[0]->ip, DC->options[0]->port, S, DC, &mtproto_methods);
    }
}

static void fail_connection (struct connection *c) {
    struct tgl_session *S = tgl_state::instance()->net_methods->get_session (c);
    tgl_state::instance()->net_methods->free (c);
    create_session_connect(S);
}

static void fail_session (struct tgl_session *S) {
  TGL_NOTICE("failing session " << S->session_id);
  struct tgl_dc *DC = S->dc;
  tgls_free_session (S);
  DC->sessions[0] = NULL;
  tglmp_dc_create_session (DC);
}

static int process_rpc_message (struct connection *c, struct encrypted_message *enc, int len) {
  const int MINSZ = offsetof (struct encrypted_message, message);
  const int UNENCSZ = offsetof (struct encrypted_message, server_salt);
  TGL_DEBUG("process_rpc_message(), len=" << len);
  if (len < MINSZ || (len & 15) != (UNENCSZ & 15)) {
    TGL_WARNING("Incorrect packet from server. Closing connection\n");
    fail_connection (c);
    return -1;
  }
  assert (len >= MINSZ && (len & 15) == (UNENCSZ & 15));
  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
  if (enc->auth_key_id != DC->temp_auth_key_id && enc->auth_key_id != DC->auth_key_id) {
    TGL_WARNING("received msg from dc " << DC->id << " with auth_key_id " << enc->auth_key_id <<
        " (perm_auth_key_id " << DC->auth_key_id << " temp_auth_key_id "<< DC->temp_auth_key_id << "). Dropping");
    return 0;
  }
  if (enc->auth_key_id == DC->temp_auth_key_id) {
    assert (enc->auth_key_id == DC->temp_auth_key_id);
    assert (DC->temp_auth_key_id);
    tgl_init_aes_auth (DC->temp_auth_key + 8, enc->msg_key, AES_DECRYPT);
  } else {
    assert (enc->auth_key_id == DC->auth_key_id);
    assert (DC->auth_key_id);
    tgl_init_aes_auth (DC->auth_key + 8, enc->msg_key, AES_DECRYPT);
  }

  int l = tgl_pad_aes_decrypt ((unsigned char *)&enc->server_salt, len - UNENCSZ, (unsigned char *)&enc->server_salt, len - UNENCSZ);
  assert (l == len - UNENCSZ);

  if (!(!(enc->msg_len & 3) && enc->msg_len > 0 && enc->msg_len <= len - MINSZ && len - MINSZ - enc->msg_len <= 12)) {
    TGL_WARNING("Incorrect packet from server. Closing connection\n");
    fail_connection(c);
    return -1;
  }
  assert (!(enc->msg_len & 3) && enc->msg_len > 0 && enc->msg_len <= len - MINSZ && len - MINSZ - enc->msg_len <= 12);

  struct tgl_session *S = tgl_state::instance()->net_methods->get_session (c);
  if (!S || S->session_id != enc->session_id) {
    TGL_WARNING("Message to bad session. Drop.\n");
    return 0;
  }

  static unsigned char sha1_buffer[20];
  TGLC_sha1 ((unsigned char *)&enc->server_salt, enc->msg_len + (MINSZ - UNENCSZ), sha1_buffer);
  if (memcmp (&enc->msg_key, sha1_buffer + 4, 16)) {
    TGL_WARNING("Incorrect packet from server. Closing connection\n");
    fail_connection(c);
    return -1;
  }
  assert (!memcmp (&enc->msg_key, sha1_buffer + 4, 16));

  int this_server_time = enc->msg_id >> 32LL;
  if (!S->received_messages) {
    DC->server_time_delta = this_server_time - get_utime (CLOCK_REALTIME);
    if (DC->server_time_udelta) {
      TGL_WARNING("adjusting CLOCK_MONOTONIC delta to " <<
          DC->server_time_udelta - this_server_time - get_utime (CLOCK_MONOTONIC));
    }
    DC->server_time_udelta = this_server_time - get_utime (CLOCK_MONOTONIC);
  }

  double st = get_server_time (DC);
  if (this_server_time < st - 300 || this_server_time > st + 30) {
    TGL_WARNING("bad msg time: salt = " << enc->server_salt << ", session_id = " << enc->session_id << ", msg_id = " << enc->msg_id
        << ", seq_no = " << enc->seq_no << ", st = " << st << ", now = " << get_utime (CLOCK_REALTIME));
    fail_session(S);
    return -1;
  }
  S->received_messages ++;

  if (DC->server_salt != enc->server_salt) {
    DC->server_salt = enc->server_salt;
  }

  assert (this_server_time >= st - 300 && this_server_time <= st + 30);
  //assert (enc->msg_id > server_last_msg_id && (enc->msg_id & 3) == 1);
  TGL_DEBUG("received mesage id " << enc->msg_id);
  //server_last_msg_id = enc->msg_id;

  //*(long long *)(longpoll_query + 3) = *(long long *)((char *)(&enc->msg_id) + 0x3c);
  //*(long long *)(longpoll_query + 5) = *(long long *)((char *)(&enc->msg_id) + 0x3c);

  assert (l >= (MINSZ - UNENCSZ) + 8);
  //assert (enc->message[0] == CODE_rpc_result && *(long long *)(enc->message + 1) == client_last_msg_id);

  in_ptr = enc->message;
  in_end = in_ptr + (enc->msg_len / 4);

  if (enc->msg_id & 1) {
    tgln_insert_msg_id(S, enc->msg_id);
  }
  assert (S->session_id == enc->session_id);

  if (rpc_execute_answer (c, enc->msg_id) < 0) {
    fail_session (S);
    return -1;
  }
  assert (in_ptr == in_end);
  return 0;
}

static int rpc_execute (struct connection *c, int op, int len) {
    struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);

    if (len >= MAX_RESPONSE_SIZE/* - 12*/ || len < 0/*12*/) {
        TGL_WARNING("answer too long, skipping. lengeth:" << len);
        return 0;
    }

    int Response_len = len;

    static char Response[MAX_RESPONSE_SIZE];
    TGL_DEBUG("Response_len = " << Response_len);
    assert (tgl_state::instance()->net_methods->read_in (c, Response, Response_len) == Response_len);

#if !defined(__MACH__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined (__CYGWIN__)
    //  setsockopt (c->fd, IPPROTO_TCP, TCP_QUICKACK, (int[]){0}, 4);
#endif
    int o = DC->state;
    //if (DC->flags & 1) { o = st_authorized;}
    if (o != st_authorized) {
        TGL_DEBUG(__func__ << ": state = " << o);
    }
    switch (o) {
    case st_reqpq_sent:
        process_respq_answer(c, Response/* + 8*/, Response_len/* - 12*/, 0);
        return 0;
    case st_reqdh_sent:
        process_dh_answer(c, Response/* + 8*/, Response_len/* - 12*/, 0);
        return 0;
    case st_client_dh_sent:
        process_auth_complete(c, Response/* + 8*/, Response_len/* - 12*/, 0);
        return 0;
    case st_reqpq_sent_temp:
        process_respq_answer(c, Response/* + 8*/, Response_len/* - 12*/, 1);
        return 0;
    case st_reqdh_sent_temp:
        process_dh_answer(c, Response/* + 8*/, Response_len/* - 12*/, 1);
        return 0;
    case st_client_dh_sent_temp:
        process_auth_complete(c, Response/* + 8*/, Response_len/* - 12*/, 1);
        return 0;
    case st_authorized:
        if (op < 0 && op >= -999) {
            //TGL_WARNING("Server error %d\n", op);
        } else {
            return process_rpc_message(c, (struct encrypted_message *)(Response/* + 8*/), Response_len/* - 12*/);
        }
        return 0;
    default:
        TGL_ERROR("fatal: cannot receive answer in state " << DC->state);
        exit (2);
    }

    return 0;
}

static void mpc_on_get_config (void *extra, int success) {
    assert (success);
    struct tgl_dc *DC = (struct tgl_dc *)extra;
    DC->flags |= TGLDCF_CONFIGURED;
}

static int tc_becomes_ready (struct connection *c) {
  TGL_NOTICE("outbound rpc connection from dc #" << tgl_state::instance()->net_methods->get_dc(c)->id << " became ready");
  //char byte = 0xef;
  //assert (tgl_state::instance()->net_methods->write_out (c, &byte, 1) == 1);
  //tgl_state::instance()->net_methods->flush_out (c);

  struct tgl_dc *DC = tgl_state::instance()->net_methods->get_dc (c);
  if (DC->flags & 1) { DC->state = st_authorized; }
  int o = DC->state;
  if (o == st_authorized && !tgl_state::instance()->pfs_enabled()) {
    DC->temp_auth_key_id = DC->auth_key_id;
    memcpy (DC->temp_auth_key, DC->auth_key, 256);
    DC->flags |= 2;
  }
  switch (o) {
  case st_init:
    send_req_pq_packet (c);
    break;
  case st_authorized:
    if (!(DC->flags & 2)) {
      assert (tgl_state::instance()->pfs_enabled());
      if (!DC->temp_auth_key_id) {
        assert (!DC->temp_auth_key_id);
        create_temp_auth_key (c);
      } else {
        bind_temp_auth_key (c);
      }
    } else if (!(DC->flags & TGLDCF_CONFIGURED)) {
      tgl_do_help_get_config_dc (DC, mpc_on_get_config, DC);
    }
    break;
  default:
    TGL_DEBUG("c_state = " << DC->state);
    DC->state = st_init; // previous connection was reset
    send_req_pq_packet (c);
    break;
  }
  return 0;
}

static int rpc_close (struct connection *c) {
  return tc_close (c, 0);
}

#define RANDSEED_PASSWORD_FILENAME     NULL
#define RANDSEED_PASSWORD_LENGTH       0
int tglmp_on_start () {
  tgl_prng_seed (RANDSEED_PASSWORD_FILENAME, RANDSEED_PASSWORD_LENGTH);

  int i;
  int ok = 0;
  for (i = 0; i < tgl_state::instance()->rsa_key_list.size(); i++) {
    char *key = tgl_state::instance()->rsa_key_list[i];
    if (!key) {
      /* This key was provided using 'tgl_set_rsa_key_direct'. */
      TGLC_rsa *rsa = tgl_state::instance()->rsa_key_loaded[i];
      assert (rsa);
      tgl_state::instance()->rsa_key_fingerprint[i] = tgl_do_compute_rsa_key_fingerprint (rsa);
      TGL_NOTICE("'direct' public key loaded successfully\n");
      ok = 1;
    } else {
      TGLC_rsa *res = rsa_load_public_key (key);
      if (!res) {
        TGL_WARNING("Can not load key " << key);
        tgl_state::instance()->rsa_key_loaded[i] = NULL;
      } else {
        ok = 1;
        tgl_state::instance()->rsa_key_loaded[i] = res;
        tgl_state::instance()->rsa_key_fingerprint[i] = tgl_do_compute_rsa_key_fingerprint (res);
      }
    }

  if (!ok) {
    TGL_ERROR("No public keys found\n");
    tgl_state::instance()->error = tstrdup ("No public keys found");
    tgl_state::instance()->error_code = ENOTCONN;
    return -1;
  }
  return 0;
}

void tgl_dc_authorize (struct tgl_dc *DC) {
  //c_state = 0;
  if (!DC->sessions[0]) {
    tglmp_dc_create_session (DC);
  }
  TGL_DEBUG("Starting authorization for DC #" << DC->id);
  //net_loop (0, auth_ok);
}

static int send_all_acks (struct tgl_session *S) {
  clear_packet ();
  out_int (CODE_msgs_ack);
  out_int (CODE_vector);
  out_int (S->ack_tree.size());
  while (S->ack_tree.begin() != S->ack_tree.end()) {
    auto it = std::min_element(std::begin(S->ack_tree), std::end(S->ack_tree));
    out_long (*it);
    S->ack_tree.erase(it);
  }
  tglmp_encrypt_send_message (S->c, packet_buffer, packet_ptr - packet_buffer, 0);
  return 0;
}

static void send_all_acks_gateway (void *arg) {
  send_all_acks((struct tgl_session*)arg);
}


void tgln_insert_msg_id (struct tgl_session *S, long long id) {
  if (S->ack_tree.empty()) {
    tgl_state::instance()->timer_methods->insert(S->ev, ACK_TIMEOUT);
  }
  for (auto it = S->ack_tree.begin(); it!=S->ack_tree.end(); it++) {
    if (*it == id) {
      return;
    }
  }
  S->ack_tree.push_back(id);
}

//extern struct tgl_dc *DC_list[];


<<<<<<< 7064ea2805712a7e727e45fc2e237ba016b7f8fc
static void regen_temp_key_gw (void *arg) {
  tglmp_regenerate_temp_auth_key((struct tgl_dc *)arg);
}

struct tgl_dc *tglmp_alloc_dc (int flags, int id, char *ip, int port) {
  //assert (!tgl_state::instance()->DC_list[id]);

  if (!tgl_state::instance()->DC_list[id]) {
    struct tgl_dc *DC = (struct tgl_dc *)talloc0(sizeof (struct tgl_dc));
    DC->id = id;
    tgl_state::instance()->DC_list[id] = DC;
    if (id > tgl_state::instance()->max_dc_num) {
      tgl_state::instance()->max_dc_num = id;
    }
    DC->ev = tgl_state::instance()->timer_methods->alloc (regen_temp_key_gw, DC);
    tgl_state::instance()->timer_methods->insert (DC->ev, 0);
  }

  struct tgl_dc *DC = tgl_state::instance()->DC_list[id];

  struct tgl_dc_option *O = DC->options[flags & 3];

  struct tgl_dc_option *O2 = O;
  while (O2) {
    if (!strcmp (O2->ip, ip)) {
      tfree_str (ip);
      return DC;
    }
    O2 = O2->next;
  }

  struct tgl_dc_option *T = (struct tgl_dc_option *)talloc0(sizeof (struct tgl_dc_option));
  T->ip = ip;
  T->port = port;
  T->next = O;
  DC->options[flags & 3] = T;


  return DC;
}

<<<<<<< 7064ea2805712a7e727e45fc2e237ba016b7f8fc
void tglmp_dc_create_session (struct tgl_dc *DC) {
  struct tgl_session *S = (struct tgl_session *)talloc0(sizeof (struct tgl_session));
  assert (TGLC_rand_pseudo_bytes ((unsigned char *) &S->session_id, 8) >= 0);
  S->dc = DC;
  //S->c = tgl_state::instance()->net_methods->create_connection (DC->ip, DC->port, S, DC, &mtproto_methods);

  create_session_connect (S);
  S->ev = tgl_state::instance()->timer_methods->alloc (send_all_acks_gateway, S);
  assert (!DC->sessions[0]);
  DC->sessions[0] = S;
}

void tgl_do_send_ping (struct connection *c) {
  int x[3];
  x[0] = CODE_ping;
  *(long long *)(x + 1) = rand () * (1ll << 32) + rand ();
  tglmp_encrypt_send_message (c, x, 3, 0);
}

void tgl_dc_iterator (void (*iterator)(struct tgl_dc *DC)) {
  int i;
  for (i = 0; i <= tgl_state::instance()->max_dc_num; i++) {
    iterator (tgl_state::instance()->DC_list[i]);
  }
}

void tgl_dc_iterator_ex (void (*iterator)(struct tgl_dc *DC, void *extra), void *extra) {
  int i;
  for (i = 0; i <= tgl_state::instance()->max_dc_num; i++) {
    iterator (tgl_state::instance()->DC_list[i], extra);
  }
}

void tglmp_regenerate_temp_auth_key (struct tgl_dc *DC) {
  DC->flags &= ~6;
  DC->temp_auth_key_id = 0;
  memset (DC->temp_auth_key, 0, 256);

  if (!DC->sessions[0]) {
    tgl_dc_authorize (DC);
    return;
  }

  struct tgl_session *S = DC->sessions[0];
  tglt_secure_random ((unsigned char*)&S->session_id, 8);
  S->seq_no = 0;

  tgl_state::instance()->timer_methods->remove (S->ev);
  S->ack_tree.clear();

  if (DC->state != st_authorized) {
    return;
  }

  if (!tgl_state::instance()->pfs_enabled()) {
    return;
  }

  if (S->c) {
    create_temp_auth_key (S->c);
  }
}

void tgls_free_session (struct tgl_session *S) {
  S->ack_tree.clear();
  if (S->ev) { tgl_state::instance()->timer_methods->free (S->ev); }
  if (S->c) {
    tgl_state::instance()->net_methods->free (S->c);
  }
  tfree (S);
}

void tgls_free_dc (struct tgl_dc *DC) {
    //if (DC->ip) { tfree_str (DC->ip); }

  struct tgl_session *S = DC->sessions[0];
  if (S) { tgls_free_session (S); }

  int i;
  for (i = 0; i < 4; i++) {
    struct tgl_dc_option *O = DC->options[i];
    while (O) {
      struct tgl_dc_option *N = O->next;
      tfree_str (O->ip);
      tfree (O, sizeof (*O));
      O = N;
    }
  }

  if (DC->ev) { tgl_state::instance()->timer_methods->free (DC->ev); }
  tfree (DC);
}

void tgls_free_pubkey () {
  int i;
  for (i = 0; i < tgl_state::instance()->rsa_key_loaded.size(); i++) {
    if (tgl_state::instance()->rsa_key_loaded[i]) {
      TGLC_rsa_free ((TGLC_rsa *)tgl_state::instance()->rsa_key_loaded[i]);
      tgl_state::instance()->rsa_key_loaded[i] = NULL;
    }
  }
}
