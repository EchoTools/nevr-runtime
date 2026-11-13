// Command quest-repack-libs checks and stages the library entries of the Quest repack: what a plugin may be called, what
// must survive signing, the pack and the script libraries (docs/adr/0019, 0021, 0029, 0040, 0046; #1117, #1737).
//
// `just android-repack-apk` runs it for every step; the subcommands and their exit codes are the ones the recipe reads.
//
//	check-plugin-name        --apk STORE_APK --plugin PLUGIN_FILE
//	check-plugin-build-ids   --apk STORE_APK --plugin PLUGIN_FILE
//	check-shim-rows          --shim SHIM [--extra-rows FILE]
//	write-plugin-manifest    --stage STAGE_DIR --plugin PLUGIN_FILE... [--early NAME]... [--off NAME]...
//	check-plugin-manifest    --stage STAGE_DIR
//	check-build-variant      --variant WORD
//	write-build-variant      --stage STAGE_DIR --variant WORD
//	stage-pack               --pack-zip PACK_ZIP --meta META_JSON --stage STAGE_DIR --store-apk STORE_APK [--plan-header FILE]
//	pack-problems            --stage STAGE_DIR [--plan-header FILE]
//	stage-libs               --libs DIR --stage STAGE_DIR --store-apk STORE_APK
//	verify-signed            --source STORE_APK --signed SIGNED_APK --stage STAGE_DIR --patcher PATCHER [--extra-rows FILE] [--scratch DIR]
//
// Exit codes: 0 the check passed (the command prints "quest-repack-libs: <command> OK" on standard output); 1 a problem
// was found (each one is a line on standard error, prefixed "quest_repack_libs: "), or the input could not be read;
// 2 a usage error; 74 the run log failed (internal/runlog). The plan header defaults to src/quest/data/plan.h, relative
// to the working directory, which the recipe sets to the checkout.
package main

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"sort"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack"
	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	toolName      = "quest-repack-libs"
	outputPrefix  = "quest_repack_libs: "
	usageExitCode = 2
	problemExit   = 1
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Invocation(toolName, args, getenv, stderr, func(log *slog.Logger) int {
		return dispatch(args, stdout, stderr, log)
	})
}

// flagSpec describes one option of a subcommand: whether it may repeat, whether it takes several values (up to the next
// option), and whether it is required.
type flagSpec struct {
	repeat   bool
	several  bool
	required bool
	def      string
}

// parsed holds the values of a subcommand's options, by option name.
type parsed struct {
	single map[string]string
	multi  map[string][]string
}

func (p parsed) get(name string) string { return p.single[name] }

func (p parsed) list(name string) []string { return p.multi[name] }

// parseOptions reads the options of one subcommand the way argparse does for the forms the recipe uses: `--name value`
// and `--name=value`, a repeatable option taken each time it occurs, and a several-valued option taking the values up to
// the next option.
func parseOptions(command string, specs map[string]flagSpec, args []string) (parsed, string) {
	p := parsed{single: map[string]string{}, multi: map[string][]string{}}
	seen := map[string]bool{}
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if !strings.HasPrefix(arg, "--") {
			return p, fmt.Sprintf("unrecognized arguments: %s", arg)
		}
		name, value, hasValue := strings.Cut(arg, "=")
		spec, known := specs[name]
		if !known {
			return p, fmt.Sprintf("unrecognized arguments: %s", arg)
		}
		seen[name] = true
		if spec.several {
			values := []string{}
			if hasValue {
				values = append(values, value)
			}
			for i+1 < len(args) && !strings.HasPrefix(args[i+1], "--") {
				i++
				values = append(values, args[i])
			}
			if len(values) == 0 {
				return p, fmt.Sprintf("argument %s: expected at least one argument", name)
			}
			p.multi[name] = append(p.multi[name], values...)
			continue
		}
		if !hasValue {
			if i+1 >= len(args) {
				return p, fmt.Sprintf("argument %s: expected one argument", name)
			}
			i++
			value = args[i]
		}
		if spec.repeat {
			p.multi[name] = append(p.multi[name], value)
		} else {
			p.single[name] = value
		}
	}
	var missing []string
	for name, spec := range specs {
		if spec.required && !seen[name] {
			missing = append(missing, name)
		}
		if !seen[name] && spec.def != "" {
			p.single[name] = spec.def
		}
	}
	if len(missing) > 0 {
		sort.Strings(missing)
		return p, "the following arguments are required: " + strings.Join(missing, ", ")
	}
	return p, ""
}

// specsFor are the options of each subcommand. The plan header's default is the checkout's, relative to the working
// directory (the recipe runs at the checkout root).
func specsFor(command string) (map[string]flagSpec, bool) {
	req := flagSpec{required: true}
	opt := flagSpec{}
	switch command {
	case "check-plugin-name", "check-plugin-build-ids":
		return map[string]flagSpec{"--apk": req, "--plugin": req}, true
	case "check-shim-rows":
		return map[string]flagSpec{"--shim": req, "--extra-rows": opt}, true
	case "write-plugin-manifest":
		return map[string]flagSpec{"--stage": req, "--plugin": {required: true, several: true},
			"--early": {repeat: true}, "--off": {repeat: true}}, true
	case "check-plugin-manifest":
		return map[string]flagSpec{"--stage": req}, true
	case "check-build-variant":
		return map[string]flagSpec{"--variant": req}, true
	case "write-build-variant":
		return map[string]flagSpec{"--stage": req, "--variant": req}, true
	case "stage-pack":
		return map[string]flagSpec{"--pack-zip": req, "--meta": req, "--stage": req, "--store-apk": req,
			"--plan-header": {def: questrepack.DefaultPlanHeader}}, true
	case "pack-problems":
		return map[string]flagSpec{"--stage": req, "--plan-header": {def: questrepack.DefaultPlanHeader}}, true
	case "stage-libs":
		return map[string]flagSpec{"--libs": req, "--stage": req, "--store-apk": req}, true
	case "verify-signed":
		return map[string]flagSpec{"--source": req, "--signed": req, "--stage": req, "--patcher": req,
			"--extra-rows": opt, "--scratch": opt}, true
	}
	return nil, false
}

// dispatch runs one subcommand and writes its problems or its OK line; it returns the exit code.
func dispatch(args []string, stdout, stderr io.Writer, log *slog.Logger) int {
	if len(args) == 0 {
		fmt.Fprintf(stderr, "%s: error: a command is required\n", toolName)
		return usageExitCode
	}
	command, rest := args[0], args[1:]
	specs, ok := specsFor(command)
	if !ok {
		fmt.Fprintf(stderr, "%s: error: argument command: invalid choice: %q\n", toolName, command)
		return usageExitCode
	}
	p, problem := parseOptions(command, specs, rest)
	if problem != "" {
		fmt.Fprintf(stderr, "%s: error: %s\n", toolName, problem)
		return usageExitCode
	}
	problems, err := execute(command, p, stdout, log)
	if err != nil {
		problems = append(problems, err.Error())
	}
	for _, line := range problems {
		fmt.Fprintf(stderr, "%s%s\n", outputPrefix, line)
	}
	if len(problems) > 0 {
		return problemExit
	}
	fmt.Fprintf(stdout, "%s%s OK\n", outputPrefix, command)
	return 0
}

// execute is the work of one subcommand: its problems (empty when it passed), and an error when it could not run.
func execute(command string, p parsed, out io.Writer, log *slog.Logger) ([]string, error) {
	log.Info("check", "command", command)
	switch command {
	case "check-plugin-name":
		found, err := questrepack.CheckPluginName(p.get("--apk"), p.get("--plugin"))
		var problems []string
		for _, c := range found {
			problems = append(problems, fmt.Sprintf("plugin %s would replace %s", baseOf(p.get("--plugin")), c))
		}
		return problems, err
	case "check-plugin-build-ids":
		return questrepack.CheckPluginBuildIDs(p.get("--apk"), p.get("--plugin"))
	case "check-shim-rows":
		return questrepack.ShimRowsProblems(p.get("--shim"), p.get("--extra-rows")), nil
	case "write-plugin-manifest":
		return questrepack.WritePluginManifest(p.get("--stage"), p.list("--plugin"), p.list("--early"), p.list("--off")), nil
	case "check-plugin-manifest":
		return questrepack.PluginManifestProblems(p.get("--stage")), nil
	case "check-build-variant":
		return questrepack.BuildVariantProblems(p.get("--variant")), nil
	case "write-build-variant":
		return questrepack.WriteBuildVariant(p.get("--stage"), p.get("--variant")), nil
	case "stage-pack":
		return questrepack.StagePack(questrepack.StagePackOptions{
			PackZip: p.get("--pack-zip"), Meta: p.get("--meta"), Stage: p.get("--stage"),
			StoreAPK: p.get("--store-apk"), PlanHeader: p.get("--plan-header"), Out: out,
		})
	case "pack-problems":
		return questrepack.PackProblems(p.get("--stage"), p.get("--plan-header")), nil
	case "stage-libs":
		return questrepack.StageLibs(questrepack.StageLibsOptions{
			Libs: p.get("--libs"), Stage: p.get("--stage"), StoreAPK: p.get("--store-apk"), Out: out,
		})
	default: // verify-signed
		return questrepack.VerifySigned(questrepack.VerifyOptions{
			Source: p.get("--source"), Signed: p.get("--signed"), Stage: p.get("--stage"), Patcher: p.get("--patcher"),
			ExtraRows: p.get("--extra-rows"), Scratch: p.get("--scratch"), PlanHeader: questrepack.DefaultPlanHeader,
			PatcherTimeout: questrepack.PatcherTimeoutDefault, Out: out,
		})
	}
}

func baseOf(path string) string {
	if i := strings.LastIndex(path, "/"); i >= 0 {
		return path[i+1:]
	}
	return path
}
