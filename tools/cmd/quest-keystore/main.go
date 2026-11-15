// Command quest-keystore holds the Quest APK signing key for one command at a time. tools/quest_keystore.sh is the
// entry a person or a recipe runs: it builds this command and runs it, so the binary is never stale (ADR 0044, rule 7).
//
//	quest-keystore with <command...>   run the command with NEVR_QUEST_KS, NEVR_QUEST_KS_ALIAS and NEVR_QUEST_KS_PASS set
//	quest-keystore fingerprint         print the signer's SHA-256 as lower-case hex with no colons, and nothing else on stdout
//	quest-keystore check               exit 0, silently, when NEVR_QUEST_KEYSTORE opens with NEVR_QUEST_KEYSTORE_PASS
//
// The keystore is the file NEVR_QUEST_KEYSTORE names and its passphrase is NEVR_QUEST_KEYSTORE_PASS. The key is never
// generated here, and the passphrase is never an argument: keytool and apksigner read it from the environment
// (-storepass:env, --ks-pass env:). `with` copies the keystore into a mode-600 file in a mode-700 directory (on tmpfs
// when XDG_RUNTIME_DIR is not usable), runs the command, and shreds the copy and removes the directory on every exit
// path, including SIGINT, SIGTERM and SIGHUP, which it passes to the command first.
//
// Exit codes: `with` returns the command's own status, or 128 plus the signal that stopped it; 1 a refusal (the reason
// is printed, and no command runs); 2 a usage error; 126 a command that cannot run; 127 a command that is not found;
// 74 the run log cannot be written (internal/runlog).
//
// The environment is passed as a list, not as a getenv function: a command started by `with` needs all of it (PATH,
// JAVA_HOME and the rest), and a getenv cannot provide that. Every lookup goes through that list, never os.Getenv.
package main

import (
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const toolName = "quest-keystore"

// The keystore's alias and the variables it reads and exports. The names are the contract the justfile recipes and
// AGENTS.md name (NEVR_QUEST_KEYSTORE, NEVR_QUEST_KEYSTORE_PASS) and the ones the signing command reads (NEVR_QUEST_KS*).
const (
	alias          = "nevr"
	envKeystore    = "NEVR_QUEST_KEYSTORE"
	envPass        = "NEVR_QUEST_KEYSTORE_PASS"
	envKS          = "NEVR_QUEST_KS"
	envKSAlias     = "NEVR_QUEST_KS_ALIAS"
	envKSPass      = "NEVR_QUEST_KS_PASS"
	envRuntimeDir  = "XDG_RUNTIME_DIR"
	envPath        = "PATH"
	keyFileName    = "quest-signing-keystore.jks"
	keyDirPattern  = "nevr-quest-keystore."
	fallbackDir    = "/dev/shm"
	keyFileMode    = 0o600
	keyDirMode     = 0o700
	exitOK         = 0
	exitRefused    = 1
	exitUsage      = 2
	exitNotRun     = 126
	exitNotFound   = 127
	signalExitBase = 128
	logPrefix      = "[quest-keystore] "
	errPrefix      = "ERROR: "
	keytoolName    = "keytool"
	shredName      = "shred"
	sha256Pattern  = `SHA-?256`
	fieldSeparator = ": "
	accessWrite    = 2 // W_OK in unistd.h
	withMinArgs    = 2 // "with" and at least one word of the command
	execBits       = 0o111
)

// usageText is what a usage error prints. The entry point is the script, so that is the name it shows.
const usageText = `usage: tools/quest_keystore.sh with <command...>
       tools/quest_keystore.sh fingerprint
       tools/quest_keystore.sh check

environment: NEVR_QUEST_KEYSTORE (path to the keystore file), NEVR_QUEST_KEYSTORE_PASS (its passphrase)
`

var sha256Line = regexp.MustCompile(sha256Pattern)

// refusal is a stop with a reason token for the run log. Its message is printed as the ERROR line and never names a
// passphrase or the key's bytes.
type refusal struct{ reason, msg string }

func (r refusal) Error() string { return r.msg }

// main is the one thing below os.Exit: it hands the process's arguments and environment to run.
func main() { os.Exit(run(os.Args[1:], os.Environ(), os.Stdout, os.Stderr)) }

// run is the tool with its run log (internal/runlog): one "invoked" and one "finished" record in
// $XDG_STATE_HOME/nevr-runtime/quest-keystore.jsonl.
func run(args []string, environ []string, stdout, stderr io.Writer) int {
	return runWith(args, environ, stdout, stderr, os.MkdirTemp)
}

// runWith is run with the function that makes the key's directory given, so a test can make it fail.
func runWith(args []string, environ []string, stdout, stderr io.Writer, mkdirTemp func(string, string) (string, error)) int {
	return runlog.Invocation(toolName, args, envLookup(environ), stderr, func(log *slog.Logger) int {
		a := &app{environ: environ, stdout: stdout, stderr: stderr, log: log, mkdirTemp: mkdirTemp}
		return a.main(args)
	})
}

// app is one run: its environment, its streams, its run log and the key's directory maker.
type app struct {
	environ   []string
	stdout    io.Writer
	stderr    io.Writer
	log       *slog.Logger
	mkdirTemp func(string, string) (string, error)
}

func (a *app) getenv(key string) string { return envLookup(a.environ)(key) }

func (a *app) main(args []string) int {
	if len(args) < 1 {
		return a.usage()
	}
	switch args[0] {
	case "with":
		if len(args) < withMinArgs {
			return a.usage()
		}
		return a.with(args[1:])
	case "fingerprint":
		if len(args) != 1 {
			return a.usage()
		}
		return a.fingerprint()
	case "check":
		if len(args) != 1 {
			return a.usage()
		}
		return a.check()
	case "-h", "--help", "help":
		return a.usage()
	default:
		fmt.Fprintf(a.stderr, "%sunknown command: %s\n", errPrefix, args[0])
		return a.usage()
	}
}

func (a *app) usage() int {
	io.WriteString(a.stderr, usageText)
	a.log.Info("usage")
	return exitUsage
}

// fail prints a stop as the ERROR line, logs its reason and returns the refusal's exit code.
func (a *app) fail(err error) int {
	reason := "error"
	var r refusal
	if errors.As(err, &r) {
		reason = r.reason
	}
	fmt.Fprintf(a.stderr, "%s%s\n", errPrefix, err)
	a.log.Error("refused", "reason", reason)
	return exitRefused
}

func (a *app) infof(event, format string, args ...any) {
	fmt.Fprintf(a.stderr, logPrefix+format+"\n", args...)
	a.log.Info(event)
}

// inputs returns the keystore path and its passphrase, or the refusal that names the variable to set.
func (a *app) inputs() (string, string, error) {
	ks := a.getenv(envKeystore)
	if ks == "" {
		return "", "", refusal{"config", fmt.Sprintf("%s is not set: set it to the path of the Quest signing keystore file (the one persistent key; it is never generated here)", envKeystore)}
	}
	if !readableFile(ks) {
		return "", "", refusal{"unreadable", fmt.Sprintf("%s=%s is not a readable file", envKeystore, ks)}
	}
	pass := a.getenv(envPass)
	if pass == "" {
		return "", "", refusal{"config", fmt.Sprintf("%s is not set: set it to the keystore's passphrase (it is read from the environment, never from an argument)", envPass)}
	}
	return ks, pass, nil
}

// openCheck opens the keystore at path with the passphrase; a wrong passphrase or a missing alias is the refusal.
func (a *app) openCheck(path, pass string) error {
	kt, ok := lookPath(keytoolName, a.getenv(envPath))
	if !ok {
		return refusal{"keytool", "keytool is required to validate the keystore"}
	}
	cmd := &exec.Cmd{
		Path: kt,
		Args: []string{keytoolName, "-list", "-keystore", path, "-alias", alias, "-storepass:env", envKSPass},
		Env:  setEnv(a.environ, envKSPass, pass),
	}
	if err := cmd.Run(); err != nil {
		return refusal{"open", fmt.Sprintf("%s cannot be opened with %s (wrong passphrase, or no alias %s)", envKeystore, envPass, alias)}
	}
	return nil
}

func (a *app) check() int {
	ks, pass, err := a.inputs()
	if err != nil {
		return a.fail(err)
	}
	if err := a.openCheck(ks, pass); err != nil {
		return a.fail(err)
	}
	a.infof("keystore_opens", "keystore opens alias=%s", alias)
	return exitOK
}

func (a *app) fingerprint() int {
	key, err := a.materialize()
	defer key.cleanup(a)
	if err != nil {
		return a.fail(err)
	}
	fp, err := a.signerSHA256(key.path, key.pass)
	if err != nil || fp == "" {
		return a.fail(refusal{"fingerprint", "could not read a SHA-256 fingerprint from the keystore"})
	}
	fmt.Fprintln(a.stdout, fp)
	return exitOK
}

// signerSHA256 is the first SHA-256 line of keytool's listing, read the way the script read it: the text after the
// first ": ", with its colons and white space removed, in lower case. keytool's standard error is discarded.
func (a *app) signerSHA256(path, pass string) (string, error) {
	kt, ok := lookPath(keytoolName, a.getenv(envPath))
	if !ok {
		return "", refusal{"keytool", "keytool is required to validate the keystore"}
	}
	cmd := &exec.Cmd{
		Path: kt,
		Args: []string{keytoolName, "-list", "-keystore", path, "-alias", alias, "-storepass:env", envKSPass},
		Env:  setEnv(a.environ, envKSPass, pass),
	}
	out, err := cmd.Output()
	if err != nil {
		return "", err
	}
	for _, line := range strings.Split(string(out), "\n") {
		if !sha256Line.MatchString(line) {
			continue
		}
		value := ""
		if parts := strings.Split(line, fieldSeparator); len(parts) > 1 {
			value = parts[1]
		}
		return strings.ToLower(strings.NewReplacer(":", "", " ", "", "\t", "", "\r", "").Replace(value)), nil
	}
	return "", nil
}

// with runs command with the keystore copy in its environment, and returns its status.
func (a *app) with(command []string) int {
	// The handler is installed before the copy exists, so a signal during the copy still shreds it.
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	defer signal.Stop(sigs)

	key, err := a.materialize()
	defer key.cleanup(a)
	if err != nil {
		return a.fail(err)
	}
	select {
	case sig := <-sigs:
		return a.interrupted(sig)
	default:
	}

	path, ok := lookPath(command[0], a.getenv(envPath))
	if !ok {
		fmt.Fprintf(a.stderr, "%s: command not found\n", command[0])
		a.infof("command_exit", "command exit=%d", exitNotFound)
		return exitNotFound
	}
	cmd := &exec.Cmd{
		Path:   path,
		Args:   command,
		Env:    key.environment(a.environ),
		Stdout: a.stdout,
		Stderr: a.stderr,
	}
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(a.stderr, "%s: cannot run: %v\n", command[0], err)
		return exitNotRun
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		status := exitStatus(err)
		a.infof("command_exit", "command exit=%d", status)
		return status
	case sig := <-sigs:
		// The command is stopped first, so the script does not outlive it; the copy is shredded by cleanup.
		_ = cmd.Process.Signal(syscall.SIGTERM)
		<-done
		return a.interrupted(sig)
	}
}

// interrupted is the status of a run a signal stopped: the signal's conventional code, with the copy already shredded
// by the deferred cleanup.
func (a *app) interrupted(sig os.Signal) int {
	code := signalExitBase
	if s, ok := sig.(syscall.Signal); ok {
		code += int(s)
	}
	a.log.Warn("signal", "signal", sig.String(), "exit", code)
	fmt.Fprintf(a.stderr, "%sstopped by %s: removing the keystore copy\n", logPrefix, sig)
	return code
}

// exitStatus is the status of a command that ended: its exit code, or 128 plus the signal that killed it, the status
// a shell reports for it.
func exitStatus(err error) int {
	if err == nil {
		return exitOK
	}
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		if ws, ok := ee.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
			return signalExitBase + int(ws.Signal())
		}
		return ee.ExitCode()
	}
	return exitRefused
}

// keyCopy is the private copy of the keystore for one run: its directory, and the file in it.
type keyCopy struct {
	dir, path, pass string
}

// environment is the command's environment: the caller's, with the three NEVR_QUEST_KS* variables set.
func (k *keyCopy) environment(environ []string) []string {
	if k == nil {
		return environ
	}
	env := setEnv(environ, envKS, k.path)
	env = setEnv(env, envKSAlias, alias)
	return setEnv(env, envKSPass, k.pass)
}

// cleanup shreds the copy and removes its directory. It runs on every exit path, as a defer.
func (k *keyCopy) cleanup(a *app) {
	if k == nil || k.dir == "" {
		return
	}
	if _, err := os.Stat(k.path); err == nil {
		// shred -u, and a plain remove when shred is not available or fails: the key file never stays.
		if !shred(a, k.path) {
			_ = os.Remove(k.path)
		}
	}
	_ = os.RemoveAll(k.dir)
}

// shred overwrites and unlinks path with the shred command; false when it could not do so.
func shred(a *app, path string) bool {
	sh, ok := lookPath(shredName, a.getenv(envPath))
	if !ok {
		return false
	}
	cmd := &exec.Cmd{Path: sh, Args: []string{shredName, "-u", "--", path}, Env: a.environ}
	return cmd.Run() == nil
}

// materialize copies the keystore into a private file and checks the copy opens. The returned copy is never nil, and
// the caller always runs its cleanup, so a failed copy leaves no file behind.
func (a *app) materialize() (*keyCopy, error) {
	ks, pass, err := a.inputs()
	if err != nil {
		return &keyCopy{}, err
	}
	base := a.getenv(envRuntimeDir)
	if !writableDir(base) {
		base = fallbackDir
	}
	dir, err := a.mkdirTemp(base, keyDirPattern)
	if err != nil && base != fallbackDir {
		base = fallbackDir
		dir, err = a.mkdirTemp(base, keyDirPattern)
	}
	if err != nil {
		return &keyCopy{}, refusal{"tmpdir", fmt.Sprintf("cannot create a private temporary directory under %s", base)}
	}
	key := &keyCopy{dir: dir, path: filepath.Join(dir, keyFileName), pass: pass}
	if err := os.Chmod(dir, keyDirMode); err != nil {
		return key, refusal{"tmpdir", fmt.Sprintf("cannot make the private temporary directory %s private", dir)}
	}
	if err := copyFile(ks, key.path); err != nil {
		return key, refusal{"copy", fmt.Sprintf("cannot copy %s into a private file", envKeystore)}
	}
	if err := a.openCheck(key.path, pass); err != nil {
		return key, err
	}
	a.infof("keystore_ready", "keystore ready alias=%s", alias)
	return key, nil
}

// copyFile copies src to a new file dst that only its owner can read, created exclusively.
func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, keyFileMode)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// readableFile reports whether path is a regular file this process can open for reading.
func readableFile(path string) bool {
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	return f.Close() == nil
}

// writableDir reports whether path names a directory this process can write to.
func writableDir(path string) bool {
	if path == "" {
		return false
	}
	info, err := os.Stat(path)
	if err != nil || !info.IsDir() {
		return false
	}
	return syscall.Access(path, accessWrite) == nil
}

// lookPath finds name on pathEnv (the PATH the caller gave), as a shell does: an empty element is the current
// directory, and a name with a slash is used as it is.
func lookPath(name, pathEnv string) (string, bool) {
	if strings.Contains(name, "/") {
		return name, isExecutable(name)
	}
	for _, dir := range filepath.SplitList(pathEnv) {
		if dir == "" {
			dir = "."
		}
		if p := filepath.Join(dir, name); isExecutable(p) {
			return p, true
		}
	}
	return "", false
}

func isExecutable(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular() && info.Mode().Perm()&execBits != 0
}

// envLookup reads a variable from an environment list: the last value when a key appears twice, "" when absent.
func envLookup(environ []string) func(string) string {
	return func(key string) string {
		value := ""
		for _, kv := range environ {
			if k, v, ok := strings.Cut(kv, "="); ok && k == key {
				value = v
			}
		}
		return value
	}
}

// setEnv returns environ with key set to value, the old entries for key removed.
func setEnv(environ []string, key, value string) []string {
	out := make([]string, 0, len(environ)+1)
	for _, kv := range environ {
		if k, _, ok := strings.Cut(kv, "="); !ok || k != key {
			out = append(out, kv)
		}
	}
	return append(out, key+"="+value)
}
