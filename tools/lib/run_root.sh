# run_root.sh: a shell tool's own scratch is a run root, made and removed by the one Go implementation
# (tools/internal/runroot, tools/cmd/run-root, built as build/go-tools/run-root). Source it; do not run it.
#
#   rr=$(run_root_bin "<repo root>") || exit 1     # the binary's path; built with the `just go-tools` command when missing
#   scratch=$("$rr" make <label>) || exit 1         # a new run root under the work root; prints its path
#   trap '"$rr" remove "$scratch"' EXIT             # removed once, by the Go tool's checks
run_root_bin() {
  local repo="$1" bin
  bin="$repo/build/go-tools/run-root"
  if [ ! -x "$bin" ]; then
    mkdir -p "$repo/build/go-tools" || return 1
    go -C "$repo/tools" build -trimpath -o "$repo/build/go-tools/" ./cmd/run-root || return 1
  fi
  printf '%s' "$bin"
}
