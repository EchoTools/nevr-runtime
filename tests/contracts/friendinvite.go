package contracts

import "errors"

// #1875: the friend-list invite button follows the friend's presence, which the game service reports. In the game: the
// blue +, the game's own invite. Only on Discord (online or idle): a different color, and the press asks the service to
// send the friend an invite code as a Discord message. Offline everywhere (or a presence this client does not know): no
// button. The service sends the Discord message; the runtime never holds a Discord credential.
type InviteButton int

const (
	ButtonNone InviteButton = iota
	ButtonBluePlus
	ButtonDiscordCode
)

func ButtonFor(presence string) InviteButton {
	switch presence {
	case "in_game":
		return ButtonBluePlus
	case "discord_online", "discord_idle":
		return ButtonDiscordCode
	}
	return ButtonNone
}

type InviteRequest struct {
	Schema       int    `json:"schema"`
	TargetUserID string `json:"target_user_id"`
	Via          string `json:"via"` // "game" | "discord"
}

// InviteRequestFor builds the request a press sends; ok is false when the friend gets no button.
func InviteRequestFor(presence, targetUserID string) (InviteRequest, bool) {
	switch ButtonFor(presence) {
	case ButtonBluePlus:
		return InviteRequest{1, targetUserID, "game"}, true
	case ButtonDiscordCode:
		return InviteRequest{1, targetUserID, "discord"}, true
	}
	return InviteRequest{}, false
}

func ParseInviteRequest(b []byte) (InviteRequest, error) {
	var r InviteRequest
	if err := strictDecode(b, &r); err != nil {
		return r, err
	}
	if r.Schema != 1 || !uuidRe.MatchString(r.TargetUserID) || (r.Via != "game" && r.Via != "discord") {
		return r, errors.New("not an invite request")
	}
	return r, nil
}
