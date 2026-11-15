package patchwiring

import (
	"strings"
	"testing"
)

const (
	pcStart    = "src/runtime/lifecycle/cdn_start.cpp"
	questCfg   = "src/quest/sentinel/quest_config.cpp"
	sharedRead = "src/cdn/cdn_config.h"
)

// The PC's CDN settings are config.yaml's `assets.*` keys, read by the one reader the Quest uses (ADR 0045 decision 9), and the
// settings reach the run's configuration; a fixed empty Settings{} is the state this replaced.
func TestThePcCdnStartReadsItsSettingsFromConfigYamlThroughTheSharedReader(t *testing.T) {
	c := code(t, pcStart)
	start := c[strings.Index(c, "bool Start() noexcept {"):]
	// The client's start only: StartServer keeps the code's defaults (its label defaults are the server's own, ADR 0045 decision 9).
	if end := strings.Index(start, "bool StartServer() noexcept {"); end >= 0 {
		start = start[:end]
	}
	before(t, start, "nevr_cdn::ReadSettings(NevrCfgLoadedConfig())", "nevr_cdn::MakeRunConfig(settings")
	if strings.Contains(start, "nevr_cdn::Settings settings{}") {
		t.Errorf("%s: the client's Start builds a fixed Settings{} again", pcStart)
	}
	before(t, start, "read.warnings", "nevr_cdn::MakeRunConfig(settings")
	if !strings.Contains(start, `Log(EchoVR::LogLevel::Warning, "[NEVR.CONFIG] cdn %s", warning.c_str())`) {
		t.Errorf("%s does not log each warning of the reader at Warning, tagged [NEVR.CONFIG]", pcStart)
	}
	// Start is noexcept: an exception from the reader would end the process, so it is caught by type and the run goes on
	// on the defaults, with one Warning that says so.
	before(t, start, "catch (const std::exception&) {", "nevr_cdn::MakeRunConfig(settings")
	before(t, start, "catch (const std::exception&) {", `"[NEVR.CONFIG] cdn config read failed reason=exception`)
}

// One code path: the Quest reads the same four keys through the shared reader and keeps only its own `assets.whole_game`.
func TestTheQuestReadsTheCdnKeysThroughTheSharedReaderAndHoldsNoCopyOfIt(t *testing.T) {
	q := code(t, questCfg)
	if !strings.Contains(q, "nevr_cdn::ReadSettings(cfg)") {
		t.Errorf("%s does not read the CDN keys through nevr_cdn::ReadSettings", questCfg)
	}
	for _, own := range []string{"bool NormalizeCdnBase(", "bool IsManifestPath(", "bool IsAssetLabel(", `"assets.cdn_url"`, `"assets.manifest"`, `"assets.labels"`, `"assets.required_only"`} {
		if strings.Contains(q, own) {
			t.Errorf("%s holds its own copy of the CDN reader: %s", questCfg, own)
		}
	}
	r := code(t, sharedRead)
	for _, key := range []string{`"assets.cdn_url"`, `"assets.manifest"`, `"assets.required_only"`, `"assets.labels"`} {
		if !strings.Contains(r, key) {
			t.Errorf("%s does not read %s", sharedRead, key)
		}
	}
}
