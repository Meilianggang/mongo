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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/current_op_common.h"

#include <tuple>
#include <vector>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/s/query/cluster_aggregate.h"

namespace mongo {
namespace {

class ClusterCurrentOpCommand final : public CurrentOpCommandBase {
    ClusterCurrentOpCommand(const ClusterCurrentOpCommand&) = delete;
    ClusterCurrentOpCommand& operator=(const ClusterCurrentOpCommand&) = delete;

public:
    ClusterCurrentOpCommand() = default;

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const final {
        bool isAuthorized =
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::inprog);

        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

private:
    virtual void modifyPipeline(std::vector<BSONObj>* pipeline) const final {
        BSONObjBuilder sortBuilder;

        BSONObjBuilder sortSpecBuilder(sortBuilder.subobjStart("$sort"));
        sortSpecBuilder.append("shard", 1);
        sortSpecBuilder.doneFast();

        pipeline->push_back(sortBuilder.obj());
    }

    virtual StatusWith<CursorResponse> runAggregation(
        OperationContext* opCtx, AggregateCommandRequest& request) const final {
        auto nss = request.getNamespace();

        BSONObjBuilder responseBuilder;

        auto status = ClusterAggregate::runAggregate(
            opCtx,
            ClusterAggregate::Namespaces{nss, nss},
            request,
            {request},
            {Privilege(ResourcePattern::forClusterResource(nss.tenantId()), ActionType::inprog)},
            &responseBuilder);

        if (!status.isOK()) {
            return status;
        }

        CommandHelpers::appendSimpleCommandStatus(responseBuilder, true);

        return CursorResponse::parseFromBSON(responseBuilder.obj());
    }

} clusterCurrentOpCmd;

}  // namespace
}  // namespace mongo
