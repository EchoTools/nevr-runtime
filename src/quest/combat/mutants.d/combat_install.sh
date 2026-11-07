# combat_install: the per-module install records and the phase line of src/quest/combat/combat_install.cpp bent once each;
# src/quest/tests/combat_install_test.cpp must fail on every one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<dir> <target>]`.
CI=src/quest/combat/combat_install.cpp
runm ci_record_dropped      $CI "s/    sentinel::LogFields(result.installed || result.off ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, \"combat_install_module\",/    if (false) sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_install_module\",/"
runm ci_record_level_info   $CI "s/result.installed || result.off ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, \"combat_install_module\"/sentinel::LogLevel::kInfo, \"combat_install_module\"/"
runm ci_record_off_warns    $CI "s/result.installed || result.off ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, \"combat_install_module\"/result.installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, \"combat_install_module\"/"
runm ci_record_failed_word  $CI "s/    const char\* outcomeName = \"failed\";/    const char* outcomeName = \"installed\";/"
runm ci_record_off_failed   $CI "s/      outcomeName = \"off\";/      outcomeName = \"failed\";/"
runm ci_record_inst_failed  $CI "s/      outcomeName = \"installed\";/      outcomeName = \"failed\";/"
runm ci_record_no_status    $CI "s/{\"status\", result.status != nullptr ? result.status : \"unset\"}/{\"status\", \"unset\"}/"
runm ci_record_wrong_phase  $CI "s/{{\"phase\", PhaseName(phase)}, {\"module\", module.name}/{{\"phase\", \"normal\"}, {\"module\", module.name}/"
runm ci_record_no_name      $CI "s/{\"module\", module.name}, {\"outcome\", outcomeName}/{\"module\", \"\"}, {\"outcome\", outcomeName}/"
runm ci_phase_list_back     $CI "s/{\"off\", outcome.off}, {\"msg\", msg}});/{\"off\", outcome.off}, {\"msg\", msg}, {\"results_truncated\", 0}});/"
