package contracts

import (
	"errors"
	"regexp"
)

// #62: the player report, runtime -> game service. The body names who is reported, in which match, why and when. It never
// names the reporter: the service takes the reporter from the authenticated session, so a client cannot report as
// someone else (a field "reporter" is an unknown field and the whole report is refused). The service logs who, about
// whom, match and time, and raises a moderator marker when several different reporters name one player.
const PlayerReportSchema = 1

var reportReasons = map[string]bool{"harassment": true, "cheating": true, "griefing": true, "hate": true, "other": true}

var evrIDRe = regexp.MustCompile(`^[A-Z0-9]+-[A-Z0-9]+-[0-9]+$`)

type PlayerReport struct {
	Schema        int    `json:"schema"`
	ReportedEvrID string `json:"reported_evr_id"`
	MatchID       string `json:"match_id"`
	Reason        string `json:"reason"`
	ReportedAt    string `json:"reported_at"`
}

func ParsePlayerReport(b []byte) (PlayerReport, error) {
	var r PlayerReport
	if err := strictDecode(b, &r); err != nil {
		return r, err
	}
	switch {
	case r.Schema != PlayerReportSchema:
		return r, errors.New("schema must be 1")
	case !evrIDRe.MatchString(r.ReportedEvrID):
		return r, errors.New("reported_evr_id is not an evr id")
	case !uuidRe.MatchString(r.MatchID):
		return r, errors.New("match_id is not a lower-case uuid")
	case !reportReasons[r.Reason]:
		return r, errors.New("reason is not one of harassment|cheating|griefing|hate|other")
	}
	return r, utcZ(r.ReportedAt)
}
