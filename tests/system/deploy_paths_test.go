package system

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// The host loads nevr-runtime.dll from the nevr root of the Wine prefix and never from beside itself (ADR 0012), so
// the deployment helpers put the runtime there and the host in bin/win10. This runs with no game and no build.
func TestDeploy_RuntimeGoesToTheNevrRootAndHostToTheGameDirectory(t *testing.T) {
	gameDir := t.TempDir()
	src := t.TempDir()
	runtimeSrc := filepath.Join(src, runtimeDLLName)
	hostSrc := filepath.Join(src, "BugSplat64.dll")
	require.NoError(t, os.WriteFile(runtimeSrc, []byte("RUNTIME"), 0o644))
	require.NoError(t, os.WriteFile(hostSrc, []byte("HOST"), 0o644))

	require.NoError(t, deployDLL(runtimeSrc, gameDir))
	require.NoError(t, deployDLL(hostSrc, gameDir))

	root := nevrRootDir(gameDir)
	assert.Contains(t, filepath.ToSlash(root), ".wineprefix/drive_c/users/")
	assert.True(t, filepath.IsAbs(root) == filepath.IsAbs(gameDir))
	assert.Equal(t, filepath.Join(root, runtimeDLLName), deployedPath(gameDir, runtimeDLLName))

	got, err := os.ReadFile(filepath.Join(root, runtimeDLLName))
	require.NoError(t, err, "the runtime must be in the nevr root")
	assert.Equal(t, "RUNTIME", string(got))
	got, err = os.ReadFile(filepath.Join(gameDir, "bin", "win10", "BugSplat64.dll"))
	require.NoError(t, err, "the host must be in bin/win10")
	assert.Equal(t, "HOST", string(got))
	_, err = os.Stat(filepath.Join(gameDir, "bin", "win10", runtimeDLLName))
	assert.True(t, os.IsNotExist(err), "nothing named nevr-runtime.dll is deployed beside the host")

	require.NoError(t, cleanupDLLs(gameDir))
	_, err = os.Stat(filepath.Join(root, runtimeDLLName))
	assert.True(t, os.IsNotExist(err), "cleanup removes the runtime from the nevr root")
	_, err = os.Stat(filepath.Join(gameDir, "bin", "win10", "BugSplat64.dll"))
	assert.True(t, os.IsNotExist(err), "cleanup removes the host")
}
