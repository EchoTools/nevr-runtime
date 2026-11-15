package contracts

import "testing"

func TestSlowLoadTriggerIsOverThirtySeconds(t *testing.T) {
	if SlowLoadFires(30) || !SlowLoadFires(30.1) {
		t.Fatal("the trigger fires strictly over 30 s")
	}
}

func TestSlowLoadNoticeOncePerNSessions(t *testing.T) {
	const n = 10
	if !NoticeDue(false, 0, n) {
		t.Error("a player never told must be told on the first fire")
	}
	if NoticeDue(true, 0, n) {
		t.Error("a second fire in the same session must not repeat the notice (never per round)")
	}
	if NoticeDue(true, n-1, n) {
		t.Error("fewer than N sessions since the last notice must not repeat it")
	}
	if !NoticeDue(true, n, n) {
		t.Error("N sessions since the last notice may repeat it")
	}
}

func TestSlowLoadNoticeShape(t *testing.T) {
	good := `{"schema":1,"kind":"sign_game_files","url":"https://github.com/EchoTools/selfsign-path-tool/releases/latest"}`
	if _, err := ParseSlowLoadNotice([]byte(good)); err != nil {
		t.Fatal(err)
	}
	for name, body := range map[string]string{
		"other link":    `{"schema":1,"kind":"sign_game_files","url":"https://example.com/x"}`,
		"http link":     `{"schema":1,"kind":"sign_game_files","url":"http://github.com/EchoTools/selfsign-path-tool/releases/latest"}`,
		"other kind":    `{"schema":1,"kind":"x","url":"https://github.com/EchoTools/selfsign-path-tool/releases/latest"}`,
		"unknown field": `{"schema":1,"kind":"sign_game_files","url":"https://github.com/EchoTools/selfsign-path-tool/releases/latest","cmd":"x"}`,
	} {
		if _, err := ParseSlowLoadNotice([]byte(body)); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}
