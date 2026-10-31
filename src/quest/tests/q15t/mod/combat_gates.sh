g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/combat_gates_test.cpp" -o "$out/combat_gates_test"
"$out/combat_gates_test"
