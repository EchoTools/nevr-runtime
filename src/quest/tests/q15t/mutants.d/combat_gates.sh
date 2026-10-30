# module `combat_gates`: each line removes one rule from src/combat_gates.h; tests/mod/combat_gates.sh must fail on it
runm combat_gates cg_bots_without_levelname src/combat_gates.h "s/c.bots = built.bots \&\& !off.bots \&\& c.levelname;/c.bots = built.bots \&\& !off.bots;/"
runm combat_gates cg_perm_off_ignored       src/combat_gates.h "s/c.perm = built.perm \&\& !off.perm;/c.perm = built.perm;/"
runm combat_gates cg_levelname_off_ignored  src/combat_gates.h "s/c.levelname = built.levelname \&\& !off.levelname;/c.levelname = built.levelname;/"
runm combat_gates cg_bots_reason            src/combat_gates.h "s/off.bots ? \"off_file\" : \"needs_levelname\"/\"off_file\"/"
runm combat_gates cg_perm_file_name         src/combat_gates.h "s/kGatePermOffFile\[\] = \"gate_perm.off\"/kGatePermOffFile[] = \"gate_perm.of\"/"
runm combat_gates cg_bots_file_name         src/combat_gates.h "s/kGateBotsOffFile\[\] = \"gate_bots.off\"/kGateBotsOffFile[] = \"gate_bots.of\"/"
runm combat_gates cg_levelname_file_name    src/combat_gates.h "s/kLevelNameOffFile\[\] = \"levelname.off\"/kLevelNameOffFile[] = \"levelname.of\"/"
