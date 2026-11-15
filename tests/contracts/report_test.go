package contracts

import "testing"

const goodReport = `{"schema":1,"reported_evr_id":"OVR-ORG-3963667097037078","match_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","reason":"harassment","reported_at":"2026-11-12T10:00:00Z"}`

func TestPlayerReportAccepts(t *testing.T) {
	r, err := ParsePlayerReport([]byte(goodReport))
	if err != nil {
		t.Fatal(err)
	}
	if r.ReportedEvrID != "OVR-ORG-3963667097037078" || r.Reason != "harassment" {
		t.Fatalf("fields lost: %+v", r)
	}
}

func TestPlayerReportRefuses(t *testing.T) {
	cases := map[string]string{
		"reporter named by the client": `{"schema":1,"reporter":"OVR-ORG-1","reported_evr_id":"OVR-ORG-2","match_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","reason":"cheating","reported_at":"2026-11-12T10:00:00Z"}`,
		"unknown reason":               `{"schema":1,"reported_evr_id":"OVR-ORG-2","match_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","reason":"spam","reported_at":"2026-11-12T10:00:00Z"}`,
		"wrong schema":                 `{"schema":2,"reported_evr_id":"OVR-ORG-2","match_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","reason":"cheating","reported_at":"2026-11-12T10:00:00Z"}`,
		"match id not a uuid":          `{"schema":1,"reported_evr_id":"OVR-ORG-2","match_id":"x","reason":"cheating","reported_at":"2026-11-12T10:00:00Z"}`,
		"time not UTC Z":               `{"schema":1,"reported_evr_id":"OVR-ORG-2","match_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","reason":"cheating","reported_at":"2026-11-12T10:00:00+02:00"}`,
		"no reported player":           `{"schema":1,"match_id":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","reason":"cheating","reported_at":"2026-11-12T10:00:00Z"}`,
		"not json":                     `report`,
	}
	for name, body := range cases {
		if _, err := ParsePlayerReport([]byte(body)); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}
