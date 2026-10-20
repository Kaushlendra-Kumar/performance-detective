#include "detective/process_runner.hpp"
#include "detective/session.hpp"

#include <catch_amalgamated.hpp>

#include <set>

using namespace detective;
using S = SessionState;

TEST_CASE("session state machine allows only the documented transitions", "[session]") {
    CHECK(valid_transition(S::Created, S::Queued));
    CHECK(valid_transition(S::Queued, S::Starting));
    CHECK(valid_transition(S::Starting, S::Running));
    CHECK(valid_transition(S::Running, S::Stopping));
    CHECK(valid_transition(S::Stopping, S::Analyzing));
    CHECK(valid_transition(S::Running, S::Analyzing));
    CHECK(valid_transition(S::Analyzing, S::Completed));
    CHECK(valid_transition(S::Starting, S::Failed));
    CHECK(valid_transition(S::Queued, S::Cancelled));

    CHECK_FALSE(valid_transition(S::Created, S::Running));
    CHECK_FALSE(valid_transition(S::Created, S::Completed));
    CHECK_FALSE(valid_transition(S::Running, S::Completed));
    for (auto terminal : {S::Completed, S::Failed, S::Cancelled}) {
        CHECK(is_terminal(terminal));
        for (auto to : {S::Created, S::Queued, S::Starting, S::Running, S::Stopping, S::Analyzing, S::Completed,
                        S::Failed, S::Cancelled})
            CHECK_FALSE(valid_transition(terminal, to));
    }
}

TEST_CASE("Session object enforces transitions and keeps messages", "[session]") {
    Session s("s-abc", SessionConfig{}, 0);
    CHECK(s.state() == S::Created);
    CHECK_FALSE(s.transition(S::Completed));
    CHECK(s.transition(S::Queued, "queued"));
    CHECK(s.transition(S::Starting));
    CHECK(s.transition(S::Running, "running"));
    CHECK(s.message() == "running");
    CHECK(s.transition(S::Analyzing));
    CHECK(s.transition(S::Completed, "done"));
    CHECK_FALSE(s.transition(S::Running));
    CHECK(s.state() == S::Completed);
    for (auto st : {S::Created, S::Queued, S::Running, S::Cancelled})
        CHECK(session_state_from_string(to_string(st)) == st);
    CHECK_FALSE(session_state_from_string("BOGUS"));
}

TEST_CASE("session ids are unique and validated", "[session][validation]") {
    std::set<std::string> ids;
    for (int i = 0; i < 2000; ++i) {
        auto id = generate_session_id();
        CHECK(valid_session_id(id));
        ids.insert(id);
    }
    CHECK(ids.size() == 2000);
    CHECK_FALSE(valid_session_id(""));
    CHECK_FALSE(valid_session_id("../etc/passwd"));
    CHECK_FALSE(valid_session_id("S-UPPER"));
    CHECK_FALSE(valid_session_id(std::string(65, 'a')));
}

TEST_CASE("config validation bounds", "[session][validation]") {
    SessionConfig c;
    c.executable = "/bin/true";
    CHECK_FALSE(validate_config(c));
    c.frequency_hz = 20000;
    CHECK(validate_config(c));
    c.frequency_hz = 999;
    c.args.assign(300, "x");
    CHECK(validate_config(c));
    c.args.clear();
    c.args.push_back(std::string("a\0b", 3));
    CHECK(validate_config(c));
}

TEST_CASE("executable validation", "[session][validation]") {
    std::string canon;
    CHECK(validate_executable("", &canon));
    CHECK(validate_executable("/nonexistent/binary", &canon));
    CHECK(validate_executable("/etc", &canon));          // directory
    CHECK(validate_executable("/etc/passwd", &canon));   // not executable
    CHECK_FALSE(validate_executable(DETECTIVE_SELF_EXE_HINT, &canon));
    CHECK(canon[0] == '/');
}
