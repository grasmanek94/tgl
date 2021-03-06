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

    Copyright Vitaly Valtman 2014-2015
    Copyright Topology LP 2016
*/

#pragma once

#include <array>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

class tgl_dc {
public:
    virtual int32_t id() const = 0;
    virtual bool is_logged_in() const = 0;
    virtual const std::vector<std::pair<std::string, int>>& ipv4_options() const = 0;
    virtual const std::vector<std::pair<std::string, int>>& ipv6_options() const = 0;
    virtual int64_t auth_key_id() const = 0;
    virtual const std::array<unsigned char, 256>& auth_key() const = 0;
    // UNIX time difference between the server and the local client. Basically it returns server_time - local_time.
    virtual double time_difference() const = 0;
    virtual ~tgl_dc() { }
};
