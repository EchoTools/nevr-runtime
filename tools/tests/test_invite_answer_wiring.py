"""#1278: the invite service's answers reach the wizard's text by the lines the GTests cannot reach.

The rule (patch/invite_answer_text.h) is tested by test_invite_answer_text with a fake writer. What no test target links is
the per-frame dispatcher, the applier's platform glue and the scenario endpoint, so the shape of those lines is pinned here:

  - the dispatcher calls the applier on the game-loop thread only, before the text writer so the same frame writes the texts;
  - the applier returns first when the service is off, takes the answers once, posts through the writer and logs through the
    rule (a text never reaches a log call);
  - the scenario endpoint reads the applier's record, not the service's list (the applier is that list's one reader), and
    only a scenario build asks the applier to keep answers.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments


ROOT = Path(__file__).resolve().parents[2]


def code(path: str) -> str:
    return strip_comments((ROOT / path).read_text())


def dispatcher_problems(tick: str) -> list[str]:
    gate = "if (loop != 0 && loop == GetCurrentThreadId())"
    apply_call = f"{gate} nevr_invite_apply::OnFrame();"
    writer_call = f"{gate} nevr_ui_text::OnFrame();"
    if apply_call not in tick:
        return ["DispatchPerFrameWork does not call nevr_invite_apply::OnFrame() behind the game-loop thread gate"]
    if writer_call not in tick:
        return ["the text writer's call lost its gate"]
    if tick.index(apply_call) > tick.index(writer_call):
        return ["the applier runs after the text writer, so its texts wait a frame"]
    return []


def applier_problems(apply: str) -> list[str]:
    problems = []
    body = apply[apply.index("void OnFrame()") :]
    first = body[body.index("{") + 1 :].lstrip()
    if not first.startswith("if (!nevr_invite_service::Active()) return;"):
        problems.append("OnFrame does not return first when the service is off")
    if body.count("nevr_invite_service::TakeAnswers()") != 1:
        problems.append("OnFrame does not take the answers exactly once")
    if "nevr_ui_text::Ready()" not in body or "nevr_ui_text::Post(" not in body:
        problems.append("OnFrame does not post through the text writer")
    if re.search(r"Log\([^;]*(\.text|request)", body):
        problems.append("a log call carries a request or its text")
    return problems


def rule_problems(rule: str) -> list[str]:
    problems = []
    apply = rule[rule.index("inline void Apply(") :]
    apply = apply[: apply.index("inline constexpr std::size_t kMaxKept")]
    if re.search(r"log\([^;]*\.text", apply) or re.search(r"Line\([^;]*\.text\b(?!\.size)", apply):
        problems.append("Apply puts a text into a log line")
    if "PostedLine(item.field, bytes)" not in apply:
        problems.append("Apply does not log the posted field and size")
    return problems


def scenario_problems(scenario: str, others: dict[str, str]) -> list[str]:
    problems = []
    answers = scenario[scenario.index("nlohmann::json InviteAnswersJson()") :]
    answers = answers[: answers.index("nlohmann::json Handle(")]
    if "nevr_invite_apply::TakeKeptForTest()" not in answers:
        problems.append("the endpoint does not read the applier's record")
    if "nevr_invite_service::TakeAnswers()" in answers:
        problems.append("the endpoint reads the service's list, which the applier empties")
    start = scenario[scenario.index("void Start()") :]
    if "nevr_invite_apply::KeepForTest(true);" not in start[: start.index("WSADATA")]:
        problems.append("the scenario endpoint does not ask the applier to keep answers")
    for path, text in others.items():
        if "KeepForTest(true)" in text:
            problems.append(f"{path} asks the applier to keep answers outside a scenario build")
    return problems


class TheFrameDrivesTheApplier(unittest.TestCase):
    def test_the_applier_runs_on_the_loop_thread_before_the_writer(self):
        self.assertEqual(dispatcher_problems(code("src/runtime/frame/tick.cpp")), [])

    def test_a_broken_dispatcher_is_caught(self):
        tick = code("src/runtime/frame/tick.cpp")
        self.assertTrue(dispatcher_problems(tick.replace("nevr_invite_apply::OnFrame();", "")))
        swapped = tick.replace("nevr_invite_apply::OnFrame();", "@@").replace(
            "nevr_ui_text::OnFrame();", "nevr_invite_apply::OnFrame();"
        ).replace("@@", "nevr_ui_text::OnFrame();")
        self.assertTrue(dispatcher_problems(swapped))


class TheApplierIsCheapWhenOffAndNeverLogsATextLine(unittest.TestCase):
    def test_the_applier_glue(self):
        apply = code("src/runtime/patch/invite_answer_apply.cpp")
        self.assertGreater(len(apply), 400)  # a sensor that reads nothing watches nothing
        self.assertEqual(applier_problems(apply), [])

    def test_a_broken_applier_is_caught(self):
        apply = code("src/runtime/patch/invite_answer_apply.cpp")
        self.assertTrue(applier_problems(apply.replace("if (!nevr_invite_service::Active()) return;", "")))
        self.assertTrue(applier_problems(apply.replace('"%s", line.c_str()', '"%s", request.text.c_str()')))

    def test_the_rule_logs_fields_and_sizes_only(self):
        rule = code("src/runtime/patch/invite_answer_text.h")
        self.assertGreater(len(rule), 1500)
        self.assertEqual(rule_problems(rule), [])
        self.assertTrue(rule_problems(rule.replace("PostedLine(item.field, bytes)", "PostedLine(item.field, item.request.text.size())")))


class TheScenarioEndpointReadsTheAppliersRecord(unittest.TestCase):
    def test_the_endpoint_and_the_keep_switch(self):
        scenario = code("src/runtime/scenario/scenario_control.cpp")
        others = {
            str(p.relative_to(ROOT)): strip_comments(p.read_text())
            for p in (ROOT / "src/runtime").rglob("*.cpp")
            if "scenario/" not in p.as_posix() and "tests/" not in p.as_posix()
        }
        self.assertGreater(len(others), 50)  # the scan found the tree
        self.assertEqual(scenario_problems(scenario, others), [])

    def test_a_broken_endpoint_is_caught(self):
        scenario = code("src/runtime/scenario/scenario_control.cpp")
        self.assertTrue(
            scenario_problems(scenario.replace("nevr_invite_apply::TakeKeptForTest()", "nevr_invite_service::TakeAnswers()"), {})
        )
        self.assertTrue(scenario_problems(scenario.replace("nevr_invite_apply::KeepForTest(true);", ""), {}))
        self.assertTrue(scenario_problems(scenario, {"src/runtime/lifecycle/boot.cpp": "nevr_invite_apply::KeepForTest(true);"}))


if __name__ == "__main__":
    unittest.main()
