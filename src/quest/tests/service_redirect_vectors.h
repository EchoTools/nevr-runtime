#pragma once

namespace nevr_quest_test {

struct RedirectVector {
  const char* input;
  const char* socketTarget;
  const char* httpTarget;
  bool bridgeActive;
  unsigned bridgePort;
  const char* expected;  // nullptr means leave the original URL unchanged.
};

inline constexpr RedirectVector kRedirectVectors[] = {
    {"wss://login.readyatdawn.com/rad/rad15_live", "ws://game.example/spr", nullptr, false, 0,
     "ws://game.example/spr"},
    {"ws://any.example/path", "ws://game.example/spr", nullptr, true, 53748, "ws://127.0.0.1:53748"},
    {"https://config.readyatdawn.com/rad/rad15_live", nullptr, "https://api.example:7350", true, 53748,
     "https://api.example:7350"},
    {"https://unrelated.example/path", "ws://game.example/spr", "https://api.example", true, 53748, nullptr},
    {"wss://login.readyatdawn.com/path", nullptr, nullptr, false, 0, nullptr},
    {"wss://login.readyatdawn.com/path", "", nullptr, false, 0, nullptr},
    {"https://config.readyatdawn.com/path", nullptr, "", false, 0, nullptr},
};

}  // namespace nevr_quest_test
