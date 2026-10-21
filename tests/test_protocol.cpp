#include <gtest/gtest.h>

#include "core/stage_protocol.hpp"

using namespace vs;

TEST(Protocol, RequestsRoundTrip) {
  for (const StageRequest& r : {StageRequest{StageCmd::Move, -1234.5, {}}, StageRequest{StageCmd::Arrive, 2000, {}},
                                StageRequest{StageCmd::Settle, 500, {}}, StageRequest{StageCmd::Pos, 0, {}},
                                StageRequest{StageCmd::Reset, 0, {}}, StageRequest{StageCmd::Fault, 0, "stall"}}) {
    auto p = parse_request(format_request(r));
    ASSERT_TRUE(p) << format_request(r);
    EXPECT_EQ(p->cmd, r.cmd);
    EXPECT_DOUBLE_EQ(p->value, r.value);
    EXPECT_EQ(p->arg, r.arg);
  }
}

TEST(Protocol, RepliesRoundTrip) {
  for (const StageReply& r : {StageReply{ReplyKind::Ok, 0, {}}, StageReply{ReplyKind::Err, 0, "out of range"},
                              StageReply{ReplyKind::Arrived, 0, {}}, StageReply{ReplyKind::Done, 12.25, {}},
                              StageReply{ReplyKind::Timeout, 0, {}}, StageReply{ReplyKind::Pos, -3.5, "SETTLING"}}) {
    auto p = parse_reply(format_reply(r));
    ASSERT_TRUE(p) << format_reply(r);
    EXPECT_EQ(p->kind, r.kind);
    EXPECT_DOUBLE_EQ(p->x_um, r.x_um);
    EXPECT_EQ(p->text, r.text);
  }
}

TEST(Protocol, MalformedRequestsRejected) {
  for (const char* bad : {"", "MOVE", "MOVE abc", "MOVE 1 2", "MOVE nan", "MOVE inf", "ARRIVE -5", "SETTLE",
                          "FAULT", "FAULT explode", "move 10", "POS extra", "MOVE 1e999"}) {
    EXPECT_FALSE(parse_request(bad)) << '"' << bad << '"';
  }
}

TEST(Protocol, ToleratesCrLfAndSpaces) {
  auto p = parse_request("  MOVE   250.5 \r\n");
  ASSERT_TRUE(p);
  EXPECT_DOUBLE_EQ(p->value, 250.5);
}

TEST(Protocol, MalformedRepliesRejected) {
  for (const char* bad : {"", "DONE", "DONE x", "POS 1", "POS a b", "WHAT"}) EXPECT_FALSE(parse_reply(bad)) << bad;
}
