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
    Copyright Topology LP 2016-2017
*/

#pragma once

#include "query.h"
#include "tgl/tgl_log.h"

#include <functional>
#include <string>

namespace tgl {
namespace impl {

class query_check_username: public query
{
public:
    query_check_username(user_agent& ua, const std::function<void(int)>& callback)
        : query(ua, "check username", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        auto value = static_cast<tl_ds_bool*>(D);
        if (m_callback) {
            // 0: user name valid and available
            // 1: user name is already taken
            m_callback(value->magic == CODE_bool_true ? 0 : 1);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            if (error_code == 400) {
                // user name invalid
                m_callback(2);
            } else if (error_code == 600) {
                // not connected
                m_callback(3);
            }
        }
        return 0;
    }

private:
    std::function<void(int)> m_callback;
};

}
}
