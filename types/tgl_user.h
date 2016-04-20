#ifndef __TGL_USER_H__
#define __TGL_USER_H__

#include "tgl_file_location.h"
#include "tgl_peer_id.h"

#include <memory>
#include <string>

class tgl_timer;
struct tgl_message;

enum class tgl_user_online_status: int {
    unknown = 0,
    online = 1,
    offline = 2,
    recent = 3,
    last_week = 4,
    last_month
};

struct tgl_user_status {
    tgl_user_online_status online;
    int when;
    tgl_user_status(): online(tgl_user_online_status::unknown), when(0) { }
};

struct tgl_user {
    tgl_peer_id_t id;
    int flags;
    long long access_hash;
    struct tgl_user_status status;
    tgl_user(): id({0, 0, 0}), flags(0) { }
};

#endif
