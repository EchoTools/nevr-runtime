package contracts

import (
	"testing"
	"time"
)

var now = time.Date(2026, 11, 12, 12, 0, 0, 0, time.UTC)

func TestCreateGameMenuClosedWhileSuspended(t *testing.T) {
	if CreateGameMenuMayOpen("2026-11-12T13:00:00Z", now) {
		t.Fatal("the create-game menu opened over an active suspension notice")
	}
}

func TestCreateGameMenuOpensAfterSuspension(t *testing.T) {
	if !CreateGameMenuMayOpen("2026-11-12T11:59:59Z", now) {
		t.Error("an ended suspension must not block the menu")
	}
	if !CreateGameMenuMayOpen("", now) {
		t.Error("no suspension: the menu opens")
	}
}

func TestCreateGameMenuFailsClosedOnAnUnreadableEnd(t *testing.T) {
	if CreateGameMenuMayOpen("tomorrow", now) {
		t.Fatal("a suspension end that cannot be read must keep the menu closed")
	}
}

func TestCreateGameMenuOpensAtTheExactEnd(t *testing.T) {
	if !CreateGameMenuMayOpen("2026-11-12T12:00:00Z", now) {
		t.Fatal("at the end instant the suspension is over")
	}
}
