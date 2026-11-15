package contracts

import "testing"

func TestInviteButtonByPresence(t *testing.T) {
	want := map[string]InviteButton{
		"in_game":        ButtonBluePlus,
		"discord_online": ButtonDiscordCode,
		"discord_idle":   ButtonDiscordCode,
		"offline":        ButtonNone,
		"something_new":  ButtonNone,
	}
	for p, b := range want {
		if got := ButtonFor(p); got != b {
			t.Errorf("%s: %v, want %v", p, got, b)
		}
	}
}

func TestInviteRequestCarriesTheChannel(t *testing.T) {
	const id = "7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a"
	r, ok := InviteRequestFor("in_game", id)
	if !ok || r.Via != "game" || r.TargetUserID != id || r.Schema != 1 {
		t.Fatalf("%+v %v", r, ok)
	}
	r, ok = InviteRequestFor("discord_idle", id)
	if !ok || r.Via != "discord" {
		t.Fatalf("%+v %v", r, ok)
	}
	if _, ok = InviteRequestFor("offline", id); ok {
		t.Fatal("an offline friend gets no request")
	}
}

func TestInviteRequestStrict(t *testing.T) {
	if _, err := ParseInviteRequest([]byte(`{"schema":1,"target_user_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","via":"discord"}`)); err != nil {
		t.Fatal(err)
	}
	for _, b := range []string{
		`{"schema":1,"target_user_id":"x","via":"discord"}`,
		`{"schema":1,"target_user_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","via":"sms"}`,
		`{"schema":1,"target_user_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","via":"game","code":"ABC"}`,
	} {
		if _, err := ParseInviteRequest([]byte(b)); err == nil {
			t.Errorf("accepted %s", b)
		}
	}
}
