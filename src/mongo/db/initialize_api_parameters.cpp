/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <memory>
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/initialize_api_parameters.h"
#include "mongo/db/operation_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

namespace mongo {

APIParametersFromClient initializeAPIParameters(const BSONObj& requestBody, Command* command) {
    auto apiParamsFromClient =
        APIParametersFromClient::parse(IDLParserContext{"APIParametersFromClient"}, requestBody);

    if (command->skipApiVersionCheck()) {
        return apiParamsFromClient;
    }

    if (apiParamsFromClient.getApiDeprecationErrors() || apiParamsFromClient.getApiStrict()) {
        uassert(4886600,
                "Provided apiStrict and/or apiDeprecationErrors without passing apiVersion",
                apiParamsFromClient.getApiVersion());
    }

    if (apiParamsFromClient.getApiVersion()) {
        auto apiVersionFromClient = apiParamsFromClient.getApiVersion().value();
        if (apiVersionFromClient == "2") {
            uassert(ErrorCodes::APIVersionError, "Cannot accept API version 2", acceptApiVersion2);
        } else {
            uassert(ErrorCodes::APIVersionError,
                    "API version must be \"1\"",
                    "1" == apiVersionFromClient);
        }
    }

    if (apiParamsFromClient.getApiStrict().get_value_or(false)) {
        auto cmdApiVersions = command->apiVersions();
        auto apiVersionFromClient = apiParamsFromClient.getApiVersion().value().toString();
        bool strictAssert = (cmdApiVersions.find(apiVersionFromClient) != cmdApiVersions.end());
        uassert(
            ErrorCodes::APIStrictError,
            str::stream() << "Provided apiStrict:true, but the command " << command->getName()
                          << " is not in API Version " << apiVersionFromClient
                          << ". Information on supported commands and migrations in API Version "
                          << apiVersionFromClient
                          << " can be found at "
                             "https://dochub.mongodb.org/core/manual-versioned-api",
            strictAssert);
        bool strictDoesntWriteToSystemJS =
            !(command->getReadWriteType() == BasicCommand::ReadWriteType::kWrite &&
              requestBody.firstElementType() == BSONType::String &&
              requestBody.firstElement().String() == "system.js");
        uassert(ErrorCodes::APIStrictError,
                str::stream() << "Provided apiStrict:true, but the command " << command->getName()
                              << " attempts to write to system.js",
                strictDoesntWriteToSystemJS);
    }

    if (apiParamsFromClient.getApiDeprecationErrors().get_value_or(false)) {
        auto cmdDepApiVersions = command->deprecatedApiVersions();
        auto apiVersionFromClient = apiParamsFromClient.getApiVersion().value().toString();

        bool deprecationAssert =
            (cmdDepApiVersions.find(apiVersionFromClient) == cmdDepApiVersions.end());
        uassert(ErrorCodes::APIDeprecationError,
                str::stream() << "Provided apiDeprecationErrors:true, but the command "
                              << command->getName() << " is deprecated in API Version "
                              << apiVersionFromClient,
                deprecationAssert);
    }

    return apiParamsFromClient;
}

void enforceRequireAPIVersion(OperationContext* opCtx, Command* command) {
    auto client = opCtx->getClient();
    auto isInternalClient =
        !client->session() || (client->session()->getTags() & transport::Session::kInternalClient);

    if (gRequireApiVersion.load() && !opCtx->getClient()->isInDirectClient() && !isInternalClient) {
        uassert(
            498870,
            "The apiVersion parameter is required, please configure your MongoClient's API version",
            APIParameters::get(opCtx).getParamsPassed());
    }
}
}  // namespace mongo
