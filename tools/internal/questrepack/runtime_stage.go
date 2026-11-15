package questrepack

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// runtimeName is the baseline runtime the shim maps at start from the APK's native library folder (quest-update piece
// shim-loads-baseline; src/quest/shim/runtime_load.cpp). The name must equal runtime_load.h's kBaselineFileName.
const runtimeName = "libnevr-runtime.so"

// runtimeEntry is the APK entry of the baseline runtime.
const runtimeEntry = abiDir + "/" + runtimeName

// StageRuntime stages the baseline runtime at runtimePath under stage as lib/arm64-v8a/libnevr-runtime.so and returns
// the problems; it writes nothing when there are any. The file must be a non-empty regular file, and the store APK must
// not hold a library of that name: the baseline would replace it, and a recipe never replaces a store library by a file
// of its own.
func StageRuntime(runtimePath, stage, storeAPK string) []string {
	info, err := os.Stat(runtimePath)
	if err != nil || !info.Mode().IsRegular() || info.Size() == 0 {
		return []string{fmt.Sprintf("%s: not a non-empty file; the baseline runtime is the sentinel's %s, built beside the shim", runtimePath, runtimeName)}
	}
	store, err := openZip(storeAPK)
	if err != nil {
		return []string{fmt.Sprintf("%s: cannot be read as the store APK: %v", storeAPK, err)}
	}
	defer store.Close()
	var taken []string
	for _, entry := range libEntries(store) {
		if baseName(entry) == runtimeName {
			taken = append(taken, entry)
		}
	}
	if len(taken) > 0 {
		return []string{fmt.Sprintf("%s already holds %s: the baseline runtime would replace it", filepath.Base(storeAPK), strings.Join(taken, ", "))}
	}
	data, err := os.ReadFile(runtimePath)
	if err != nil {
		return []string{fmt.Sprintf("%s: %v", runtimePath, err)}
	}
	target := filepath.Join(stage, abiDir)
	if err := os.MkdirAll(target, 0o755); err != nil {
		return []string{fmt.Sprintf("%s: %v", target, err)}
	}
	if err := os.WriteFile(filepath.Join(target, runtimeName), data, 0o644); err != nil {
		return []string{fmt.Sprintf("%s: %v", runtimeName, err)}
	}
	fmt.Printf("quest_repack_libs: baseline runtime %s sha256 staged %s\n", runtimeEntry, sha256Hex(data))
	return nil
}
