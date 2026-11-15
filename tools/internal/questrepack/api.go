package questrepack

import "time"

// The exported surface the command and the tests use. Each function is the check or stage of one subcommand of
// quest-repack-libs; the problems it returns are the lines the command prints, and an empty result is a pass.

// DefaultPlanHeader is the data step's header (the pack's pins), relative to the checkout the recipe runs in.
const DefaultPlanHeader = planHeaderPath

// PatcherTimeoutDefault is how long the libr15 patcher may run before it is a hang.
const PatcherTimeoutDefault time.Duration = patcherTimeoutDefault

// CheckPluginName is check-plugin-name: what the plugin's file name would replace in the store APK.
func CheckPluginName(apk, plugin string) ([]string, error) { return checkPluginName(apk, plugin) }

// CheckPluginBuildIDs is check-plugin-build-ids: the loader's build-ID rule for the plugin against the store libr15.
func CheckPluginBuildIDs(apk, plugin string) ([]string, error) {
	return checkPluginBuildIDs(apk, plugin)
}

// ShimRowsProblems is check-shim-rows; extraRows "" when the library is patched without rows.
func ShimRowsProblems(shim, extraRows string) []string { return shimRowsProblems(shim, extraRows) }

// WritePluginManifest is write-plugin-manifest.
func WritePluginManifest(stage string, plugins, early, off []string) []string {
	return writePluginManifest(stage, plugins, early, off)
}

// PluginManifestProblems is check-plugin-manifest.
func PluginManifestProblems(stage string) []string { return pluginManifestProblems(stage) }

// BuildVariantProblems is check-build-variant.
func BuildVariantProblems(word string) []string { return buildVariantProblems(word) }

// WriteBuildVariant is write-build-variant.
func WriteBuildVariant(stage, word string) []string { return writeBuildVariant(stage, word) }

// PackProblems is pack-problems: the staged pack as the runtime reads it.
func PackProblems(stage, planHeader string) []string { return packProblems(stage, planHeader) }
