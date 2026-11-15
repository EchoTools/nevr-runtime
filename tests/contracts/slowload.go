package contracts

import "errors"

// #446: the slow-load notice. The game service owns the trigger (the LOAD_STATS remote log's client_load_time, the field
// the existing RemoteLogLoadStats trigger reads, docs/engine/remote-log.md) and the per-player history across sessions,
// so the service decides and sends; the runtime only shows what it is sent, once. The trigger is over 30 s, the notice is
// at most once per N sessions (never per round), and N is the service's setting.
const (
	SlowLoadSeconds   = 30.0
	SlowLoadNoticeURL = "https://github.com/EchoTools/selfsign-path-tool/releases/latest"
	SlowLoadKind      = "sign_game_files"
)

func SlowLoadFires(clientLoadTimeSeconds float64) bool {
	return clientLoadTimeSeconds > SlowLoadSeconds
}

// NoticeDue says whether a fire sends the notice. everShown: the player was told before. sessionsSince: sessions started
// since that notice (0 is the same session).
func NoticeDue(everShown bool, sessionsSince, n int) bool {
	return !everShown || sessionsSince >= n
}

type SlowLoadNotice struct {
	Schema int    `json:"schema"`
	Kind   string `json:"kind"`
	URL    string `json:"url"`
}

func ParseSlowLoadNotice(b []byte) (SlowLoadNotice, error) {
	var n SlowLoadNotice
	if err := strictDecode(b, &n); err != nil {
		return n, err
	}
	if n.Schema != 1 || n.Kind != SlowLoadKind {
		return n, errors.New("not a slow-load notice")
	}
	if n.URL != SlowLoadNoticeURL {
		return n, errors.New("the notice links only to the latest selfsign-path-tool release")
	}
	return n, nil
}
