#include "quest/plugins/autopilot/route.h"

#include <cstdio>

namespace nevr_autopilot {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

// Splits a line (comment already removed) into words separated by spaces or tabs.
std::vector<std::string_view> Words(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && IsSpace(line[i])) ++i;
    const std::size_t from = i;
    while (i < line.size() && !IsSpace(line[i])) ++i;
    if (i > from) words.push_back(line.substr(from, i - from));
  }
  return words;
}

// A decimal `[-]digits[.digits]`, at most 9 digits after the point, scaled by 10^9 into `out`. Exact: no floating point,
// so 0.1 s is 100000000 ns on every compiler and build.
bool ParseFixed9(std::string_view text, bool allowNegative, std::int64_t* out) {
  std::size_t i = 0;
  bool negative = false;
  if (i < text.size() && text[i] == '-') {
    if (!allowNegative) return false;
    negative = true;
    ++i;
  }
  std::int64_t whole = 0;
  std::size_t wholeDigits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    if (++wholeDigits > 6) return false;  // nothing here is larger than 3600
    whole = whole * 10 + (text[i] - '0');
    ++i;
  }
  if (wholeDigits == 0) return false;
  std::int64_t frac = 0;
  std::size_t fracDigits = 0;
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      if (++fracDigits > 9) return false;
      frac = frac * 10 + (text[i] - '0');
      ++i;
    }
    if (fracDigits == 0) return false;
  }
  if (i != text.size()) return false;
  for (std::size_t d = fracDigits; d < 9; ++d) frac *= 10;
  const std::int64_t value = whole * kNsPerSecond + frac;
  *out = negative ? -value : value;
  return true;
}

bool ParseUnsigned(std::string_view text, std::uint32_t max, std::uint32_t* out) {
  if (text.empty() || text.size() > 9) return false;
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (value > max) return false;
  *out = value;
  return true;
}

// A control value in [lo, hi] (as ns-scaled fixed point) to a float.
bool ParseUnit(std::string_view text, std::int64_t lo, std::int64_t hi, float* out) {
  std::int64_t v = 0;
  if (!ParseFixed9(text, lo < 0, &v) || v < lo || v > hi) return false;
  *out = static_cast<float>(static_cast<double>(v) / static_cast<double>(kNsPerSecond));
  return true;
}

bool ValidName(std::string_view name) {
  if (name.empty() || name.size() > 32) return false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

// What one hand's named controls are, before the derived bits.
struct Named {
  bool any = false;
  bool stick = false, trigger = false, grip = false, buttons = false;
  std::uint32_t pressed = 0;  // ovrButton bits of the named buttons
  bool thumb = false;
  float trigger_v = 0.0f, grip_v = 0.0f, x = 0.0f, y = 0.0f;
};

bool ParseButtons(Hand hand, std::string_view list, Named* named, std::string* reason) {
  if (list.empty()) {
    *reason = "empty button list";
    return false;
  }
  std::size_t i = 0;
  while (i <= list.size()) {
    const std::size_t plus = list.find('+', i);
    const std::string_view b = list.substr(i, plus == std::string_view::npos ? std::string_view::npos : plus - i);
    std::uint32_t bit = 0;
    if (hand == kLeft) {
      if (b == "x") bit = kButtonX;
      else if (b == "y") bit = kButtonY;
      else if (b == "thumb") bit = kButtonLThumb;
      else if (b == "menu") bit = kButtonEnter;
    } else {
      if (b == "a") bit = kButtonA;
      else if (b == "b") bit = kButtonB;
      else if (b == "thumb") bit = kButtonRThumb;
    }
    if (bit == 0) {
      *reason = std::string("no button '") + std::string(b) + "' on the " + (hand == kLeft ? "left" : "right") + " hand";
      return false;
    }
    if ((named->pressed & bit) != 0) {
      *reason = std::string("button '") + std::string(b) + "' named twice";
      return false;
    }
    named->pressed |= bit;
    if (b == "thumb") named->thumb = true;
    if (plus == std::string_view::npos) break;
    i = plus + 1;
  }
  return true;
}

bool ParseControl(std::string_view word, Named named[2], std::string* reason) {
  Hand hand;
  std::string_view rest;
  if (word.substr(0, 5) == "left.") {
    hand = kLeft;
    rest = word.substr(5);
  } else if (word.substr(0, 6) == "right.") {
    hand = kRight;
    rest = word.substr(6);
  } else {
    *reason = std::string("control '") + std::string(word) + "' names no hand (left. or right.)";
    return false;
  }
  const std::size_t eq = rest.find('=');
  if (eq == std::string_view::npos) {
    *reason = std::string("control '") + std::string(word) + "' has no '='";
    return false;
  }
  const std::string_view key = rest.substr(0, eq);
  const std::string_view value = rest.substr(eq + 1);
  Named& n = named[hand];
  const auto twice = [&](bool* seen) {
    if (*seen) {
      *reason = std::string("control '") + std::string(key) + "' named twice for one hand";
      return false;
    }
    *seen = true;
    return true;
  };
  if (key == "stick") {
    if (!twice(&n.stick)) return false;
    const std::size_t comma = value.find(',');
    if (comma == std::string_view::npos || !ParseUnit(value.substr(0, comma), -kNsPerSecond, kNsPerSecond, &n.x) ||
        !ParseUnit(value.substr(comma + 1), -kNsPerSecond, kNsPerSecond, &n.y)) {
      *reason = std::string("stick '") + std::string(value) + "' is not x,y with each in -1..1";
      return false;
    }
  } else if (key == "trigger") {
    if (!twice(&n.trigger)) return false;
    if (!ParseUnit(value, 0, kNsPerSecond, &n.trigger_v)) {
      *reason = std::string("trigger '") + std::string(value) + "' is not in 0..1";
      return false;
    }
  } else if (key == "grip") {
    if (!twice(&n.grip)) return false;
    if (!ParseUnit(value, 0, kNsPerSecond, &n.grip_v)) {
      *reason = std::string("grip '") + std::string(value) + "' is not in 0..1";
      return false;
    }
  } else if (key == "buttons") {
    if (!twice(&n.buttons)) return false;
    if (!ParseButtons(hand, value, &n, reason)) return false;
  } else {
    *reason = std::string("unknown control '") + std::string(key) + "'";
    return false;
  }
  n.any = true;
  return true;
}

// The derived bits (ADR 0025, "The route file"): what a controller held this way reports.
HandState Derive(Hand hand, const Named& n) {
  HandState s;
  s.trigger = n.trigger_v;
  s.grip = n.grip_v;
  s.stickX = n.x;
  s.stickY = n.y;
  s.buttons = n.pressed;
  if (n.trigger_v >= 0.5f) s.buttons |= kButtonTrigger;
  if (n.grip_v >= 0.5f) s.buttons |= kButtonGripTrigger;
  if (n.thumb) s.buttons |= kButtonJoystick;
  const std::uint32_t thumbTouch = hand == kLeft ? kTouchLThumb : kTouchRThumb;
  if ((n.pressed & kButtonA) != 0) s.touches |= kTouchA;
  if ((n.pressed & kButtonB) != 0) s.touches |= kTouchB;
  if ((n.pressed & kButtonX) != 0) s.touches |= kTouchX;
  if ((n.pressed & kButtonY) != 0) s.touches |= kTouchY;
  if (n.trigger_v > 0.0f) s.touches |= kTouchIndexTrigger;
  if (n.thumb || n.x != 0.0f || n.y != 0.0f) s.touches |= kTouchJoystick | thumbTouch;
  if ((s.touches & (kTouchA | kTouchB | kTouchX | kTouchY | kTouchJoystick)) == 0) s.touches |= kTouchThumbUp;
  if ((s.touches & kTouchIndexTrigger) == 0) s.touches |= kTouchIndexPointing;
  return s;
}

bool Fail(ParseError* error, std::size_t line, std::string reason) {
  error->line = line;
  error->reason = std::move(reason);
  return false;
}

}  // namespace

std::int64_t Route::PassNs() const noexcept {
  std::int64_t total = 0;
  for (const Step& s : steps) total += s.durationNs;
  return total;
}

HandState RestState() noexcept { return Derive(kLeft, Named{}); }

bool ParseRoute(std::string_view text, Route* route, ParseError* error) {
  if (text.size() > kMaxFileBytes) return Fail(error, 0, "the file is larger than 64 KiB");
  Route r;
  bool sawHeader = false, sawName = false, sawStartAfter = false, sawRepeat = false;
  std::size_t lineNo = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    ++lineNo;
    std::size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    for (const char c : line) {
      if ((c < 0x20 || c > 0x7e) && c != '\t') return Fail(error, lineNo, "a character that is not printable ASCII");
    }
    const std::size_t hash = line.find('#');
    if (hash != std::string_view::npos) line = line.substr(0, hash);
    const std::vector<std::string_view> w = Words(line);
    if (w.empty()) continue;
    const std::string_view directive = w[0];
    if (!sawHeader) {
      if (directive != "autopilot-route" || w.size() != 2 || w[1] != "1") {
        return Fail(error, lineNo, "the first directive is not 'autopilot-route 1'");
      }
      sawHeader = true;
      continue;
    }
    if (directive == "autopilot-route") return Fail(error, lineNo, "'autopilot-route' appears twice");
    if (directive == "name") {
      if (sawName) return Fail(error, lineNo, "'name' appears twice");
      if (w.size() != 2 || !ValidName(w[1])) return Fail(error, lineNo, "the name is not [a-z0-9_]{1,32}");
      r.name = std::string(w[1]);
      sawName = true;
    } else if (directive == "start_after") {
      if (sawStartAfter) return Fail(error, lineNo, "'start_after' appears twice");
      if (!r.steps.empty()) return Fail(error, lineNo, "'start_after' after a step");
      std::int64_t ns = 0;
      if (w.size() != 2 || !ParseFixed9(w[1], false, &ns) || ns > kMaxStartAfterNs) {
        return Fail(error, lineNo, "start_after is not a number of seconds in 0..600");
      }
      r.startAfterNs = ns;
      sawStartAfter = true;
    } else if (directive == "repeat") {
      if (sawRepeat) return Fail(error, lineNo, "'repeat' appears twice");
      if (!r.steps.empty()) return Fail(error, lineNo, "'repeat' after a step");
      std::uint32_t n = 0;
      if (w.size() != 2 || !ParseUnsigned(w[1], kMaxRepeat, &n) || n == 0) {
        return Fail(error, lineNo, "repeat is not a whole number in 1..1000");
      }
      r.repeat = n;
      sawRepeat = true;
    } else if (directive == "step") {
      if (r.steps.size() == kMaxSteps) return Fail(error, lineNo, "more than 256 steps");
      Step step;
      if (w.size() < 2 || !ParseFixed9(w[1], false, &step.durationNs) || step.durationNs < kMinStepNs ||
          step.durationNs > kMaxStepNs) {
        return Fail(error, lineNo, "the step duration is not a number of seconds in 0.01..600");
      }
      Named named[2];
      std::string reason;
      for (std::size_t i = 2; i < w.size(); ++i) {
        if (!ParseControl(w[i], named, &reason)) return Fail(error, lineNo, reason);
      }
      for (const Hand h : {kLeft, kRight}) {
        step.hands[h] = Derive(h, named[h]);
        if (named[h].any) r.drives[h] = true;
      }
      r.steps.push_back(step);
    } else {
      return Fail(error, lineNo, std::string("unknown directive '") + std::string(directive) + "'");
    }
  }
  if (!sawHeader) return Fail(error, 0, "the file has no 'autopilot-route 1'");
  if (!sawName) return Fail(error, 0, "the file has no 'name'");
  if (r.steps.empty()) return Fail(error, 0, "the file has no step");
  if (r.PassNs() * static_cast<std::int64_t>(r.repeat) > kMaxRouteNs) {
    return Fail(error, 0, "the steps times repeat last more than 3600 seconds");
  }
  *route = std::move(r);
  return true;
}

Position Locate(const Route& route, std::int64_t sinceStartNs) noexcept {
  Position p;
  const std::int64_t t = sinceStartNs - route.startAfterNs;
  if (t < 0) return p;
  const std::int64_t passNs = route.PassNs();
  if (passNs <= 0 || t >= route.PlannedNs()) {
    p.phase = Position::kDone;
    return p;
  }
  p.phase = Position::kRunning;
  p.pass = static_cast<std::uint32_t>(t / passNs);
  std::int64_t inPass = t % passNs;
  for (std::uint32_t i = 0; i < route.steps.size(); ++i) {
    if (inPass < route.steps[i].durationNs) {
      p.index = i;
      return p;
    }
    inPass -= route.steps[i].durationNs;
  }
  p.index = static_cast<std::uint32_t>(route.steps.size() - 1);  // unreachable: inPass < passNs
  return p;
}

}  // namespace nevr_autopilot
