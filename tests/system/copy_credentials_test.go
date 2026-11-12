package system

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/require"
)

// fakeGameCopy makes a fake copy's echovr/ directory with the folders a sign-in file lives in: _local and the nevr
// root of a Wine prefix. It returns the echovr/ path.
func fakeGameCopy(t *testing.T) string {
	t.Helper()
	echovr := filepath.Join(t.TempDir(), "echovr")
	require.NoError(t, os.MkdirAll(filepath.Join(echovr, "_local"), 0o755))
	require.NoError(t, os.MkdirAll(filepath.Join(echovr, ".wineprefix", "drive_c", "users", "tester",
		"AppData", "Local", "nevr", "echovr"), 0o755))
	return echovr
}

func TestCopyCredentials_FakeCopyWithSignInFilesIsListed(t *testing.T) {
	echovr := fakeGameCopy(t)
	local := filepath.Join(echovr, "_local", ".credentials.json")
	require.NoError(t, os.WriteFile(local, []byte("{}"), 0o600))
	// A backup in the nevr root, and a dangling link under the same name: both count, as find counts them.
	root := filepath.Join(echovr, ".wineprefix", "drive_c", "users", "tester", "AppData", "Local", "nevr", "echovr")
	backup := filepath.Join(root, ".credentials.json.prod-bak")
	require.NoError(t, os.WriteFile(backup, []byte("{}"), 0o600))
	link := filepath.Join(root, ".credentials.json")
	require.NoError(t, os.Symlink(filepath.Join(t.TempDir(), "missing"), link))

	found, err := copyCredentialFiles(echovr)
	require.NoError(t, err)
	require.ElementsMatch(t, []string{local, backup, link}, found)
}

func TestCopyCredentials_CleanCopyIsNotRefused(t *testing.T) {
	echovr := fakeGameCopy(t)
	// A directory with the pattern's name is not a sign-in file.
	require.NoError(t, os.MkdirAll(filepath.Join(echovr, "_local", ".credentials.json.d"), 0o755))

	found, err := copyCredentialFiles(echovr)
	require.NoError(t, err)
	require.Empty(t, found)

	t.Setenv(envGameDir, echovr)
	require.NoError(t, checkCopyCredentials())
}

func TestCopyCredentials_SignInInTheCopyRefusesTheRun(t *testing.T) {
	echovr := fakeGameCopy(t)
	signIn := filepath.Join(echovr, "_local", ".credentials.json")
	require.NoError(t, os.WriteFile(signIn, []byte("{}"), 0o600))

	t.Setenv(envGameDir, echovr)
	err := checkCopyCredentials()
	require.Error(t, err)
	require.Contains(t, err.Error(), signIn)
}

func TestCopyCredentials_UnsearchableCopyIsAnErrorNotAClean(t *testing.T) {
	t.Setenv(envGameDir, filepath.Join(t.TempDir(), "no-such-copy"))
	require.Error(t, checkCopyCredentials())
}

func TestCopyCredentials_PatternComesFromTheSharedFile(t *testing.T) {
	pattern, err := copyCredentialPattern()
	require.NoError(t, err)
	require.Equal(t, ".credentials.json*", pattern)
}

func TestCopyCredentials_UnsetIsTheOwnersInstallAndIsNotSearched(t *testing.T) {
	// The main checkout's own install holds the owner's sign-in, so an unset EVR_GAME_DIR refuses nothing.
	t.Setenv(envGameDir, "")
	require.NoError(t, checkCopyCredentials())
}
