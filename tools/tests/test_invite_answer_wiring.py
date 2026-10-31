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
    gate = "if (!nevr_invite_service::AnswersPending()) return;"
    if gate not in body or body.index(gate) > body.index("nevr_invite_service::TakeAnswers()"):
        problems.append("OnFrame takes the service lock before it checks the pending flag")
    if "nevr_ui_text::Ready()" not in body or "nevr_ui_text::Post(" not in body:
        problems.append("OnFrame does not post through the text writer")
    # The log call is the named `Say` above OnFrame (#1734) as well as anything in OnFrame: scan the whole file, and require the rule
    # to be given `Say`, so a text cannot reach a log call through a lambda written in its place.
    if re.search(r"Log\([^;]*(\.text|request)", apply):
        problems.append("a log call carries a request or its text")
    if not re.search(r"nevr_invite_text::Apply\(.*?\bSay\);", body, re.S):
        problems.append("OnFrame does not log the rule's lines through Say")
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
        self.assertTrue(applier_problems(apply.replace("      Say);", "      nullptr);")))
        gate = "if (!nevr_invite_service::AnswersPending()) return;"
        self.assertTrue(applier_problems(apply.replace(gate, "")))
        moved = apply.replace(gate, "").replace(
            "nevr_invite_service::TakeAnswers();", "nevr_invite_service::TakeAnswers();\n  " + gate
        )
        self.assertTrue(applier_problems(moved))

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


def matching_close(text: str, open_at: int) -> int:
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def function_span(text: str, signature: str) -> tuple[int, int]:
    """The (open, close) indices of the body of the function with this signature, braces included."""
    open_at = text.index("{", text.index(signature))
    return open_at, matching_close(text, open_at)


def enclosing_block(text: str, at: int) -> tuple[int, int]:
    """The (open, close) indices of the innermost block that holds index `at`."""
    depth = 0
    for i in range(at, -1, -1):
        if text[i] == "}":
            depth += 1
        elif text[i] == "{":
            if depth == 0:
                return i, matching_close(text, i)
            depth -= 1
    return -1, -1


LOCK_TAKEN = "lock(m_)"
FLAG_SET = "answers_pending->store(true, std::memory_order_release)"
FLAG_CLEAR = "answers_pending->store(false, std::memory_order_release)"


def worker_flag_problems(worker: str) -> list[str]:
    """#1415: the worker sets the pending flag under its lock after the push, and clears it under the lock when it empties the list."""
    problems = []
    open_at, close_at = function_span(worker, "void Deliver(Answer answer)")
    deliver = worker[open_at : close_at + 1]
    lock_at = deliver.find("std::lock_guard<std::mutex> lock(m_);")
    if lock_at < 0:
        problems.append("Deliver takes no lock on m_")
    else:
        block_open, block_close = enclosing_block(deliver, lock_at)
        block = deliver[block_open : block_close + 1]
        set_at = block.find(FLAG_SET)
        push_at = block.find("answers_.push_back(")
        if set_at < 0:
            problems.append("Deliver does not set the pending flag inside its lock block")
        elif push_at < 0 or push_at > set_at:
            problems.append("Deliver sets the pending flag before it pushes the answer")
    open_at, close_at = function_span(worker, "std::vector<Answer> DrainLocked()")
    drain = worker[open_at : close_at + 1]
    clear_at = drain.find(FLAG_CLEAR)
    if clear_at < 0:
        problems.append("DrainLocked does not clear the pending flag")
    elif clear_at < drain.find("answers_.clear();"):
        problems.append("DrainLocked clears the pending flag before it empties the list")
    for name, lock in (("TakeAnswers", "std::lock_guard<std::mutex> lock(m_);"), ("WaitForAnswers", "std::unique_lock<std::mutex> lock(m_);")):
        open_at, close_at = function_span(worker, f"std::vector<Answer> {name}(")
        if lock not in worker[open_at : close_at + 1]:
            problems.append(f"{name} calls DrainLocked without taking m_")
    return problems


class TheWorkerSetsThePendingFlagUnderItsLock(unittest.TestCase):
    def test_the_flag_moves_with_the_answers_under_the_lock(self):
        worker = code("src/runtime/patch/invite_worker.h")
        self.assertGreater(len(worker), 2000)  # a sensor that reads nothing watches nothing
        self.assertEqual(worker_flag_problems(worker), [])

    def test_a_broken_worker_is_caught(self):
        worker = code("src/runtime/patch/invite_worker.h")
        set_line = f"if (parts_.answers_pending != nullptr) parts_.{FLAG_SET};"
        clear_line = f"if (parts_.answers_pending != nullptr) parts_.{FLAG_CLEAR};"
        push = "answers_.push_back(std::move(answer));"
        for needle in (set_line, clear_line, push, "answered_.notify_all();", "answers_.clear();"):
            self.assertEqual(worker.count(needle), 1, needle)  # each mutation below must change the text it is aimed at
        self.assertTrue(worker_flag_problems(worker.replace(set_line, "")))
        self.assertTrue(worker_flag_problems(worker.replace(set_line, "").replace("answered_.notify_all();", set_line + "\n    answered_.notify_all();")))
        self.assertTrue(worker_flag_problems(worker.replace(set_line, "").replace(push, set_line + "\n      " + push)))
        self.assertTrue(worker_flag_problems(worker.replace(clear_line, "")))
        self.assertTrue(worker_flag_problems(worker.replace(clear_line, "").replace("answers_.clear();", clear_line + "\n    answers_.clear();")))


if __name__ == "__main__":
    unittest.main()
