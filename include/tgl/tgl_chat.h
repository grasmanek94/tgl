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

    Copyright Vitaly Valtman 2013-2015
    Copyright Topology LP 2016
*/

#pragma once

#include <cstdint>
#include <string>

#include "tgl_file_location.h"
#include "tgl_peer_id.h"

struct tgl_chat_user {
    int32_t user_id = 0;
    int32_t inviter_id = 0;
    int64_t date = 0;
};

struct tgl_chat_participant{
    int32_t user_id = 0;
    int32_t inviter_id = 0;
    int64_t date = 0;
    bool is_admin = false;
    bool is_creator = false;
};

class tgl_chat
{
public:
    virtual ~tgl_chat() { }
    virtual const tgl_input_peer_t& id() const = 0;
    virtual int64_t date() const = 0;
    virtual int32_t participants_count() const = 0;
    virtual bool is_creator() const = 0;
    virtual bool is_kicked() const = 0;
    virtual bool is_left() const = 0;
    virtual bool is_admins_enabled() const = 0;
    virtual bool is_deactivated() const = 0;
    virtual bool is_admin() const = 0;
    virtual bool is_editor() const = 0;
    virtual bool is_moderator() const = 0;
    virtual bool is_verified() const = 0;
    virtual bool is_mega_group() const = 0;
    virtual bool is_restricted() const = 0;
    virtual bool is_forbidden() const = 0;
    virtual const std::string& title() const = 0;
    virtual const std::string& user_name() const = 0;
    virtual const tgl_file_location& photo_big() const = 0;
    virtual const tgl_file_location& photo_small() const = 0;
};
