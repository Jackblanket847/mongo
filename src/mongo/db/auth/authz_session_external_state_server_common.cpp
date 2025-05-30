/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <mutex>

#include "mongo/db/auth/authz_session_external_state_server_common.h"
#include "mongo/db/auth/enable_localhost_auth_bypass_parameter_gen.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {

namespace {
std::once_flag checkShouldAllowLocalhostOnceFlag;
}  // namespace

// NOTE: we default _allowLocalhost to true under the assumption that _checkShouldAllowLocalhost
// will always be called before any calls to shouldAllowLocalhost.  If this is not the case,
// it could cause a security hole.
AuthzSessionExternalStateServerCommon::AuthzSessionExternalStateServerCommon(Client* client)
    : AuthzSessionExternalState(client), _allowLocalhost(enableLocalhostAuthBypass) {}
AuthzSessionExternalStateServerCommon::~AuthzSessionExternalStateServerCommon() {}

void AuthzSessionExternalStateServerCommon::_checkShouldAllowLocalhost(OperationContext* opCtx) {
    if (!AuthorizationManager::get(opCtx->getService())->isAuthEnabled())
        return;
    // If we know that an admin user exists, don't re-check.
    if (!_allowLocalhost)
        return;
    // Don't bother checking if we're not on a localhost connection
    if (!Client::getCurrent()->getIsLocalHostConnection()) {
        _allowLocalhost = false;
        return;
    }

    _allowLocalhost =
        !AuthorizationManager::get(opCtx->getService())->hasAnyPrivilegeDocuments(opCtx);
    if (_allowLocalhost) {
        std::call_once(checkShouldAllowLocalhostOnceFlag, []() {
            LOGV2(20248,
                  "note: no users configured in admin.system.users, allowing localhost "
                  "access");
        });
    }
}

bool AuthzSessionExternalStateServerCommon::serverIsArbiter() const {
    return false;
}

bool AuthzSessionExternalStateServerCommon::shouldAllowLocalhost() const {
    if (!haveClient()) {
        return false;
    }
    Client* client = Client::getCurrent();
    return _allowLocalhost && client->getIsLocalHostConnection();
}

bool AuthzSessionExternalStateServerCommon::shouldIgnoreAuthChecks() const {
    return !AuthorizationManager::get(_client->getService())->isAuthEnabled();
}

}  // namespace mongo
