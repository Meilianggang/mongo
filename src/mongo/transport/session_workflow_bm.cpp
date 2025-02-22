/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <chrono>

#include <benchmark/benchmark.h>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/mock_service_executor.h"
#include "mongo/transport/service_entry_point_impl.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/processinfo.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {
namespace {

/** For troubleshooting the benchmark. */
constexpr bool enableInstrumentation = false;

/** Benchmarks can't do this with command line flags like unit tests can. */
void initializeInstrumentation() {
    constexpr auto kLogLevel =
        enableInstrumentation ? logv2::LogSeverity::Debug(4) : logv2::LogSeverity::Error();
    std::array components = {
        std::pair{logv2::LogComponent::kExecutor, kLogLevel},
        std::pair{logv2::LogComponent::kNetwork, kLogLevel},
    };
    for (auto&& [comp, sev] : components)
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(comp, sev);
    for (auto&& [comp, sev] : components)
        invariant(logv2::shouldLog(comp, sev));
}

Status makeClosedSessionError() {
    return Status{ErrorCodes::SocketException, "Session is closed"};
}

class NoopReactor : public Reactor {
public:
    void run() noexcept override {}
    void stop() override {}

    void runFor(Milliseconds time) noexcept override {
        MONGO_UNREACHABLE;
    }

    void drain() override {
        MONGO_UNREACHABLE;
    }

    void schedule(Task) override {
        MONGO_UNREACHABLE;
    }

    void dispatch(Task) override {
        MONGO_UNREACHABLE;
    }

    bool onReactorThread() const override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ReactorTimer> makeTimer() override {
        MONGO_UNREACHABLE;
    }

    Date_t now() override {
        MONGO_UNREACHABLE;
    }

    void appendStats(BSONObjBuilder&) const {
        MONGO_UNREACHABLE;
    }
};

class TransportLayerMockWithReactor : public TransportLayerMock {
public:
    ReactorHandle getReactor(WhichReactor) override {
        return _mockReactor;
    }

private:
    ReactorHandle _mockReactor = std::make_unique<NoopReactor>();
};

/**
 * Coordinate between a mock Session and ServiceEntryPoint to implement
 * a prescribed number of exhaust rounds.
 */
class MockCoordinator {
public:
    MockCoordinator(ServiceContext* sc, int rounds) : _sc{sc}, _rounds{rounds} {}

    class Session : public CallbackMockSession {
    public:
        explicit Session(MockCoordinator* mc) : _mc{mc} {
            LOGV2_DEBUG(7015130, 3, "MockCoordinator::Session ctor");
        }
        ~Session() {
            LOGV2_DEBUG(7015131, 3, "MockCoordinator::Session dtor");
        }

        void end() override {
            _observeEnd.promise.emplaceValue();
        }
        Status waitForData() noexcept override {
            return Status::OK();
        }
        Status sinkMessage(Message) noexcept override {
            return Status::OK();
        }
        Future<void> asyncWaitForData() noexcept override {
            return {};
        }
        StatusWith<Message> sourceMessage() noexcept override {
            LOGV2_DEBUG(7015132, 3, "sourceMessage", "rounds"_attr = _rounds);
            if (!_rounds)
                return makeClosedSessionError();
            return _request;
        }

        /** Return a future that is ready when this session is ended. */
        Future<void> observeEnd() {
            return std::move(_observeEnd.future);
        }

        int& rounds() {
            return _rounds;
        }

    private:
        static Message _makeRequest() {
            Message request = [&] {
                OpMsgBuilder builder;
                builder.beginBody().append("ping", 1);
                return builder.finish();
            }();
            OpMsg::setFlag(&request, OpMsg::kExhaustSupported);
            return request;
        }

        MockCoordinator* _mc;
        Message _request = _makeRequest();
        int _rounds = _mc->_rounds;
        PromiseAndFuture<void> _observeEnd;
    };

    class Sep : public MockServiceEntryPoint {
    public:
        explicit Sep(MockCoordinator* mc) : MockServiceEntryPoint{mc->_sc}, _mc{mc} {}

        void derivedOnClientDisconnect(Client*) override {}

        void onEndSession(const std::shared_ptr<transport::Session>& session) override {}

        Future<DbResponse> handleRequest(OperationContext* opCtx,
                                         const Message& request) noexcept override {
            DbResponse response;
            response.response = request;

            auto session = _mc->opCtxToSession(opCtx);
            invariant(session);
            if (int& rounds = session->rounds(); --rounds) {
                response.nextInvocation = BSONObjBuilder{}.append("ping", 1).obj();
                response.shouldRunAgainForExhaust = true;
            }
            return Future{std::move(response)};
        }

    private:
        MockCoordinator* _mc;
    };

    Session* opCtxToSession(OperationContext* opCtx) const {
        return dynamic_cast<Session*>(opCtx->getClient()->session().get());
    }


    std::shared_ptr<Session> makeSession() {
        return std::make_shared<Session>(this);
    }

    std::unique_ptr<Sep> makeServiceEntryPoint() {
        auto p = std::make_unique<Sep>(this);
        _sep = &*p;
        return p;
    }

    Sep* serviceEntryPoint() {
        return _sep;
    }

private:
    ServiceContext* _sc;
    int _rounds = 0;
    Sep* _sep = nullptr;
};

class SessionWorkflowBm : public benchmark::Fixture {
public:
    SessionWorkflowBm() {
        initializeInstrumentation();
        LOGV2_DEBUG(7015133, 3, "SessionWorkflowBm ctor");
    }

    void SetUp(benchmark::State& state) override {
        stdx::lock_guard lk{_setupMutex};
        LOGV2_DEBUG(7015134, 3, "SetUp", "configuredThreads"_attr = _configuredThreads);
        if (_configuredThreads++)
            return;

        size_t argIndex = 0;
        int exhaustRounds = state.range(argIndex++);
        int dedicatedThread = state.range(argIndex++);
        int reserved = state.range(argIndex++);

        LOGV2_DEBUG(7015135,
                    3,
                    "SetUp (first)",
                    "exhaustRounds"_attr = exhaustRounds,
                    "dedicatedThread"_attr = dedicatedThread,
                    "reserved"_attr = reserved);

#if TRANSITIONAL_SERVICE_EXECUTOR_SYNCHRONOUS_HAS_RESERVE
        _savedDefaultReserved.emplace(ServiceExecutorSynchronous::defaultReserved, reserved);
#endif
        _savedUseDedicated.emplace(gInitialUseDedicatedThread, dedicatedThread);

        setGlobalServiceContext(ServiceContext::make());
        auto sc = getGlobalServiceContext();
        _coordinator = std::make_unique<MockCoordinator>(sc, exhaustRounds + 1);
        sc->setServiceEntryPoint(_coordinator->makeServiceEntryPoint());
        sc->setTransportLayer(std::make_unique<TransportLayerMockWithReactor>());
        LOGV2_DEBUG(7015136, 3, "About to start sep");
        invariant(_coordinator->serviceEntryPoint()->start());
    }

    void TearDown(benchmark::State& state) override {
        stdx::lock_guard lk{_setupMutex};
        LOGV2_DEBUG(7015137, 3, "TearDown", "configuredThreads"_attr = _configuredThreads);
        if (--_configuredThreads)
            return;
        LOGV2_DEBUG(7015138, 3, "TearDown (last)");

        invariant(_coordinator->serviceEntryPoint()->shutdownAndWait(Seconds{10}));
        setGlobalServiceContext({});
        _savedDefaultReserved.reset();
        _savedUseDedicated.reset();
    }

    void run(benchmark::State& state) {
        for (auto _ : state) {
            LOGV2_DEBUG(7015139, 3, "run: iteration start");
            auto sep = _coordinator->serviceEntryPoint();
            invariant(sep);
            auto session = _coordinator->makeSession();
            invariant(session);
            Future<void> ended = session->observeEnd();
            sep->startSession(std::move(session));
            ended.get();
        }
        LOGV2_DEBUG(7015140, 3, "run: all iterations finished");
        invariant(_coordinator->serviceEntryPoint()->waitForNoSessions(Seconds{1}));
    }

private:
    Mutex _setupMutex;
    int _configuredThreads = 0;
    boost::optional<ScopedValueOverride<size_t>> _savedDefaultReserved;
    boost::optional<ScopedValueOverride<bool>> _savedUseDedicated;
    std::unique_ptr<MockCoordinator> _coordinator;
};

/**
 * ASAN can't handle the # of threads the benchmark creates.
 * With sanitizers, run this in a diminished "correctness check" mode.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const auto kMaxThreads = 1;
#else
/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
const auto kMaxThreads = 2 * ProcessInfo::getNumCores();
#endif

BENCHMARK_DEFINE_F(SessionWorkflowBm, Loop)(benchmark::State& state) {
    run(state);
}

BENCHMARK_REGISTER_F(SessionWorkflowBm, Loop)->Apply([](auto* b) {
    b->ArgNames({"ExhaustRounds", "DedicatedThread", "ReservedThreads"});
    for (int exhaust : {0, 1, 8}) {
        for (int isDedicatedThread : {0, 1}) {
            std::vector<int> res{0};
#if TRANSITIONAL_SERVICE_EXECUTOR_SYNCHRONOUS_HAS_RESERVE
            if (isDedicatedThread == 1)
                res = {0, 1, 4, 16};
#endif
            for (int reserved : res)
                b->Args({exhaust, isDedicatedThread, reserved});
        }
    }
    b->ThreadRange(1, kMaxThreads);
});

}  // namespace
}  // namespace mongo::transport
