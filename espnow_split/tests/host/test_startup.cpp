#include "StartupResources.h"
#include "test_framework.h"

namespace {
struct Resources {
  int fail = 0, attempts = 0;
  bool state = false, buffer = false, mutex = false, queue = false, task = false;
  bool allocate(int stage) { ++attempts; return fail != stage; }
  bool createState() { return state = allocate(1); }
  bool createPublishBuffer() { return buffer = allocate(2); }
  bool createMutex() { return mutex = allocate(3); }
  bool createQueue() { return queue = allocate(4); }
  bool createLocalTask() { return task = allocate(5); }
};
}

TEST(startup_all_resources_ready) {
  Resources r;
  CHECK_EQ(gw::allocateStartupResources(r), gw::StartupFailure::None);
  CHECK(r.state && r.buffer && r.mutex && r.queue && r.task);
  CHECK_EQ(r.attempts, 5);
}
TEST(startup_state_failure_stops_before_other_resources) {
  Resources r; r.fail = 1;
  CHECK_EQ(gw::allocateStartupResources(r), gw::StartupFailure::State);
  CHECK_EQ(r.attempts, 1);
  CHECK(!r.state && !r.buffer && !r.mutex && !r.queue && !r.task);
}
TEST(startup_publish_failure_preserves_local_resources) {
  Resources r; r.fail = 2;
  CHECK_EQ(gw::allocateStartupResources(r), gw::StartupFailure::None);
  CHECK(r.state && !r.buffer && r.mutex && r.queue && r.task);
  CHECK_EQ(r.attempts, 5);
}
TEST(startup_mutex_failure_never_creates_queue_or_task) {
  Resources r; r.fail = 3;
  CHECK_EQ(gw::allocateStartupResources(r), gw::StartupFailure::Mutex);
  CHECK_EQ(r.attempts, 3);
  CHECK(!r.mutex && !r.queue && !r.task);
}
TEST(startup_queue_failure_never_creates_task) {
  Resources r; r.fail = 4;
  CHECK_EQ(gw::allocateStartupResources(r), gw::StartupFailure::Queue);
  CHECK_EQ(r.attempts, 4);
  CHECK(!r.queue && !r.task);
}
TEST(startup_task_failure_is_critical) {
  Resources r; r.fail = 5;
  CHECK_EQ(gw::allocateStartupResources(r), gw::StartupFailure::LocalTask);
  CHECK_EQ(r.attempts, 5);
  CHECK(!r.task);
  CHECK_STR(gw::startupFailureName(gw::StartupFailure::LocalTask), "bioiot_local");
}
