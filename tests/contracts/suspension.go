package contracts

import "time"

// #1987: while a suspension notice is up, the create-game menu does not open over it. The input is the suspension's end
// as the notice already carries it ("" when none). The server half is nakama #765 (a suspended player cannot create); this
// is the client's half, so the notice stays readable. An end that cannot be read keeps the menu closed: the notice is the
// safe screen.
func CreateGameMenuMayOpen(suspensionEndsAt string, now time.Time) bool {
	if suspensionEndsAt == "" {
		return true
	}
	end, err := time.Parse(time.RFC3339, suspensionEndsAt)
	if err != nil {
		return false
	}
	return !now.Before(end)
}
