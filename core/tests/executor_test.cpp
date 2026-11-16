// Action executor: ordering, waits, failed steps (the sequence goes on),
// pending results, overflow, cancellation and the generation guard.
#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

#include "fixtures.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/engine/executor.h"

using namespace hubbo;
using sim::action;
using sim::param;
using sim::waitMs;

namespace {

constexpr ParamSpec kN[] = {ParamSpec::integer("n", 0, 1000)};
constexpr CommandSpec kCommands[] = {{"act", kN, 1}};

struct TestAdapter : IntegrationAdapter {
    const char *integrationId() const override { return "fake"; }
    CommandCatalog commandsFor(const DeviceSpec &) const override { return {kCommands, 1}; }
    ExecResult execute(const ExecRequest &r) override {
        log.push_back(r.params[0].integer);
        tickets.push_back(r.ticket);
        EXPECT_STREQ(r.deviceId, "dev");
        EXPECT_STREQ(r.providerRef, "ref.dev");
        if (results.empty()) return ExecResult::accepted();
        const ExecResult res = results.front();
        results.pop_front();
        return res;
    }
    void releaseAll(uint32_t) override { releases++; }
    void cancel(Ticket ticket, uint32_t) override { cancelled.push_back(ticket); }

    std::vector<int32_t> log;
    std::vector<Ticket> cancelled;
    std::vector<Ticket> tickets;
    std::deque<ExecResult> results;
    int releases = 0;
};

ActionSpec act(int n) { return action("fake", "dev", "act", {param("n", n)}); }

struct Reports : ExecutionObserver {
    std::vector<SequenceReport> finished;
    int dispatched = 0;
    void onActionDispatched(const DispatchRecord &) override { dispatched++; }
    void onSequenceFinished(const SequenceReport &r) override { finished.push_back(r); }
    SequenceReport last() const { return finished.empty() ? SequenceReport{} : finished.back(); }
};

class ExecutorTest : public ::testing::Test {
protected:
    ExecutorTest() {
        adapters.add(adapter);
        exec.setObserver(&reports);
    }

    // Compiles each sequence as the startup of its own activity.
    void build(std::vector<std::vector<ActionSpec>> sequences) {
        ConfigSpec cfg;
        cfg.integrations = {sim::integration("fake", "fake", "Fake")};
        cfg.devices = {sim::device("dev", "Dev", "fake", "other", {"fake", "ref.dev", ""})};
        for (size_t i = 0; i < sequences.size(); i++) {
            ActivitySpec a;
            a.id = "s" + std::to_string(i);
            a.name = a.id;
            a.startup = sequences[i];
            cfg.activities.push_back(a);
        }
        ValidationReport report;
        config = compileConfig(cfg, adapters, &report);
        ASSERT_NE(config, nullptr) << (report.ok() ? "" : report.issues()[0].path);
    }

    ErrorCode run(size_t sequence, Lane lane = Lane::Input, uint32_t now = 0) {
        SequenceInfo info;
        info.lane = lane;
        info.generation = exec.generation();
        return exec.enqueue(info, *config, config->activities[sequence].startup, now);
    }

    TestAdapter adapter;
    AdapterSet adapters;
    Executor exec{adapters, 500};
    Reports reports;
    std::unique_ptr<CompiledConfig> config;
};

}  // namespace

TEST_F(ExecutorTest, RunsActionsInOrder) {
    build({{act(1), act(2), act(3)}});
    ASSERT_EQ(run(0), ErrorCode::Ok);
    EXPECT_TRUE(adapter.log.empty());  // enqueue doesn't run anything
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2, 3}));
    ASSERT_EQ(reports.finished.size(), 1u);
    EXPECT_EQ(reports.last().outcome, SequenceOutcome::Completed);
    EXPECT_TRUE(exec.idle());
    EXPECT_EQ(exec.freeNodes(), limits::kExecutorActionPool);
}

TEST_F(ExecutorTest, WaitIsNonBlockingAndOtherLanesKeepMoving) {
    build({{act(1), waitMs(1000), act(2)}, {act(9)}});
    run(0, Lane::Transition, 0);
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1}));
    uint32_t due = 0;
    ASSERT_TRUE(exec.nextDeadline(&due));
    EXPECT_EQ(due, 1000u);

    // A button press during the wait runs immediately.
    run(1, Lane::Input, 10);
    exec.pump(10);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 9}));

    exec.pump(999);
    EXPECT_EQ(adapter.log.size(), 2u);
    exec.pump(1000);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 9, 2}));
    EXPECT_TRUE(exec.idle());
}

TEST_F(ExecutorTest, SequencesInALaneRunOneAfterAnother) {
    build({{waitMs(100), act(1)}, {act(2)}});
    run(0, Lane::Input);
    run(1, Lane::Input);
    exec.pump(0);
    EXPECT_TRUE(adapter.log.empty());  // act(2) waits behind the first sequence
    exec.pump(100);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2}));
}

TEST_F(ExecutorTest, ZeroWaitDoesNotPark) {
    build({{act(1), waitMs(0), act(2)}});
    run(0);
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2}));
}

// A failed step never stops its sequence: the next step starts at once and
// the report names the first failure.
TEST_F(ExecutorTest, AFailedStepIsReportedAndTheNextOneRuns) {
    build({{act(1), act(2), act(3), act(4)}, {act(5)}});
    adapter.results = {ExecResult::accepted(), ExecResult::failed(ErrorCode::AdapterUnavailable),
                       ExecResult::failed(ErrorCode::AdapterFailed)};
    run(0);
    run(1);
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2, 3, 4, 5}));
    ASSERT_EQ(reports.finished.size(), 2u);
    EXPECT_EQ(reports.finished[0].outcome, SequenceOutcome::CompletedWithErrors);
    EXPECT_EQ(reports.finished[0].error, ErrorCode::AdapterUnavailable);
    EXPECT_EQ(reports.finished[0].actionIndex, 1);
    EXPECT_EQ(reports.finished[1].outcome, SequenceOutcome::Completed);
    EXPECT_EQ(exec.stats().withErrors, 1u);
    EXPECT_EQ(exec.stats().completed, 1u);
    EXPECT_EQ(exec.freeNodes(), limits::kExecutorActionPool);
}

TEST_F(ExecutorTest, ATimedOutStepIsReportedAndTheNextOneRuns) {
    build({{act(1), act(2)}});
    adapter.results = {ExecResult::timeout()};
    run(0);
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(reports.last().outcome, SequenceOutcome::CompletedWithErrors);
    EXPECT_EQ(reports.last().error, ErrorCode::AdapterTimeout);
    EXPECT_EQ(exec.stats().timeouts, 1u);
}

TEST_F(ExecutorTest, PendingMidSequenceParksTheLaneUntilComplete) {
    build({{act(1), act(2)}, {act(3)}});
    adapter.results = {ExecResult::pending()};
    run(0);
    run(1);
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1}));
    exec.pump(100);
    EXPECT_EQ(adapter.log.size(), 1u);
    exec.complete(adapter.tickets[0], ExecResult::accepted(), 120);
    exec.pump(120);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2, 3}));
}

TEST_F(ExecutorTest, APendingStepThatFailsIsReportedAndTheNextOneRuns) {
    build({{act(1), act(2)}});
    adapter.results = {ExecResult::pending()};
    run(0);
    exec.pump(0);
    exec.complete(adapter.tickets[0], ExecResult::failed(ErrorCode::AdapterFailed), 50);
    exec.pump(50);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(reports.last().outcome, SequenceOutcome::CompletedWithErrors);
    EXPECT_EQ(reports.last().actionIndex, 0);
}

TEST_F(ExecutorTest, APendingStepTimesOutAndTheNextOneRuns) {
    build({{act(1), act(2)}});
    adapter.results = {ExecResult::pending()};
    run(0);
    exec.pump(0);
    exec.pump(499);
    EXPECT_TRUE(reports.finished.empty());
    exec.pump(500);
    EXPECT_EQ(adapter.cancelled, (std::vector<Ticket>{adapter.tickets[0]}));
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(reports.last().outcome, SequenceOutcome::CompletedWithErrors);
    EXPECT_EQ(reports.last().error, ErrorCode::AdapterTimeout);
    // A late result for that ticket is ignored.
    exec.complete(adapter.tickets[0], ExecResult::accepted(), 600);
    exec.pump(600);
    EXPECT_EQ(adapter.log.size(), 2u);
    EXPECT_EQ(reports.finished.size(), 1u);
}

TEST_F(ExecutorTest, PendingLastActionDoesNotBlockTheLane) {
    build({{act(1)}, {act(2)}});
    adapter.results = {ExecResult::pending()};
    run(0);
    run(1);
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2}));  // e.g. HA volume, then a BLE key
    EXPECT_EQ(exec.inFlight(), 1u);
    ASSERT_EQ(reports.finished.size(), 1u);  // only the second has finished
    exec.complete(adapter.tickets[0], ExecResult::failed(ErrorCode::AdapterFailed), 30);
    ASSERT_EQ(reports.finished.size(), 2u);
    EXPECT_EQ(reports.last().outcome, SequenceOutcome::CompletedWithErrors);
    EXPECT_EQ(exec.inFlight(), 0u);
}

TEST_F(ExecutorTest, InFlightResultsTimeOut) {
    build({{act(1)}});
    adapter.results = {ExecResult::pending()};
    run(0);
    exec.pump(0);
    exec.pump(500);
    EXPECT_EQ(reports.last().error, ErrorCode::AdapterTimeout);
    EXPECT_EQ(exec.inFlight(), 0u);
}

TEST_F(ExecutorTest, UnknownTicketsAreIgnored) {
    build({{act(1)}});
    exec.complete(0, ExecResult::accepted(), 0);
    exec.complete(12345, ExecResult::failed(ErrorCode::AdapterFailed), 0);
    EXPECT_TRUE(reports.finished.empty());
}

TEST_F(ExecutorTest, OverflowRejectsTheWholeSequenceAndKeepsQueuedWork) {
    build({{waitMs(100), act(1)}, {act(2), act(3)}});
    // Fill the lane with parked sequences.
    for (size_t i = 0; i < limits::kExecutorSequencesPerLane; i++) ASSERT_EQ(run(0), ErrorCode::Ok);
    EXPECT_EQ(run(1), ErrorCode::QueueFull);
    EXPECT_EQ(reports.last().outcome, SequenceOutcome::Rejected);
    EXPECT_EQ(reports.last().error, ErrorCode::QueueFull);
    EXPECT_EQ(exec.stats().rejected, 1u);
    // The other lane is unaffected.
    EXPECT_EQ(run(1, Lane::Transition), ErrorCode::Ok);
    exec.pump(0);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{2, 3}));
    // Everything already queued still runs.
    for (uint32_t t = 100; !exec.idle(); t += 100) exec.pump(t);
    EXPECT_EQ(adapter.log.size(), 2 + limits::kExecutorSequencesPerLane);
    EXPECT_EQ(exec.freeNodes(), limits::kExecutorActionPool);
}

TEST_F(ExecutorTest, OverflowWhenTheNodePoolIsExhausted) {
    std::vector<ActionSpec> longSeq(limits::kMaxSequenceLength, act(1));
    longSeq[0] = waitMs(100);
    build({longSeq});
    size_t accepted = 0;
    while (run(0, accepted % 2 ? Lane::Input : Lane::Transition) == ErrorCode::Ok) accepted++;
    EXPECT_EQ(accepted, limits::kExecutorActionPool / limits::kMaxSequenceLength);
    EXPECT_EQ(exec.stats().rejected, 1u);
    EXPECT_EQ(exec.stats().nodesHighWater, accepted * limits::kMaxSequenceLength);
}

TEST_F(ExecutorTest, CancelAllDropsEverythingAndBumpsGeneration) {
    build({{act(1), waitMs(1000), act(2)}, {waitMs(50), act(3)}});
    adapter.results = {ExecResult::accepted()};
    run(0, Lane::Transition);
    run(1, Lane::Input);
    exec.pump(0);
    exec.cancelAll(7, 10);
    EXPECT_EQ(exec.generation(), 7u);
    EXPECT_TRUE(exec.idle());
    EXPECT_EQ(exec.stats().cancelled, 2u);
    EXPECT_EQ(reports.finished[0].outcome, SequenceOutcome::Cancelled);
    EXPECT_EQ(reports.finished[0].actionIndex, 2);  // next unrun action
    exec.pump(5000);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1}));  // the stale startup never resumes
    EXPECT_EQ(exec.freeNodes(), limits::kExecutorActionPool);
}

TEST_F(ExecutorTest, CancelForgetsInFlightResults) {
    build({{act(1)}});
    adapter.results = {ExecResult::pending()};
    run(0);
    exec.pump(0);
    exec.cancelAll(1, 5);
    EXPECT_EQ(reports.last().outcome, SequenceOutcome::Cancelled);
    const size_t n = reports.finished.size();
    exec.complete(adapter.tickets[0], ExecResult::accepted(), 6);
    EXPECT_EQ(reports.finished.size(), n);
}

// Pending work the executor gives up on is cancelled at its adapter (a
// Bluetooth wait or timed press frees its slot and releases its key); work
// that finished, or failed by its own result, isn't.
TEST_F(ExecutorTest, AbandonedPendingWorkIsCancelledAtTheAdapter) {
    build({{act(1), act(2)}, {act(3)}});
    adapter.results = {ExecResult::pending(), ExecResult::pending()};
    run(0, Lane::Transition);  // parked on its first action
    run(1, Lane::Input);       // in flight (its last action)
    exec.pump(0);
    ASSERT_EQ(adapter.tickets.size(), 2u);
    exec.cancelAll(1, 10);
    EXPECT_EQ(adapter.cancelled, (std::vector<Ticket>{adapter.tickets[0], adapter.tickets[1]}));

    // Past its deadline: cancelled too.
    adapter.cancelled.clear();
    adapter.results = {ExecResult::pending()};
    run(0);
    exec.pump(100);
    exec.pump(600);
    EXPECT_EQ(adapter.cancelled, (std::vector<Ticket>{adapter.tickets[2]}));

    // Finished by the adapter, well or not: nothing to cancel.
    adapter.cancelled.clear();
    adapter.results = {ExecResult::pending()};
    run(0);
    exec.pump(700);
    exec.complete(adapter.tickets.back(), ExecResult::failed(ErrorCode::AdapterFailed), 710);
    exec.cancelAll(2, 720);
    EXPECT_TRUE(adapter.cancelled.empty());
}

TEST_F(ExecutorTest, GenerationGuardRefusesStaleWork) {
    build({{act(1)}});
    SequenceInfo stale;
    stale.generation = exec.generation();
    exec.cancelAll(exec.generation() + 1, 0);
    EXPECT_EQ(exec.enqueue(stale, *config, config->activities[0].startup, 0), ErrorCode::Cancelled);
    exec.pump(0);
    EXPECT_TRUE(adapter.log.empty());
}

TEST_F(ExecutorTest, EmptySpanIsANoOp) {
    build({{}});
    EXPECT_EQ(run(0), ErrorCode::Ok);
    EXPECT_TRUE(exec.idle());
    EXPECT_TRUE(reports.finished.empty());
}

TEST_F(ExecutorTest, WaitsAreWrapSafe) {
    build({{waitMs(100), act(1)}});
    const uint32_t start = 0xFFFFFFF0u;
    run(0, Lane::Input, start);
    exec.pump(start);
    exec.pump(start + 50);
    EXPECT_TRUE(adapter.log.empty());
    exec.pump(start + 100);  // wrapped past zero
    EXPECT_EQ(adapter.log.size(), 1u);
}

TEST_F(ExecutorTest, CopiesActionsSoTheConfigCanBeFreed) {
    build({{act(1), waitMs(100), act(2)}});
    run(0);
    exec.pump(0);
    config.reset();  // a config swap frees the old snapshot
    exec.pump(100);
    EXPECT_EQ(adapter.log, (std::vector<int32_t>{1, 2}));
}
