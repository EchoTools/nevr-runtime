// Host test for the autopilot's route file and clock (#1202, src/quest/plugins/autopilot/route.h; the grammar is
// docs/adr/0025-quest-autopilot-route.md, "The route file"). Every limit and every derived bit is written out here as a
// literal, so a change to route.cpp fails this test rather than following it.
//
// Built and run by `just test-quest-autopilot`.

#include <cstdint>
#include <cstdio>
#include <string>

#include "quest/plugins/autopilot/route.h"
#include "quest/tests/test_check.h"

namespace {

namespace ap = nevr_autopilot;

const char kSample[] =
    "# a comment line\n"
    "autopilot-route 1          # the version\n"
    "name lobby_loop\n"
    "start_after 5\n"
    "repeat 3\n"
    "\n"
    "step 2.0\n"
    "step 3.0 left.stick=0,1\n"
    "step 1.5 right.stick=1,0\r\n"
    "step 2.0\tleft.stick=0,1 right.trigger=1\n"
    "step 0.5 right.grip=1 right.buttons=a+b\n";

ap::Route Parse(const std::string& text) {
  ap::Route r;
  ap::ParseError e;
  if (!ap::ParseRoute(text, &r, &e)) std::fprintf(stderr, "unexpected refusal, line %zu: %s\n", e.line, e.reason.c_str());
  return r;
}

// The refusal of `text`: line and reason. Checks that it is refused.
ap::ParseError Refusal(const std::string& text) {
  ap::Route r;
  ap::ParseError e;
  const bool ok = ap::ParseRoute(text, &r, &e);
  if (ok) std::fprintf(stderr, "accepted, expected a refusal:\n%s\n", text.c_str());
  QCHECK(!ok);
  return e;
}

std::string Head() { return "autopilot-route 1\nname r\n"; }

void TheSampleParses() {
  ap::Route r;
  ap::ParseError e;
  QCHECK(ap::ParseRoute(kSample, &r, &e));
  QCHECK(r.name == "lobby_loop");
  QCHECK(r.startAfterNs == 5000000000LL);
  QCHECK(r.repeat == 3u);
  QCHECK(r.steps.size() == 5u);
  QCHECK(r.steps[0].durationNs == 2000000000LL);
  QCHECK(r.steps[2].durationNs == 1500000000LL);
  QCHECK(r.steps[4].durationNs == 500000000LL);
  QCHECK(r.PassNs() == 9000000000LL);
  QCHECK(r.PlannedNs() == 27000000000LL);
  QCHECK(r.drives[ap::kLeft] && r.drives[ap::kRight]);
  QCHECK(r.steps[1].hands[ap::kLeft].stickY == 1.0f && r.steps[1].hands[ap::kLeft].stickX == 0.0f);
  QCHECK(r.steps[2].hands[ap::kRight].stickX == 1.0f);
  QCHECK(r.steps[3].hands[ap::kRight].trigger == 1.0f);
  QCHECK(r.steps[4].hands[ap::kRight].grip == 1.0f);
}

void DefaultsAndOneHand() {
  const ap::Route r = Parse(Head() + "step 0.01 left.trigger=0.25\n");
  QCHECK(r.startAfterNs == 0);
  QCHECK(r.repeat == 1u);
  QCHECK(r.steps.size() == 1u && r.steps[0].durationNs == 10000000LL);
  QCHECK(r.drives[ap::kLeft] && !r.drives[ap::kRight]);
  QCHECK(r.steps[0].hands[ap::kLeft].trigger == 0.25f);
  const ap::Route none = Parse(Head() + "step 1\n");
  QCHECK(!none.drives[ap::kLeft] && !none.drives[ap::kRight]);
}

void TheRestStateIsTheHeadersBaseState() {
  const ap::HandState rest = ap::RestState();
  QCHECK(rest.buttons == 0u);
  QCHECK(rest.touches == 0x300u);  // ovrTouch_ThumbUp | ovrTouch_IndexPointing
  QCHECK(rest.trigger == 0.0f && rest.grip == 0.0f && rest.stickX == 0.0f && rest.stickY == 0.0f);
  // A step that names nothing for a hand gives it the rest state.
  const ap::Route r = Parse(Head() + "step 1 right.grip=1\n");
  QCHECK(r.steps[0].hands[ap::kLeft].touches == 0x300u && r.steps[0].hands[ap::kLeft].buttons == 0u);
}

// The bits of one hand's state for a one-step route.
ap::HandState One(const std::string& controls, ap::Hand hand) {
  const ap::Route r = Parse(Head() + "step 1 " + controls + "\n");
  return r.steps.empty() ? ap::HandState{} : r.steps[0].hands[hand];
}

void TheDerivedBits() {
  // Buttons and their touches: A 0x1/0x1, B 0x2/0x2, X 0x100/0x4, Y 0x200/0x8, menu 0x100000 (no touch). A touched key
  // clears ThumbUp; IndexPointing stays (no trigger touch).
  QCHECK(One("right.buttons=a", ap::kRight).buttons == 0x1u);
  QCHECK(One("right.buttons=a", ap::kRight).touches == (0x1u | 0x200u));
  QCHECK(One("right.buttons=b", ap::kRight).buttons == 0x2u);
  QCHECK(One("right.buttons=b", ap::kRight).touches == (0x2u | 0x200u));
  QCHECK(One("left.buttons=x", ap::kLeft).buttons == 0x100u);
  QCHECK(One("left.buttons=x", ap::kLeft).touches == (0x4u | 0x200u));
  QCHECK(One("left.buttons=y", ap::kLeft).buttons == 0x200u);
  QCHECK(One("left.buttons=y", ap::kLeft).touches == (0x8u | 0x200u));
  QCHECK(One("left.buttons=menu", ap::kLeft).buttons == 0x100000u);
  QCHECK(One("left.buttons=menu", ap::kLeft).touches == 0x300u);
  // thumb: the hand's thumb button, the joystick click, and the stick and thumb touches.
  QCHECK(One("left.buttons=thumb", ap::kLeft).buttons == (0x400u | 0x80000000u));
  QCHECK(One("left.buttons=thumb", ap::kLeft).touches == (0x20u | 0x400u | 0x200u));
  QCHECK(One("right.buttons=thumb", ap::kRight).buttons == (0x4u | 0x80000000u));
  QCHECK(One("right.buttons=thumb", ap::kRight).touches == (0x20u | 0x800u | 0x200u));
  // Trigger: any value above 0 touches it (IndexPointing goes); 0.5 and more is the Trigger button.
  QCHECK(One("right.trigger=0.49", ap::kRight).buttons == 0u);
  QCHECK(One("right.trigger=0.49", ap::kRight).touches == (0x40u | 0x100u));
  QCHECK(One("right.trigger=0.5", ap::kRight).buttons == 0x20000000u);
  QCHECK(One("right.trigger=0", ap::kRight).touches == 0x300u);
  // Grip: 0.5 and more is the GripTrigger button; it touches nothing.
  QCHECK(One("left.grip=0.49", ap::kLeft).buttons == 0u);
  QCHECK(One("left.grip=0.5", ap::kLeft).buttons == 0x04000000u);
  QCHECK(One("left.grip=1", ap::kLeft).touches == 0x300u);
  // Stick off centre: the stick and the hand's thumb are touched; centred, not.
  QCHECK(One("left.stick=0,-0.5", ap::kLeft).touches == (0x20u | 0x400u | 0x200u));
  QCHECK(One("right.stick=0.5,0", ap::kRight).touches == (0x20u | 0x800u | 0x200u));
  QCHECK(One("right.stick=0,0", ap::kRight).touches == 0x300u);
  QCHECK(One("left.stick=-1,-1", ap::kLeft).stickX == -1.0f);
  // Together.
  const ap::HandState all = One("right.buttons=a+b+thumb right.trigger=1 right.grip=1 right.stick=1,1", ap::kRight);
  QCHECK(all.buttons == (0x1u | 0x2u | 0x4u | 0x80000000u | 0x20000000u | 0x04000000u));
  QCHECK(all.touches == (0x1u | 0x2u | 0x20u | 0x800u | 0x40u));
}

void NumbersAreExact() {
  QCHECK(Parse(Head() + "step 0.1\n").steps[0].durationNs == 100000000LL);
  QCHECK(Parse(Head() + "step 0.123456789\n").steps[0].durationNs == 123456789LL);
  QCHECK(Parse(Head() + "step 600\n").steps[0].durationNs == 600000000000LL);
  QCHECK(Parse(Head() + "start_after 0\nstep 1\n").startAfterNs == 0);
  QCHECK(Parse(Head() + "start_after 600\nstep 1\n").startAfterNs == 600000000000LL);
  QCHECK(Parse(Head() + "repeat 1000\nstep 1\n").repeat == 1000u);
}

void EachRefusalNamesItsLine() {
  struct Case {
    std::string text;
    std::size_t line;
  };
  const Case cases[] = {
      {"", 0},                                                          // no header
      {"# only a comment\n", 0},                                        // no header
      {"name r\nautopilot-route 1\nstep 1\n", 1},                      // header not first
      {"autopilot-route 2\nname r\nstep 1\n", 1},                      // wrong version
      {"autopilot-route 1 x\nname r\nstep 1\n", 1},                    // extra word
      {"autopilot-route 1\nautopilot-route 1\nname r\nstep 1\n", 2},   // header twice
      {"autopilot-route 1\nstep 1\n", 0},                              // no name
      {"autopilot-route 1\nname r\n", 0},                              // no step
      {Head() + "name s\nstep 1\n", 3},                                // name twice
      {"autopilot-route 1\nname Upper\nstep 1\n", 2},                  // name charset
      {"autopilot-route 1\nname a-b\nstep 1\n", 2},
      {"autopilot-route 1\nname " + std::string(33, 'a') + "\nstep 1\n", 2},
      {"autopilot-route 1\nname\nstep 1\n", 2},
      {Head() + "start_after 600.000000001\nstep 1\n", 3},
      {Head() + "start_after -1\nstep 1\n", 3},
      {Head() + "start_after 1\nstart_after 1\nstep 1\n", 4},
      {Head() + "step 1\nstart_after 1\n", 4},                         // start_after after a step
      {Head() + "repeat 0\nstep 1\n", 3},
      {Head() + "repeat 1001\nstep 1\n", 3},
      {Head() + "repeat 1.5\nstep 1\n", 3},
      {Head() + "repeat 2\nrepeat 2\nstep 1\n", 4},
      {Head() + "step 1\nrepeat 2\n", 4},                              // repeat after a step
      {Head() + "step\n", 3},
      {Head() + "step 0.009\n", 3},
      {Head() + "step 600.000000001\n", 3},
      {Head() + "step 1.\n", 3},
      {Head() + "step .5\n", 3},
      {Head() + "step 1.0000000001\n", 3},                             // ten fractional digits
      {Head() + "step 1e1\n", 3},
      {Head() + "step -1\n", 3},
      {Head() + "step 1 left.stick=0\n", 3},
      {Head() + "step 1 left.stick=1.01,0\n", 3},
      {Head() + "step 1 left.stick=0,-1.5\n", 3},
      {Head() + "step 1 left.trigger=-0.1\n", 3},
      {Head() + "step 1 left.trigger=1.5\n", 3},
      {Head() + "step 1 left.grip=2\n", 3},
      {Head() + "step 1 left.buttons=a\n", 3},                         // a is a right-hand button
      {Head() + "step 1 right.buttons=x\n", 3},
      {Head() + "step 1 right.buttons=menu\n", 3},
      {Head() + "step 1 right.buttons=a+a\n", 3},
      {Head() + "step 1 right.buttons=\n", 3},
      {Head() + "step 1 right.buttons=a+\n", 3},
      {Head() + "step 1 right.trigger=1 right.trigger=0\n", 3},       // named twice
      {Head() + "step 1 right.stick=0,0 right.stick=0,0\n", 3},
      {Head() + "step 1 right.grip=1 right.grip=1\n", 3},
      {Head() + "step 1 right.buttons=a right.buttons=b\n", 3},
      {Head() + "step 1 middle.trigger=1\n", 3},
      {Head() + "step 1 right.trigger\n", 3},
      {Head() + "step 1 right.wheel=1\n", 3},
      {Head() + "walk 1\n", 3},
      {Head() + "step 1 \x01\n", 3},                                    // not printable
      {Head() + "step 1 \xc3\xa9\n", 3},                                // not ASCII
      {Head() + "repeat 7\nstep 600\n", 0},                            // 4200 s over the 3600 s cap
  };
  for (const Case& c : cases) {
    const ap::ParseError e = Refusal(c.text);
    if (e.line != c.line) std::fprintf(stderr, "line %zu, expected %zu: %s\n", e.line, c.line, e.reason.c_str());
    QCHECK(e.line == c.line);
    QCHECK(!e.reason.empty());
  }
}

void TheLimitsHoldAtTheirEdges() {
  // 256 steps parse; 257 do not.
  std::string text = Head();
  for (int i = 0; i < 256; ++i) text += "step 0.01\n";
  QCHECK(Parse(text).steps.size() == 256u);
  const ap::ParseError e = Refusal(text + "step 0.01\n");
  QCHECK(e.line == 259u);
  // 3600 s exactly is allowed.
  QCHECK(Parse(Head() + "repeat 6\nstep 600\n").PlannedNs() == 3600000000000LL);
  // 64 KiB exactly is allowed (comment padding), one byte more is not.
  std::string big = Head() + "step 1\n";
  big += std::string(64 * 1024 - big.size() - 1, '#') + "\n";
  QCHECK(big.size() == 64u * 1024u);
  QCHECK(Parse(big).steps.size() == 1u);
  QCHECK(Refusal(big + "\n").line == 0u);
}

// ---- the clock --------------------------------------------------------------------------------------------------------

void LocateWalksTheSteps() {
  const ap::Route r = Parse(Head() + "start_after 1\nrepeat 2\nstep 0.5\nstep 0.25\n");
  const auto at = [&](std::int64_t ns) { return ap::Locate(r, ns); };
  QCHECK(at(0).phase == ap::Position::kWaiting);
  QCHECK(at(999999999).phase == ap::Position::kWaiting);
  QCHECK(at(1000000000).phase == ap::Position::kRunning && at(1000000000).index == 0 && at(1000000000).pass == 0);
  QCHECK(at(1499999999).index == 0);
  QCHECK(at(1500000000).index == 1 && at(1500000000).pass == 0);  // a boundary opens the next step
  QCHECK(at(1749999999).index == 1);
  QCHECK(at(1750000000).index == 0 && at(1750000000).pass == 1);
  QCHECK(at(2250000000).index == 1 && at(2250000000).pass == 1);
  QCHECK(at(2499999999).phase == ap::Position::kRunning && at(2499999999).index == 1);
  QCHECK(at(2500000000).phase == ap::Position::kDone);  // the planned end is done
  QCHECK(at(9000000000).phase == ap::Position::kDone);
  QCHECK(at(-5).phase == ap::Position::kWaiting);
}

}  // namespace

int main() {
  TheSampleParses();
  DefaultsAndOneHand();
  TheRestStateIsTheHeadersBaseState();
  TheDerivedBits();
  NumbersAreExact();
  EachRefusalNamesItsLine();
  TheLimitsHoldAtTheirEdges();
  LocateWalksTheSteps();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "autopilot_route_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("autopilot_route_test: all checks pass\n");
  return 0;
}
