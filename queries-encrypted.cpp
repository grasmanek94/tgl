
// This file will be included

#ifdef ENABLE_SECRET_CHAT
#include "tgl-layout.h"

/* {{{ Encrypt decrypted */
static int *encr_extra;
static int *encr_ptr;
static int *encr_end;

static void out_random (int n) {
    assert (n <= 32);
    static unsigned char buf[32];
    tglt_secure_random (buf, n);
    out_cstring ((char*)buf, n);
}

static char *encrypt_decrypted_message (struct tgl_secret_chat *E) {
  static int msg_key[4];
  static unsigned char sha1a_buffer[20];
  static unsigned char sha1b_buffer[20];
  static unsigned char sha1c_buffer[20];
  static unsigned char sha1d_buffer[20];
  int x = *(encr_ptr);  
  assert (x >= 0 && !(x & 3));
  TGLC_sha1 (reinterpret_cast<const unsigned char*>(encr_ptr), 4 + x, sha1a_buffer);
  memcpy (msg_key, sha1a_buffer + 4, 16);
 
  static unsigned char buf[64];
  memcpy (buf, msg_key, 16);
  memcpy (buf + 16, E->key, 32);
  TGLC_sha1 (buf, 48, sha1a_buffer);
  
  memcpy (buf, E->key + 8, 16);
  memcpy (buf + 16, msg_key, 16);
  memcpy (buf + 32, E->key + 12, 16);
  TGLC_sha1 (buf, 48, sha1b_buffer);
  
  memcpy (buf, E->key + 16, 32);
  memcpy (buf + 32, msg_key, 16);
  TGLC_sha1 (buf, 48, sha1c_buffer);
  
  memcpy (buf, msg_key, 16);
  memcpy (buf + 16, E->key + 24, 32);
  TGLC_sha1 (buf, 48, sha1d_buffer);

  static unsigned char key[32];
  memcpy (key, sha1a_buffer + 0, 8);
  memcpy (key + 8, sha1b_buffer + 8, 12);
  memcpy (key + 20, sha1c_buffer + 4, 12);

  static unsigned char iv[32];
  memcpy (iv, sha1a_buffer + 8, 12);
  memcpy (iv + 12, sha1b_buffer + 0, 8);
  memcpy (iv + 20, sha1c_buffer + 16, 4);
  memcpy (iv + 24, sha1d_buffer + 0, 8);

  TGLC_aes_key aes_key;
  TGLC_aes_set_encrypt_key (key, 256, &aes_key);
  TGLC_aes_ige_encrypt (reinterpret_cast<const unsigned char*>(encr_ptr), reinterpret_cast<unsigned char*>(encr_ptr), 4 * (encr_end - encr_ptr), &aes_key, iv, 1);
  memset (&aes_key, 0, sizeof (aes_key));

  return reinterpret_cast<char*>(msg_key);
}

static void encr_start (void) {
    encr_extra = packet_ptr;
    packet_ptr += 1; // str len
    packet_ptr += 2; // fingerprint
    packet_ptr += 4; // msg_key
    packet_ptr += 1; // len
}


static void encr_finish (struct tgl_secret_chat *E) {
    int l = packet_ptr - (encr_extra +  8);
    while (((packet_ptr - encr_extra) - 3) & 3) {
        int t;
        tglt_secure_random ((unsigned char*)&t, 4);
        out_int (t);
    }

    *encr_extra = ((packet_ptr - encr_extra) - 1) * 4 * 256 + 0xfe;
    encr_extra ++;
    *(long long *)encr_extra = E->key_fingerprint;
    encr_extra += 2;
    encr_extra[4] = l * 4;
    encr_ptr = encr_extra + 4;
    encr_end = packet_ptr;
    memcpy (encr_extra, encrypt_decrypted_message (E), 16);
}
/* }}} */

void tgl_do_send_encr_action (struct tgl_secret_chat *E, struct tl_ds_decrypted_message_action *A) {
#if 0 // FIXME
  long long t;
  tglt_secure_random ((unsigned char*)&t, 8);
  int date = time (0);

  struct tgl_message_id id = tgl_peer_id_to_random_msg_id (E->id);
  
  tgl_peer_id_t from_id = tgl_state::instance()->our_id();
  bl_do_edit_message_encr (tgl_state::instance(), &id, &from_id, &E->id, &date, NULL, 0, NULL, A, NULL, TGLMF_PENDING | TGLMF_OUT | TGLMF_UNREAD | TGLMF_CREATE | TGLMF_CREATED | TGLMF_ENCRYPTED);

  struct tgl_message *M = tgl_message_get (&id);
  assert (M);
  tgl_do_send_msg (M, 0, 0);
#endif
}

void tgl_do_send_encr_chat_layer (struct tgl_secret_chat *E) {
    static struct tl_ds_decrypted_message_action A;
    A.magic = CODE_decrypted_message_action_notify_layer;
    int layer = TGL_ENCRYPTED_LAYER;
    A.layer = &layer;

    tgl_do_send_encr_action (E, &A);
}

void tgl_do_set_encr_chat_ttl (struct tgl_secret_chat *E, int ttl) {
    static struct tl_ds_decrypted_message_action A;
    A.magic = CODE_decrypted_message_action_set_message_t_t_l;
    A.layer = &ttl;

    tgl_do_send_encr_action (E, &A);
}

/* {{{ Seng msg (plain text, encrypted) */
static int msg_send_encr_on_answer (std::shared_ptr<query> q, void *D) {
  struct tl_ds_updates *DS_U = (struct tl_ds_updates *)D;
  //struct tgl_message *M = (struct tgl_message*)q->extra.get();
  std::shared_ptr<msg_callback_extra> old_msg_id = std::static_pointer_cast<msg_callback_extra>(q->extra);
  if (old_msg_id) {
      tgl_state::instance()->callback()->message_sent(old_msg_id->old_msg_id, DS_LVAL(DS_U->id), old_msg_id->to_id);
  }
#if 0 // FXIME
  //assert (M->flags & TGLMF_ENCRYPTED);
  
  if (M->flags & TGLMF_PENDING) {
    bl_do_edit_message_encr (&M->permanent_id, NULL, NULL,  
      &M->date,
      NULL, 0, NULL, NULL, NULL, M->flags ^ TGLMF_PENDING);
    
    //bl_do_msg_update (&M->permanent_id);
  }

  if (q->callback) {
    ((void (*)(std::shared_ptr<void>, int, struct tgl_message *))q->callback) (q->callback_extra, 1, M);
  }
#endif
  return 0;
}

static int msg_send_encr_on_error (std::shared_ptr<query> q, int error_code, const std::string& error) {
  struct tgl_message *M = (struct tgl_message*)q->extra.get();
  std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(M->to_id);
  if (secret_chat && secret_chat->state != sc_deleted && error_code == 400) {
    if (strncmp (error.c_str(), "ENCRYPTION_DECLINED", 19) == 0) {
      //bl_do_peer_delete (tgl_state::instance(), secret_chat->id);
    }
  }
  if (q->callback) {
    ((void (*)(std::shared_ptr<void>, int, struct tgl_message *))q->callback) (q->callback_extra, 0, M);
  }
  if (M) {
    //bl_do_message_delete (&M->permanent_id);
    tgl_state::instance()->callback()->message_deleted(M->permanent_id.id);
  }
  return 0;
}

struct paramed_type msg_enc_type = TYPE_TO_PARAM(messages_sent_encrypted_message);
static struct query_methods msg_send_encr_methods = {
  .on_answer = msg_send_encr_on_answer,
  .on_error = msg_send_encr_on_error,
  .on_timeout = nullptr,
  .type = msg_enc_type,
  .name = "send encrypted (message)",
  .timeout = 0,
};
/* }}} */

void tgl_do_send_encr_msg_action (struct tgl_message *M, void (*callback)(std::shared_ptr<void> callback_extra, bool success, struct tgl_message *M), std::shared_ptr<void> callback_extra) {
  std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(M->to_id);
  if (!secret_chat || secret_chat->state != sc_ok) { 
    TGL_WARNING("Unknown encrypted chat");
    if (callback) {
      callback (callback_extra, 0, 0);
    }
    return;
  }
 
  assert (M->flags & TGLMF_ENCRYPTED);
  clear_packet ();
  out_int (CODE_messages_send_encrypted_service);
  out_int (CODE_input_encrypted_chat);
  out_int (M->permanent_id.peer_id);
  out_long (M->permanent_id.access_hash);
  out_long (M->permanent_id.id);
  encr_start ();
  out_int (CODE_decrypted_message_layer);
  out_random (15 + 4 * (rand () % 3));
  out_int (TGL_ENCRYPTED_LAYER);
  out_int (2 * secret_chat->in_seq_no + (secret_chat->admin_id != tgl_get_peer_id (tgl_state::instance()->our_id())));
  out_int (2 * secret_chat->out_seq_no + (secret_chat->admin_id == tgl_get_peer_id (tgl_state::instance()->our_id())) - 2);
  out_int (CODE_decrypted_message_service);
  out_long (M->permanent_id.id);

  switch (M->action.type) {
  case tgl_message_action_notify_layer:
    out_int (CODE_decrypted_message_action_notify_layer);
    out_int (M->action.layer);
    break;
  case tgl_message_action_set_message_ttl:
    out_int (CODE_decrypted_message_action_set_message_t_t_l);
    out_int (M->action.ttl);
    break;
  case tgl_message_action_request_key:
    out_int (CODE_decrypted_message_action_request_key);
    out_long (M->action.exchange_id);
    out_cstring (reinterpret_cast<char*>(M->action.g_a), 256);
    break;
  case tgl_message_action_accept_key:
    out_int (CODE_decrypted_message_action_accept_key);
    out_long (M->action.exchange_id);
    out_cstring (reinterpret_cast<char*>(M->action.g_a), 256);    
    out_long (M->action.key_fingerprint);
    break;
  case tgl_message_action_commit_key:
    out_int (CODE_decrypted_message_action_commit_key);
    out_long (M->action.exchange_id);
    out_long (M->action.key_fingerprint);
    break;
  case tgl_message_action_abort_key:
    out_int (CODE_decrypted_message_action_abort_key);
    out_long (M->action.exchange_id);
    break;
  case tgl_message_action_noop:
    out_int (CODE_decrypted_message_action_noop);
    break;
  default:
    assert (0);
  }
  encr_finish (secret_chat.get());
  
  std::shared_ptr<msg_callback_extra> extra = std::make_shared<msg_callback_extra>(M->permanent_id.id, tgl_get_peer_id(M->to_id));
  tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &msg_send_encr_methods, extra, (void*)callback, callback_extra);
}

void tgl_do_send_encr_msg (struct tgl_message *M, void (*callback)(std::shared_ptr<void> callback_extra, bool success, struct tgl_message *M), std::shared_ptr<void> callback_extra) {
  if (M->flags & TGLMF_SERVICE) {
    tgl_do_send_encr_msg_action (M, callback, callback_extra);
    return;
  }
  std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(M->to_id);
  if (!secret_chat || secret_chat->state != sc_ok) { 
    TGL_WARNING("Unknown encrypted chat");
    if (callback) {
      callback (callback_extra, 0, M);
    }
    return;
  }
  
  assert (M->flags & TGLMF_ENCRYPTED);

  clear_packet ();
  out_int (CODE_messages_send_encrypted);
  out_int (CODE_input_encrypted_chat);
  out_int (tgl_get_peer_id (M->to_id));
  out_long (secret_chat->access_hash);
  out_long (M->permanent_id.id);
  encr_start ();
  out_int (CODE_decrypted_message_layer);
  out_random (15 + 4 * (rand () % 3));
  out_int (TGL_ENCRYPTED_LAYER);
  out_int (2 * secret_chat->in_seq_no + (secret_chat->admin_id != tgl_get_peer_id (tgl_state::instance()->our_id())));
  out_int (2 * secret_chat->out_seq_no + (secret_chat->admin_id == tgl_get_peer_id (tgl_state::instance()->our_id())) - 2);
  out_int (CODE_decrypted_message);
  out_long (M->permanent_id.id);
  out_int (secret_chat->ttl);
  out_cstring (M->message, M->message_len);
  switch (M->media.type) {
  case tgl_message_media_none:
    out_int (CODE_decrypted_message_media_empty);
    break;
  case tgl_message_media_geo:
    out_int (CODE_decrypted_message_media_geo_point);
    out_double (M->media.geo.latitude);
    out_double (M->media.geo.longitude);
    break;
  default:
    assert (0);
  }
  encr_finish (secret_chat.get());
  
  std::shared_ptr<msg_callback_extra> extra = std::make_shared<msg_callback_extra>(M->permanent_id.id, tgl_get_peer_id(M->to_id));
  tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &msg_send_encr_methods, extra, (void*)callback, callback_extra);
}

static int mark_read_encr_on_receive (std::shared_ptr<query> q, void *D) {
    TGL_UNUSED(D);
    if (q->callback) {
        ((void (*)(std::shared_ptr<void>, int))q->callback)(q->callback_extra, 1);
    }
    return 0;
}

static int mark_read_encr_on_error (std::shared_ptr<query> q, int error_code, const std::string& error) {
#if 0
  tgl_peer_t *P = q->extra;
  if (P && P->encr_chat.state != sc_deleted && error_code == 400) {
    if (strncmp (error.c_str(), "ENCRYPTION_DECLINED", 19) == 0) {
      bl_do_peer_delete (P->id);
    }
  }
#endif
  return 0;
}

static struct query_methods mark_read_encr_methods = {
  .on_answer = mark_read_encr_on_receive,
  .on_error = mark_read_encr_on_error,
  .on_timeout = nullptr,
  .type = bool_type,
  .name = "read encrypted",
  .timeout = 0,
};

void tgl_do_messages_mark_read_encr (tgl_peer_id_t id, long long access_hash, int last_time, void (*callback)(std::shared_ptr<void> callback_extra, bool), std::shared_ptr<void> callback_extra) {
    clear_packet ();
    out_int (CODE_messages_read_encrypted_history);
    out_int (CODE_input_encrypted_chat);
    out_int (tgl_get_peer_id (id));
    out_long (access_hash);
    out_int (last_time);
    tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &mark_read_encr_methods, /*tgl_peer_get (id)*/0, (void*)callback, callback_extra);
}

static int send_encr_file_on_answer (std::shared_ptr<query> q, void *D) {
#if 0 // FIXME
  struct tl_ds_messages_sent_encrypted_message *DS_MSEM = (struct tl_ds_messages_sent_encrypted_message*)D; 
  struct tgl_message *M = q->extra;

  if (M->flags & TGLMF_PENDING) {
    //bl_do_edit_message_encr (&M->permanent_id, NULL, NULL, DS_MSEM->date, 
    //NULL, 0, NULL, NULL, DS_MSEM->file, M->flags ^ TGLMF_PENDING);   
    //bl_do_msg_update (&M->permanent_id);
    tgl_state::instance()->callback.new_msg(M);
  }
  
  if (q->callback) {
    ((void (*)(void *, int, struct tgl_message *))q->callback)(q->callback_extra, 1, M);
  }
#endif
  return 0;
}

struct paramed_type msg_send_enc_type = TYPE_TO_PARAM(messages_sent_encrypted_message);
static struct query_methods send_encr_file_methods = {
  .on_answer = send_encr_file_on_answer,
  .on_error = msg_send_encr_on_error,
  .on_timeout = nullptr,
  .type = msg_send_enc_type,
  .name = "send encrypted (file)",
  .timeout = 0,
};

void send_file_encrypted_end (std::shared_ptr<send_file> f, void *callback, std::shared_ptr<void> callback_extra) {
  out_int (CODE_messages_send_encrypted_file);
  out_int (CODE_input_encrypted_chat);
  out_int (tgl_get_peer_id (f->to_id));
  std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(f->to_id);
  assert(secret_chat);
  out_long (secret_chat->access_hash);
  long long r;
  tglt_secure_random (reinterpret_cast<unsigned char*>(&r), 8);
  out_long (r);
  encr_start ();
  out_int (CODE_decrypted_message_layer);
  out_random (15 + 4 * (rand () % 3));
  out_int (TGL_ENCRYPTED_LAYER);
  out_int (2 * secret_chat->in_seq_no + (secret_chat->admin_id != tgl_get_peer_id (tgl_state::instance()->our_id())));
  out_int (2 * secret_chat->out_seq_no + (secret_chat->admin_id == tgl_get_peer_id (tgl_state::instance()->our_id())));
  out_int (CODE_decrypted_message);
  out_long (r);
  out_int (secret_chat->ttl);
  out_string ("");
  int *save_ptr = packet_ptr;
  if (f->flags == -1) {
    out_int (CODE_decrypted_message_media_photo);
  } else if ((f->flags & TGLDF_VIDEO)) {
    out_int (CODE_decrypted_message_media_video);
  } else if ((f->flags & TGLDF_AUDIO)) {
    out_int (CODE_decrypted_message_media_audio);
  } else {
    out_int (CODE_decrypted_message_media_document);
  }
  if (f->flags == -1 || !(f->flags & TGLDF_AUDIO)) {
    out_cstring ("", 0);
    out_int (90);
    out_int (90);
  }
  
  if (f->flags == -1) {
    out_int (f->w);
    out_int (f->h);
  } else if (f->flags & TGLDF_VIDEO) {
    out_int (f->duration);
    out_string (tg_mime_by_filename (f->file_name.c_str()));
    out_int (f->w);
    out_int (f->h);
  } else if (f->flags & TGLDF_AUDIO) {
    out_int (f->duration);
    out_string (tg_mime_by_filename (f->file_name.c_str()));
  } else {
    out_string ("");
    out_string (tg_mime_by_filename (f->file_name.c_str()));
    // document
  }
  
  out_int (f->size);
  out_cstring (reinterpret_cast<const char*>(f->key), 32);
  out_cstring (reinterpret_cast<const char*>(f->init_iv), 32);
 
  int *save_in_ptr = in_ptr;
  int *save_in_end = in_end;

  in_ptr = save_ptr;
  in_end = packet_ptr;

  struct paramed_type decrypted_message_media = TYPE_TO_PARAM(decrypted_message_media);
  assert (skip_type_any (&decrypted_message_media) >= 0);
  assert (in_ptr == in_end);
  
  in_ptr = save_ptr;
  in_end = packet_ptr;
  
  //struct tl_ds_decrypted_message_media *DS_DMM = fetch_ds_type_decrypted_message_media (&decrypted_message_media);
  in_end = save_in_ptr;
  in_ptr = save_in_end;


  //int date = time (NULL);


  encr_finish (secret_chat.get());
  if (f->size < (16 << 20)) {
    out_int (CODE_input_encrypted_file_uploaded);
  } else {
    out_int (CODE_input_encrypted_file_big_uploaded);
  }
  out_long (f->id);
  out_int (f->part_num);
  if (f->size < (16 << 20)) {
    out_string ("");
  }

  unsigned char md5[16];
  unsigned char str[64];
  memcpy (str, f->key, 32);
  memcpy (str + 32, f->init_iv, 32);
  TGLC_md5 (str, 64, md5);
  out_int ((*(int *)md5) ^ (*(int *)(md5 + 4)));

  tfree_secure (f->iv, 32);
 
#if 0 // FXIME
  tgl_peer_id_t from_id = tgl_state::instance()->our_id();
  
  struct tgl_message_id id = tgl_peer_id_to_msg_id (P->id, r);
  bl_do_edit_message_encr (&id, &from_id, &f->to_id, &date, NULL, 0, DS_DMM, NULL, NULL, TGLMF_OUT | TGLMF_UNREAD | TGLMF_ENCRYPTED | TGLMF_CREATE | TGLMF_CREATED);

  free_ds_type_decrypted_message_media (DS_DMM, TYPE_TO_PARAM (decrypted_message_media));
  struct tgl_message *M = tgl_message_get (&id);
  assert (M);
      
  tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &send_encr_file_methods, M, callback, callback_extra);
#endif
}

void tgl_do_send_location_encr (tgl_peer_id_t peer_id, double latitude, double longitude, unsigned long long flags, void (*callback)(std::shared_ptr<void> callback_extra, bool success, struct tgl_message *M), std::shared_ptr<void> callback_extra) {
#if 0 // FIXME
  struct tl_ds_decrypted_message_media TDSM;
  TDSM.magic = CODE_decrypted_message_media_geo_point;
  TDSM.latitude = (double*)talloc (sizeof (double));
  *TDSM.latitude = latitude;
  TDSM.longitude = (double*)talloc (sizeof (double));
  *TDSM.longitude = longitude;
  
  int date = time (0);

  tgl_peer_id_t from_id = tgl_state::instance()->our_id();

  tgl_peer_t *P = tgl_peer_get (peer_id);
  
  struct tgl_message_id id = tgl_peer_id_to_random_msg_id (P->id);;
  bl_do_edit_message_encr (&id, &from_id, &peer_id, &date, NULL, 0, &TDSM, NULL, NULL, TGLMF_UNREAD | TGLMF_OUT | TGLMF_PENDING | TGLMF_CREATE | TGLMF_CREATED | TGLMF_ENCRYPTED);

  free (TDSM.latitude);
  free (TDSM.longitude);

  struct tgl_message *M = tgl_message_get (&id);

  tgl_do_send_encr_msg (M, callback, callback_extra);
#endif
}

/* {{{ Encr accept */
static int send_encr_accept_on_answer (std::shared_ptr<query> q, void *D) {
  std::shared_ptr<tgl_secret_chat> E = tglf_fetch_alloc_encrypted_chat ((struct tl_ds_encrypted_chat*)D);

  if (E->state == sc_ok) {
    tgl_do_send_encr_chat_layer (E.get());
  }
  if (q->callback) {
    ((void (*)(std::shared_ptr<void>, int, std::shared_ptr<tgl_secret_chat>))q->callback) (q->callback_extra, E->state == sc_ok, E);
  }
  return 0;
}

static int send_encr_request_on_answer (std::shared_ptr<query> q, void *D) {
  std::shared_ptr<tgl_secret_chat> E = tglf_fetch_alloc_encrypted_chat ((struct tl_ds_encrypted_chat*)D);
  
  if (q->callback) {
    ((void (*)(std::shared_ptr<void>, int, std::shared_ptr<tgl_secret_chat>))q->callback) (q->callback_extra, E->state != sc_deleted, E);
  }
  return 0;
}

static int encr_accept_on_error (std::shared_ptr<query> q, int error_code, const std::string& error) {
#if 0 // FIXME
  tgl_peer_t *P = (tgl_peer_t *)q->extra;
  if (P && P->encr_chat.state != sc_deleted &&  error_code == 400) {
    if (strncmp (error, "ENCRYPTION_DECLINED", 19) == 0) {
      bl_do_peer_delete (P->id);
    }
  }
  if (q->callback) {
    ((void (*)(void *, int, struct tgl_secret_chat *))q->callback) (q->callback_extra, 0, NULL);
  }
#endif
  return 0;
}

struct paramed_type enc_chat_type = TYPE_TO_PARAM(encrypted_chat);
static struct query_methods send_encr_accept_methods  = {
  .on_answer = send_encr_accept_on_answer,
  .on_error = encr_accept_on_error,
  .on_timeout = nullptr,
  .type = enc_chat_type,
  .name = "send encrypted (chat accept)",
  .timeout = 0,
};

static struct query_methods send_encr_request_methods  = {
  .on_answer = send_encr_request_on_answer,
  .on_error = q_ptr_on_error,
  .on_timeout = nullptr,
  .type = enc_chat_type,
  .name = "send encrypted (chat request)",
  .timeout = 0,
};

//int encr_root;
//unsigned char *encr_prime;
//int encr_param_version;
//static TGLC_bn_ctx *ctx;

void tgl_do_send_accept_encr_chat (std::shared_ptr<void> x, unsigned char *random, void (*callback)(std::shared_ptr<void> callback_extra, bool success, std::shared_ptr<tgl_secret_chat> E), std::shared_ptr<void> callback_extra) {
  int i;
  int ok = 0;
  auto E = std::static_pointer_cast<tgl_secret_chat>(x);
  for (i = 0; i < 64; i++) {
    if (E->key[i]) {
      ok = 1;
      break;
    }
  }
  if (ok) { 
    if (callback) {
      callback (callback_extra, 1, E);
    }
    return; 
  } // Already generated key for this chat
  assert (!E->g_key.empty());
  assert (tgl_state::instance()->bn_ctx);
  unsigned char random_here[256];
  tglt_secure_random (random_here, 256);
  for (i = 0; i < 256; i++) {
    random[i] ^= random_here[i];
  }
  TGLC_bn *b = TGLC_bn_bin2bn (random, 256, 0);
  ensure_ptr (b);
  TGLC_bn *g_a = TGLC_bn_bin2bn (E->g_key.data(), 256, 0);
  ensure_ptr (g_a);
  assert (tglmp_check_g_a (tgl_state::instance()->encr_prime_bn, g_a) >= 0);
  //if (!ctx) {
  //  ctx = TGLC_bn_ctx_new ();
  //  ensure_ptr (ctx);
  //}
  TGLC_bn *p = tgl_state::instance()->encr_prime_bn;
  TGLC_bn *r = TGLC_bn_new ();
  ensure_ptr (r);
  ensure (TGLC_bn_mod_exp (r, g_a, b, p, tgl_state::instance()->bn_ctx));
  static unsigned char kk[256];
  memset (kk, 0, sizeof (kk));
  TGLC_bn_bn2bin (r, kk + (256 - TGLC_bn_num_bytes (r)));
  static unsigned char sha_buffer[20];
  TGLC_sha1 (kk, 256, sha_buffer);

#if 0 // FIXME
  long long fingerprint = *(long long *)(sha_buffer + 12);

  //bl_do_encr_chat_set_key (E, kk, *(long long *)(sha_buffer + 12));
  //bl_do_encr_chat_set_sha (E, sha_buffer);

  int state = sc_ok;

  bl_do_encr_chat (tgl_state::instance(),
    tgl_get_peer_id (E->id), 
    NULL, NULL, NULL, NULL, 
    kk, NULL, sha_buffer, &state, 
    NULL, NULL, NULL, NULL, NULL, 
    &fingerprint, 
    TGL_FLAGS_UNCHANGED,
    NULL, 0
  );
#endif

  clear_packet ();
  out_int (CODE_messages_accept_encryption);
  out_int (CODE_input_encrypted_chat);
  out_int (tgl_get_peer_id (E->id));
  out_long (E->access_hash);
  
  ensure (TGLC_bn_set_word (g_a, tgl_state::instance()->encr_root));
  ensure (TGLC_bn_mod_exp (r, g_a, b, p, tgl_state::instance()->bn_ctx));
  static unsigned char buf[256];
  memset (buf, 0, sizeof (buf));
  TGLC_bn_bn2bin (r, buf + (256 - TGLC_bn_num_bytes (r)));
  out_cstring (reinterpret_cast<const char*>(buf), 256);

  out_long (E->key_fingerprint);
  TGLC_bn_clear_free (b);
  TGLC_bn_clear_free (g_a);
  TGLC_bn_clear_free (r);

  tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &send_encr_accept_methods, E, (void*)callback, callback_extra);
}

void tgl_do_create_keys_end (struct tgl_secret_chat *U) {
  assert (tgl_state::instance()->encr_prime);
  TGLC_bn *g_b = TGLC_bn_bin2bn (U->g_key.data(), 256, 0);
  ensure_ptr (g_b);
  assert (tglmp_check_g_a (tgl_state::instance()->encr_prime_bn, g_b) >= 0);
  
  TGLC_bn *p = tgl_state::instance()->encr_prime_bn;
  ensure_ptr (p);
  TGLC_bn *r = TGLC_bn_new ();
  ensure_ptr (r);
  TGLC_bn *a = TGLC_bn_bin2bn (reinterpret_cast<const unsigned char*>(U->key), 256, 0);
  ensure_ptr (a);
  ensure (TGLC_bn_mod_exp (r, g_b, a, p, tgl_state::instance()->bn_ctx));

  unsigned char *t = (unsigned char*)talloc (256);
  memcpy (t, U->key, 256);
  
  memset (U->key, 0, sizeof (U->key));
  TGLC_bn_bn2bin (r, ((reinterpret_cast<unsigned char*>(U->key)) + (256 - TGLC_bn_num_bytes (r))));
  
  static unsigned char sha_buffer[20];
  TGLC_sha1 (reinterpret_cast<const unsigned char*>(U->key), 256, sha_buffer);
  long long k = *(long long *)(sha_buffer + 12);
  if (k != U->key_fingerprint) {
    TGL_WARNING("Key fingerprint mismatch (my 0x" << std::hex << (unsigned long long)k << "x 0x" << (unsigned long long)U->key_fingerprint << "x)");
    U->state = sc_deleted;
  }

  memcpy (U->first_key_sha, sha_buffer, 20);
  tfree_secure (t, 256);
  
  TGLC_bn_clear_free (g_b);
  TGLC_bn_clear_free (r);
  TGLC_bn_clear_free (a);
}

void tgl_do_send_create_encr_chat(std::shared_ptr<void> x, unsigned char *random, void (*callback)(std::shared_ptr<void> callback_extra, bool success, std::shared_ptr<tgl_secret_chat> E), std::shared_ptr<void> callback_extra) {
  //int user_id = (long)x;
  auto user_id = std::static_pointer_cast<tgl_peer_id_t>(x);
  int i;
  unsigned char random_here[256];
  tglt_secure_random (random_here, 256);
  for (i = 0; i < 256; i++) {
    random[i] ^= random_here[i];
  }
  TGLC_bn *a = TGLC_bn_bin2bn (random, 256, 0);
  ensure_ptr (a);
  TGLC_bn *p = TGLC_bn_bin2bn (tgl_state::instance()->encr_prime, 256, 0);
  ensure_ptr (p);
 
  TGLC_bn *g = TGLC_bn_new ();
  ensure_ptr (g);

  ensure (TGLC_bn_set_word (g, tgl_state::instance()->encr_root));

  TGLC_bn *r = TGLC_bn_new ();
  ensure_ptr (r);

  ensure (TGLC_bn_mod_exp (r, g, a, p, tgl_state::instance()->bn_ctx));

  TGLC_bn_clear_free (a);

  static char g_a[256];
  memset (g_a, 0, 256);

  TGLC_bn_bn2bin (r, reinterpret_cast<unsigned char*>(g_a + (256 - TGLC_bn_num_bytes (r))));
  
  int t = rand ();
  while (tgl_state::instance()->secret_chat_for_id(TGL_MK_ENCR_CHAT (t))) {
    t = rand ();
  }

  //bl_do_encr_chat_init (t, user_id, (void *)random, (void *)g_a);
  
#if 0 // FIXME
  int state = sc_waiting;
  int our_id = tgl_get_peer_id (tgl_state::instance()->our_id());
  bl_do_encr_chat (tgl_state::instance(), t, NULL, NULL, &our_id, &user_id->peer_id, random, NULL, NULL, &state, NULL, NULL, NULL, NULL, NULL, NULL, TGLPF_CREATE | TGLPF_CREATED, NULL, 0);
#endif

  
  std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(TGL_MK_ENCR_CHAT (t));
  assert(secret_chat);
  
  clear_packet ();
  out_int (CODE_messages_request_encryption);
  //tgl_peer_t *U = tgl_peer_get (TGL_MK_USER (secret_chat->user_id));
  //assert (U);
  
  out_int (CODE_input_user);
  out_int (secret_chat->user_id);
  //out_long (U ? U->user.access_hash : 0);
  out_long(user_id->access_hash);

  out_int (tgl_get_peer_id (secret_chat->id));
  out_cstring (g_a, 256);
  //write_secret_chat_file ();
  
  TGLC_bn_clear_free (g);
  TGLC_bn_clear_free (p);
  TGLC_bn_clear_free (r);

  tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &send_encr_request_methods, secret_chat, (void*)callback, callback_extra);
}

static int send_encr_discard_on_answer (std::shared_ptr<query> q, void *D) {
  TGL_UNUSED(D);
  std::shared_ptr<tgl_secret_chat> E = std::static_pointer_cast<tgl_secret_chat>(q->extra);

  // FIXME
  //bl_do_peer_delete(tgl_state::instance(), E->id);

  if (q->callback) {
    ((void (*)(std::shared_ptr<void>, bool, std::shared_ptr<tgl_secret_chat>))q->callback)(q->callback_extra, true, E);
  }
  return 0;
}

static struct query_methods send_encr_discard_methods  = {
  .on_answer = send_encr_discard_on_answer,
  .on_error = q_ptr_on_error,
  .on_timeout = nullptr,
  .type = TYPE_TO_PARAM(bool),
  .name = "send encrypted (chat discard)",
  .timeout = 0,
};

void tgl_do_discard_secret_chat (std::shared_ptr<tgl_secret_chat> E, void (*callback)(std::shared_ptr<void> callback_extra, bool success, std::shared_ptr<tgl_secret_chat> E), std::shared_ptr<void> callback_extra) {
  assert (E);
  assert (tgl_get_peer_id (E->id) > 0);

  if (E->state == sc_deleted || E->state == sc_none) {
    if (callback) {
      callback (callback_extra, 0, E);
    }
    return;
  }

  clear_packet ();
  out_int (CODE_messages_discard_encryption);
  out_int (tgl_get_peer_id (E->id));

  tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &send_encr_discard_methods, E, (void*)callback, callback_extra);
}

struct encr_chat_extra {
   std::function<void(std::shared_ptr<void>, unsigned char *random, void (*callback)(std::shared_ptr<void> callback_extra, bool success, std::shared_ptr<tgl_secret_chat> E), std::shared_ptr<void> callback_extra)> callback;
   std::shared_ptr<void> callback_extra;
};

static int get_dh_config_on_answer (std::shared_ptr<query> q, void *D) {
    struct tl_ds_messages_dh_config *DS_MDC = (struct tl_ds_messages_dh_config *)D;

    if (DS_MDC->magic == CODE_messages_dh_config) {
        assert (DS_MDC->p->len == 256);
        // FIXME
        //bl_do_set_dh_params(tgl_state::instance(), DS_LVAL (DS_MDC->g), (unsigned char *)DS_MDC->p->data, DS_LVAL (DS_MDC->version));
    } else {
        assert (tgl_state::instance()->encr_param_version);
    }
    unsigned char *random = (unsigned char *)malloc (256);
    assert (DS_MDC->random->len == 256);
    memcpy (random, DS_MDC->random->data, 256);

    if (q->extra) {
        auto extra = std::static_pointer_cast<encr_chat_extra>(q->extra);
        extra->callback(extra->callback_extra, random, (void(*)(std::shared_ptr<void>, bool, std::shared_ptr<tgl_secret_chat>))q->callback, q->callback_extra);
        free (random);
    } else {
        free (random);
    }
    return 0;
}

struct paramed_type dh_config_type = TYPE_TO_PARAM(messages_dh_config);
static struct query_methods get_dh_config_methods  = {
  .on_answer = get_dh_config_on_answer,
  .on_error = q_void_on_error,
  .on_timeout = nullptr,
  .type = dh_config_type,
  .name = "dh config",
  .timeout = 0,
};

void tgl_do_accept_encr_chat_request(std::shared_ptr<tgl_secret_chat> E, void (*callback)(std::shared_ptr<void> callback_extra, bool success, std::shared_ptr<tgl_secret_chat> E), std::shared_ptr<void> callback_extra) {
    if (E->state != sc_request) {
        if (callback) {
            callback (callback_extra, 0, E);
        }
        return;
    }
    assert (E->state == sc_request);

    clear_packet ();
    out_int (CODE_messages_get_dh_config);
    out_int (tgl_state::instance()->encr_param_version);
    out_int (256);
    auto extra = std::make_shared<encr_chat_extra>();
    extra->callback = tgl_do_send_accept_encr_chat;
    extra->callback_extra = E;
    tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &get_dh_config_methods, extra, (void*)callback, callback_extra);
}

void tgl_do_create_encr_chat_request(const tgl_peer_id_t& user_id, void (*callback)(std::shared_ptr<void> callback_extra, bool success, std::shared_ptr<tgl_secret_chat> E), std::shared_ptr<void> callback_extra) {
    clear_packet ();
    out_int (CODE_messages_get_dh_config);
    out_int (tgl_state::instance()->encr_param_version);
    out_int (256);
    auto extra = std::make_shared<encr_chat_extra>();
    extra->callback = tgl_do_send_create_encr_chat;
    auto user_id_copy = std::make_shared<tgl_peer_id_t>();
    *user_id_copy = user_id;
    extra->callback_extra = user_id_copy;
    tglq_send_query (tgl_state::instance()->DC_working, packet_ptr - packet_buffer, packet_buffer, &get_dh_config_methods, extra, (void*)callback, callback_extra);
}
/* }}} */
#endif