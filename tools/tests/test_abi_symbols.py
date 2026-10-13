"""Every constant in src/abi/symbols.h is the hash of a name, in the namespace whose hash produced it.

The header is plain literals. A mistyped digit, a constant moved to the wrong namespace and a name that no
longer hashes to its value all stay silent at compile time, and the wire value is what the game compares.
This test holds the name string of every constant and recomputes the hash:

  - `EchoVR::Symbols` (top level): CMatSym::Hash of the game's message-type string. The 184 strings are the
    registry of rad-protocol/src/message_registry.rs in nevr-server-rs (`REGISTRY` below).
  - `EchoVR::Symbols::Social` and `EchoVR::Symbols::Tcp`: CSymbol64 (src/abi/symbol_hash.h) of the string.
  - `NEVRProtobufJSONMessageV1`, at top level, is the one CSymbol64 constant outside those two namespaces.
  - `ProtobufMsg` and `ProtobufJson` have no known source string: no registry or corpus name reproduces them
    under either hash, so they are pinned by value and nothing else.

The hash implementations are the ones in tools/gen_symbol_corpus.py. `test_hash_known_answers` ties them to
values the game binary produced (rad-protocol/src/symbol/tests.rs, src/abi/symbol_hash.h).
"""
import importlib.util
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
U64 = (1 << 64) - 1

# identifier -> the string CMatSym::Hash is applied to
MATSYM_TOP = {
    'LobbyRegistrationSuccess': 'SNSLobbyRegistrationSuccess',
    'LobbyRegistrationFailure': 'SNSLobbyRegistrationFailure',
    'LobbySessionStarting': 'SNSLobbySessionStarting',
    'LobbySessionError': 'SNSLobbySessionError',
    'LobbyTerminateProcess': 'SNSLobbyTerminateProcess',
    'LobbyStartSessionV4': 'SNSLobbyStartSessionv4',
    'LobbyJoinRequestedV4': 'SNSLobbyJoinRequestv4',
    'LobbyAddEntrantRequestV4': 'SNSLobbyAddEntrantRequestv4',
    'LobbySessionSuccessV5': 'SNSLobbySessionSuccessv5',
    'LobbyAcceptPlayersSuccessV2': 'SNSLobbyAcceptPlayersSuccessv2',
    'LobbyAcceptPlayersFailureV2': 'SNSLobbyAcceptPlayersFailurev2',
    'LobbySmiteEntrant': 'SNSLobbySmiteEntrant',
    'LobbyChatEntry': 'SNSLobbyChatEntry',
    'LobbyVoiceEntry': 'SNSLobbyVoiceEntry',
    'SaveLoadoutRequest': 'SR15NetSaveLoadoutRequest',
    'SaveLoadoutSuccess': 'SR15NetSaveLoadoutSuccess',
    'SaveLoadoutPartial': 'SR15NetSaveLoadoutPartial',
    'CurrentLoadoutRequest': 'SR15NetCurrentLoadoutRequest',
    'CurrentLoadoutResponse': 'SR15NetCurrentLoadoutResponse',
    'RefreshProfileForUser': 'SNSRefreshProfileForUser',
    'RefreshProfileFromServer': 'SNSRefreshProfileFromServer',
    'LobbySendClientLobbySettings': 'SNSLobbySendClientLobbySettings',
    'TierRewardMsg': 'SR15NetTierRewardMsg',
    'TopAwardsMsg': 'SR15NetTopAwardsMsg',
    'NewUnlocks': 'SR15NetNewUnlocks',
    'ReliableStatUpdate': 'SR15NetReliableStatUpdate',
    'ReliableTeamStatUpdate': 'SR15NetReliableTeamStatUpdate',
    'BroadcasterConnectEvent': 'SBroadcasterConnectEvent',
    'BroadcasterConnectFailedEvent': 'SBroadcasterConnectFailedEvent',
    'BroadcasterDisconnectEvent': 'SBroadcasterDisconnectEvent',
    'BroadcasterEnableCompressionEvent_1': 'SBroadcasterEnableCompressionEvent_1',
    'BroadcasterIntroduceAckEvent': 'SBroadcasterIntroduceAckEvent',
    'BroadcasterIntroduceFailEvent': 'SBroadcasterIntroduceFailEvent',
    'BroadcasterIntroduceFinEvent': 'SBroadcasterIntroduceFinEvent',
    'BroadcasterIntroduceReqEvent': 'SBroadcasterIntroduceReqEvent',
    'BroadcasterMailboxBatchEvent': 'SBroadcasterMailboxBatchEvent',
    'BroadcasterMsgEncodingFailEvent': 'SBroadcasterMsgEncodingFailEvent',
    'BroadcasterMsgEncodingSupportEvent_1': 'SBroadcasterMsgEncodingSupportEvent_1',
    'BroadcasterPingUpdate': 'SBroadcasterPingUpdate',
    'BroadcasterReliableBatchEvent': 'SBroadcasterReliableBatchEvent',
    'BroadcasterRequestEncodingResetEvent': 'SBroadcasterRequestEncodingResetEvent',
    'BroadcasterResetEncodingEvent': 'SBroadcasterResetEncodingEvent',
    'LobbyAddEntrantAcceptedV2': 'SNSLobbyAddEntrantAcceptedv2',
    'LobbyAddEntrantRejectedV2': 'SNSLobbyAddEntrantRejectedv2',
    'LobbyDirectoryJson': 'SNSLobbyDirectoryJson',
    'LobbyEntrantAccepted': 'SNSLobbyEntrantAccepted',
    'LobbyEntrantAdded': 'SNSLobbyEntrantAdded',
    'LobbyEntrantDataChanged': 'SNSLobbyEntrantDataChanged',
    'LobbyEntrantRemoved': 'SNSLobbyEntrantRemoved',
    'LobbyEntrantStateV2': 'SNSLobbyEntrantStatev2',
    'LobbyEntrantTeamChanged': 'SNSLobbyEntrantTeamChanged',
    'LobbyHostConnectFailed': 'SNSLobbyHostConnectFailed',
    'LobbyHostConnected': 'SNSLobbyHostConnected',
    'LobbyHostDataChanged': 'SNSLobbyHostDataChanged',
    'LobbyHostDisconnected': 'SNSLobbyHostDisconnected',
    'LobbyHostState': 'SNSLobbyHostState',
    'LobbyJoinAcceptedV2': 'SNSLobbyJoinAcceptedv2',
    'LobbyJoinComplete': 'SNSLobbyJoinComplete',
    'LobbyJoinRejected': 'SNSLobbyJoinRejected',
    'LobbyJoinRejectedInternal': 'SNSLobbyJoinRejectedInternal',
    'LobbyJoinSetup': 'SNSLobbyJoinSetup',
    'LobbyLeft': 'SNSLobbyLeft',
    'LobbyLockEntrants': 'SNSLobbyLockEntrants',
    'LobbyMatchmakerStatusReceived': 'SNSLobbyMatchmakerStatusReceived',
    'LobbyOwnerDataChanged': 'SNSLobbyOwnerDataChanged',
    'LobbyOwnerState': 'SNSLobbyOwnerState',
    'LobbyPassOwnership': 'SNSLobbyPassOwnership',
    'LobbyRemovingEntrant': 'SNSLobbyRemovingEntrant',
    'LobbySessionFailureV4': 'SNSLobbySessionFailurev4',
    'LobbySetSpawnBotOnServer': 'SNSLobbySetSpawnBotOnServer',
    'LobbySettingsResponse': 'SNSLobbySettingsResponse',
    'LobbyUnlockEntrants': 'SNSLobbyUnlockEntrants',
    'LobbyUpdatePings': 'SNSLobbyUpdatePings',
    'AbilityActivate': 'SR15NetAbilityActivate',
    'AbilityCooldown': 'SR15NetAbilityCooldown',
    'ActivateExplosion': 'SR15NetActivateExplosion',
    'AutoTargetFireBullet': 'SR15NetAutoTargetFireBullet',
    'BalanceSettingsClientReady': 'SR15NetBalanceSettingsClientReady',
    'BalanceSettingsMassUpdate': 'SR15NetBalanceSettingsMassUpdate',
    'BalanceSettingsUpdate': 'SR15NetBalanceSettingsUpdate',
    'BitFieldRequest': 'SR15NetBitFieldRequest',
    'BitFieldResponse': 'SR15NetBitFieldResponse',
    'BotActivateScenario': 'SR15NetBotActivateScenario',
    'BotDisableScenario': 'SR15NetBotDisableScenario',
    'BotRPC': 'SR15NetBotRPC',
    'BotSetGear': 'SR15NetBotSetGear',
    'CancelGoal': 'SR15NetCancelGoal',
    'CaptureVolumeReset': 'SR15NetCaptureVolumeReset',
    'CaptureVolumeSpoofClaim': 'SR15NetCaptureVolumeSpoofClaim',
    'CaptureVolumeUpdate': 'SR15NetCaptureVolumeUpdate',
    'ClientPingResponseMsg': 'SR15NetClientPingResponseMsg',
    'ComponentEvent': 'SR15NetComponentEvent',
    'CooldownActive': 'SR15NetCooldownActive',
    'CooldownRefund': 'SR15NetCooldownRefund',
    'CooldownRefundRequest': 'SR15NetCooldownRefundRequest',
    'DamageableBulletCollision': 'SR15NetDamageableBulletCollision',
    'DamageableDamageDealt': 'SR15NetDamageableDamageDealt',
    'DamageableDealDamage': 'SR15NetDamageableDealDamage',
    'DamageableHealthBroadcast': 'SR15NetDamageableHealthBroadcast',
    'DamageableLocallyTrackedDamage': 'SR15NetDamageableLocallyTrackedDamage',
    'DamageablePlayerKilled': 'SR15NetDamageablePlayerKilled',
    'DamageablePlayerRespawn': 'SR15NetDamageablePlayerRespawn',
    'DamageablePlayerRespawnComplete': 'SR15NetDamageablePlayerRespawnComplete',
    'DamageableShieldRepaired': 'SR15NetDamageableShieldRepaired',
    'ExplosionNothingHit': 'SR15NetExplosionNothingHit',
    'ExplosionPlayerDamage': 'SR15NetExplosionPlayerDamage',
    'FrisbeeServerGoaliePingWindowReliable': 'SR15NetFrisbeeServerGoaliePingWindowReliable',
    'GameActivateHealBubbleMsg': 'SR15NetGameActivateHealBubbleMsg',
    'GameCancelFireGunMsg': 'SR15NetGameCancelFireGunMsg',
    'GameDepositMagazine': 'SR15NetGameDepositMagazine',
    'GameDestroyMagazine': 'SR15NetGameDestroyMagazine',
    'GameFireGun': 'SR15NetGameFireGun',
    'GameInputFireGunMsg': 'SR15NetGameInputFireGunMsg',
    'GameInputPullPinMsg': 'SR15NetGameInputPullPinMsg',
    'GameReplicateFireGunMsg': 'SR15NetGameReplicateFireGunMsg',
    'GameReplicatePullPinMsg': 'SR15NetGameReplicatePullPinMsg',
    'GameSetMagazineState': 'SR15NetGameSetMagazineState',
    'GameSpawnMagazine': 'SR15NetGameSpawnMagazine',
    'GameSynchronizeCreateMagazine': 'SR15NetGameSynchronizeCreateMagazine',
    'GameplaySensorTriggered': 'SR15NetGameplaySensorTriggered',
    'GameplayTeleportPlayer': 'SR15NetGameplayTeleportPlayer',
    'GameplayTeleportPlayerRequest': 'SR15NetGameplayTeleportPlayerRequest',
    'HealBubbleParamUpdate': 'SR15NetHealBubbleParamUpdate',
    'IdsRequest': 'SR15NetIdsRequest',
    'IdsResponse': 'SR15NetIdsResponse',
    'ItemEquippedNotification': 'SR15NetItemEquippedNotification',
    'LeaderboardMsg': 'SR15NetLeaderboardMsg',
    'LoadoutNumberNotification': 'SR15NetLoadoutNumberNotification',
    'PayloadPosStateUpdateReliable': 'SR15NetPayloadPosStateUpdateReliable',
    'PayloadRequestAutodriveChangeReliable': 'SR15NetPayloadRequestAutodriveChangeReliable',
    'PayloadRequestSpoofedClaimChangeReliable': 'SR15NetPayloadRequestSpoofedClaimChangeReliable',
    'PayloadTrackpointReachedReliable': 'SR15NetPayloadTrackpointReachedReliable',
    'PayloadUpdateMiscStateReliable': 'SR15NetPayloadUpdateMiscStateReliable',
    'PayloadUpdatedAutodriveReliable': 'SR15NetPayloadUpdatedAutodriveReliable',
    'PayloadUpdatedSpoofedClaimersReliable': 'SR15NetPayloadUpdatedSpoofedClaimersReliable',
    'PhysicsSimStateEvent': 'SR15NetPhysicsSimStateEvent',
    'PlayEmote': 'SR15NetPlayEmote',
    'PlayerEquipmentUpdate': 'SR15NetPlayerEquipmentUpdate',
    'PlayerPausedMsg': 'SR15NetPlayerPausedMsg',
    'PlayerStateMsg': 'SR15NetPlayerStateMsg',
    'PooledActorCreate': 'SR15NetPooledActorCreate',
    'PooledActorDestroy': 'SR15NetPooledActorDestroy',
    'PropertyModifiersAdd': 'SR15NetPropertyModifiersAdd',
    'ReliablePostScoreUpdate': 'SR15NetReliablePostScoreUpdate',
    'ReplicatedClientReady': 'SR15NetReplicatedClientReady',
    'ReplicatedDeltaAck': 'SR15NetReplicatedDeltaAck',
    'ReplicatedDeltaUnreliableVars': 'SR15NetReplicatedDeltaUnreliableVars',
    'RequestUserApplyEntrant': 'SR15NetRequestUserApplyEntrant',
    'SaveLoadoutFailure': 'SR15NetSaveLoadoutFailure',
    'SensorPing': 'SR15NetSensorPing',
    'ServerFrameMsg': 'SR15NetServerFrameMsg',
    'ServerInfo': 'SR15NetServerInfo',
    'SetBits': 'SR15NetSetBits',
    'SetOwner': 'SR15NetSetOwner',
    'SocketLink': 'SR15NetSocketLink',
    'SocketUnlink': 'SR15NetSocketUnlink',
    'TetherActivate': 'SR15NetTetherActivate',
    'TetherConnected': 'SR15NetTetherConnected',
    'TetherConnectingUpdate': 'SR15NetTetherConnectingUpdate',
    'TetherConnectionBroken': 'SR15NetTetherConnectionBroken',
    'TetherFoundTarget': 'SR15NetTetherFoundTarget',
    'TetherParamUpdate': 'SR15NetTetherParamUpdate',
    'TouchOwnershipDeclined': 'SR15NetTouchOwnershipDeclined',
    'TouchOwnershipGranted': 'SR15NetTouchOwnershipGranted',
    'TouchOwnershipTransferMailbox': 'SR15NetTouchOwnershipTransferMailbox',
    'TouchOwnershipTransferReliable': 'SR15NetTouchOwnershipTransferReliable',
    'TouchStateEvent': 'SR15NetTouchStateEvent',
    'UIDecSettingMsg': 'SR15NetUIDecSettingMsg',
    'UIIncSettingMsg': 'SR15NetUIIncSettingMsg',
    'UIReplJSONSettings': 'SR15NetUIReplJSONSettings',
    'UIUserChangedSettings': 'SR15NetUIUserChangedSettings',
    'UnreliableStatUpdate': 'SR15NetUnreliableStatUpdate',
    'UserAssignTeamRequest': 'SR15NetUserAssignTeamRequest',
    'UserAssignTeamResponse': 'SR15NetUserAssignTeamResponse',
    'UserPingEstMsg': 'SR15NetUserPingEstMsg',
    'UserVoipEvent': 'SR15NetUserVoipEvent',
    'ForcePushPlayer': 'SR15ForcePushPlayer',
    'RemoveStun': 'SR15RemoveStun',
    'RequestDebugInputPlayback': 'SR15RequestDebugInputPlayback',
    'RequestDebugInputRecording': 'SR15RequestDebugInputRecording',
    'StartDebugInputPlayback': 'SR15StartDebugInputPlayback',
    'StartDebugInputRecording': 'SR15StartDebugInputRecording',
    'StunMsg': 'SR15StunMsg',
    'StunPlayerMsg': 'SR15StunPlayerMsg',
}

# identifier -> the string CSymbol64 is applied to
CSYMBOL64_TOP = {
    'NEVRProtobufJSONMessageV1': 'NEVRProtobufJSONMessageV1',
}

SOCIAL = {
    'FriendSendInviteRequest': 'SNSFriendInviteRequest',
    'FriendRemoveRequest': 'SNSFriendAcceptRequest',
    'FriendActionRequest': 'SNSFriendRemoveRequest',
    'FriendListResponse': 'SNSFriendListResponse',
    'FriendStatusNotify': 'SNSFriendStatusNotify',
    'FriendInviteSuccess': 'SNSFriendInviteSuccess',
    'FriendInviteFailure': 'SNSFriendInviteFailure',
    'FriendInviteNotify': 'SNSFriendInviteNotify',
    'FriendAcceptSuccess': 'SNSFriendAcceptSuccess',
    'FriendAddFailure': 'SNSFriendAcceptFailure',
    'FriendAcceptNotify': 'SNSFriendAcceptNotify',
    'FriendRemoveResponse': 'SNSFriendRemoveResponse',
    'FriendRemoveNotify': 'SNSFriendRemoveNotify',
    'FriendWithdrawnNotify': 'SNSFriendWithdrawnNotify',
    'FriendRejectNotify': 'SNSFriendRejectNotify',
    'PartyKickRequest': 'SNSActivityDailyRewardRequest',
    'PartyPassOwnershipRequest': 'SNSActivityEventRewardRequest',
    'PartyRespondToInvite': 'SNSActivityWeeklyRewardRequest',
    'PartyMemberUpdate': 'SNSPartyCreateRequest',
}

TCP = {
    'LobbyRegistrationSuccess': 'SNSLobbyRegistrationSuccess',
    'LobbyRegistrationFailure': 'SNSLobbyRegistrationFailure',
    'LobbySessionSuccessV5': 'SNSLobbySessionSuccessv5',
    'GameClientMsg1': 'SNSLobbySettingsResponse',
    'GameClientMsg2': 'SNSServerSettingsResponsev2',
    'GameClientMsg3': 'SNSRewardsSettings',
}

# identifier -> value, for constants whose source string is not known
UNSOURCED_TOP = {
    'ProtobufMsg': 0xf62cb25bb5b8a839,
    'ProtobufJson': 0x59f3af95e7d44e72,
}

EXPECTED_NAMESPACES = {"EchoVR::Symbols", "EchoVR::Symbols::Social", "EchoVR::Symbols::Tcp"}


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_symbol_corpus", REPO / "tools/gen_symbol_corpus.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def header_constants():
    """{(namespace, identifier): value as unsigned 64-bit} for every `constexpr SymbolId` in symbols.h."""
    found = {}
    scope = []
    for line in (REPO / "src/abi/symbols.h").read_text().split("\n"):
        code = line.split("//")[0]
        opened = re.match(r"\s*namespace\s+([\w:]+)\s*\{", code)
        if opened:
            scope.append(opened.group(1))
            continue
        if re.match(r"\s*\}", code) and "namespace" in line:
            scope.pop()
            continue
        const = re.match(r"\s*constexpr\s+SymbolId\s+(\w+)\s*=\s*(-?(?:0[xX][0-9a-fA-F]+|\d+))\s*;", code)
        if const:
            key = ("::".join(scope), const.group(1))
            assert key not in found, "duplicate constant %s" % (key,)
            found[key] = int(const.group(2), 0) & U64
    return found


class AbiSymbolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gen = load_generator()
        cls.header = header_constants()

    def expected(self):
        """{(namespace, identifier): (algorithm, string or None, pinned value or None)}"""
        top = "EchoVR::Symbols"
        want = {}
        for ident, text in MATSYM_TOP.items():
            want[(top, ident)] = ("matsym", text, None)
        for ident, text in CSYMBOL64_TOP.items():
            want[(top, ident)] = ("csymbol64", text, None)
        for ident, value in UNSOURCED_TOP.items():
            want[(top, ident)] = ("unsourced", None, value)
        for ident, text in SOCIAL.items():
            want[(top + "::Social", ident)] = ("csymbol64", text, None)
        for ident, text in TCP.items():
            want[(top + "::Tcp", ident)] = ("csymbol64", text, None)
        return want

    def test_hash_known_answers(self):
        # Values the game itself produced, so the implementations under test are the game's.
        g = self.gen
        for text, value in [("Echo_Arena", 0x331aa1b587e3a22f), ("test", 0x8b5df0c5a80654f7),
                            ("Hello_World", 0x2ac03f56a07afe92), ("main", 0x34db730a1c777fa9)]:
            self.assertEqual(g.cmatsym_hash(text), value, "CMatSym::Hash(%r)" % text)
        for text, value in [("serverdb", 0x25e886012ced8064), ("rwd_tint_0000", 0x74d228d09dc5dc86),
                            ("NEVRProtobufJSONMessageV1", 0xc6b3710cd9c4ef47), ("main", 0xc8c33e48377217be)]:
            self.assertEqual(g.csymbol64_hash(text), value, "CSymbol64(%r)" % text)

    def test_the_header_holds_exactly_the_constants_this_test_names(self):
        want = self.expected()
        self.assertEqual({ns for ns, _ in self.header}, EXPECTED_NAMESPACES)
        self.assertEqual(sorted(set(self.header) - set(want)), [], "constants with no name string here")
        self.assertEqual(sorted(set(want) - set(self.header)), [], "name strings with no constant in symbols.h")

    def test_every_constant_is_the_hash_of_its_name_in_its_namespace(self):
        g = self.gen
        wrong = []
        for key, (algorithm, text, pinned) in sorted(self.expected().items()):
            have = self.header.get(key)
            if algorithm == "matsym":
                value = g.cmatsym_hash(text)
            elif algorithm == "csymbol64":
                value = g.csymbol64_hash(text)
            else:
                value = pinned
            if have != value:
                wrong.append("%s::%s = %s, %s(%r) = 0x%x" % (key[0], key[1],
                             None if have is None else "0x%x" % have, algorithm, text, value))
        self.assertEqual(wrong, [], "constants that are not the hash of their name")

    def test_the_top_level_matsym_names_are_the_registry(self):
        self.assertEqual(len(REGISTRY), 184)
        self.assertEqual(len(set(REGISTRY)), 184)
        self.assertEqual(sorted(MATSYM_TOP.values()), sorted(REGISTRY))

    def test_no_two_constants_in_a_namespace_share_a_value(self):
        by_value = {}
        for (ns, ident), value in self.header.items():
            by_value.setdefault((ns, value), []).append(ident)
        shared = {k: v for k, v in by_value.items() if len(v) > 1}
        self.assertEqual(shared, {})


# The strings of rad-protocol/src/message_registry.rs (nevr-server-rs), sorted, one per message type.
REGISTRY = [
    'SBroadcasterConnectEvent',
    'SBroadcasterConnectFailedEvent',
    'SBroadcasterDisconnectEvent',
    'SBroadcasterEnableCompressionEvent_1',
    'SBroadcasterIntroduceAckEvent',
    'SBroadcasterIntroduceFailEvent',
    'SBroadcasterIntroduceFinEvent',
    'SBroadcasterIntroduceReqEvent',
    'SBroadcasterMailboxBatchEvent',
    'SBroadcasterMsgEncodingFailEvent',
    'SBroadcasterMsgEncodingSupportEvent_1',
    'SBroadcasterPingUpdate',
    'SBroadcasterReliableBatchEvent',
    'SBroadcasterRequestEncodingResetEvent',
    'SBroadcasterResetEncodingEvent',
    'SNSLobbyAcceptPlayersFailurev2',
    'SNSLobbyAcceptPlayersSuccessv2',
    'SNSLobbyAddEntrantAcceptedv2',
    'SNSLobbyAddEntrantRejectedv2',
    'SNSLobbyAddEntrantRequestv4',
    'SNSLobbyChatEntry',
    'SNSLobbyDirectoryJson',
    'SNSLobbyEntrantAccepted',
    'SNSLobbyEntrantAdded',
    'SNSLobbyEntrantDataChanged',
    'SNSLobbyEntrantRemoved',
    'SNSLobbyEntrantStatev2',
    'SNSLobbyEntrantTeamChanged',
    'SNSLobbyHostConnectFailed',
    'SNSLobbyHostConnected',
    'SNSLobbyHostDataChanged',
    'SNSLobbyHostDisconnected',
    'SNSLobbyHostState',
    'SNSLobbyJoinAcceptedv2',
    'SNSLobbyJoinComplete',
    'SNSLobbyJoinRejected',
    'SNSLobbyJoinRejectedInternal',
    'SNSLobbyJoinRequestv4',
    'SNSLobbyJoinSetup',
    'SNSLobbyLeft',
    'SNSLobbyLockEntrants',
    'SNSLobbyMatchmakerStatusReceived',
    'SNSLobbyOwnerDataChanged',
    'SNSLobbyOwnerState',
    'SNSLobbyPassOwnership',
    'SNSLobbyRegistrationFailure',
    'SNSLobbyRegistrationSuccess',
    'SNSLobbyRemovingEntrant',
    'SNSLobbySendClientLobbySettings',
    'SNSLobbySessionError',
    'SNSLobbySessionFailurev4',
    'SNSLobbySessionStarting',
    'SNSLobbySessionSuccessv5',
    'SNSLobbySetSpawnBotOnServer',
    'SNSLobbySettingsResponse',
    'SNSLobbySmiteEntrant',
    'SNSLobbyStartSessionv4',
    'SNSLobbyTerminateProcess',
    'SNSLobbyUnlockEntrants',
    'SNSLobbyUpdatePings',
    'SNSLobbyVoiceEntry',
    'SNSRefreshProfileForUser',
    'SNSRefreshProfileFromServer',
    'SR15ForcePushPlayer',
    'SR15NetAbilityActivate',
    'SR15NetAbilityCooldown',
    'SR15NetActivateExplosion',
    'SR15NetAutoTargetFireBullet',
    'SR15NetBalanceSettingsClientReady',
    'SR15NetBalanceSettingsMassUpdate',
    'SR15NetBalanceSettingsUpdate',
    'SR15NetBitFieldRequest',
    'SR15NetBitFieldResponse',
    'SR15NetBotActivateScenario',
    'SR15NetBotDisableScenario',
    'SR15NetBotRPC',
    'SR15NetBotSetGear',
    'SR15NetCancelGoal',
    'SR15NetCaptureVolumeReset',
    'SR15NetCaptureVolumeSpoofClaim',
    'SR15NetCaptureVolumeUpdate',
    'SR15NetClientPingResponseMsg',
    'SR15NetComponentEvent',
    'SR15NetCooldownActive',
    'SR15NetCooldownRefund',
    'SR15NetCooldownRefundRequest',
    'SR15NetCurrentLoadoutRequest',
    'SR15NetCurrentLoadoutResponse',
    'SR15NetDamageableBulletCollision',
    'SR15NetDamageableDamageDealt',
    'SR15NetDamageableDealDamage',
    'SR15NetDamageableHealthBroadcast',
    'SR15NetDamageableLocallyTrackedDamage',
    'SR15NetDamageablePlayerKilled',
    'SR15NetDamageablePlayerRespawn',
    'SR15NetDamageablePlayerRespawnComplete',
    'SR15NetDamageableShieldRepaired',
    'SR15NetExplosionNothingHit',
    'SR15NetExplosionPlayerDamage',
    'SR15NetFrisbeeServerGoaliePingWindowReliable',
    'SR15NetGameActivateHealBubbleMsg',
    'SR15NetGameCancelFireGunMsg',
    'SR15NetGameDepositMagazine',
    'SR15NetGameDestroyMagazine',
    'SR15NetGameFireGun',
    'SR15NetGameInputFireGunMsg',
    'SR15NetGameInputPullPinMsg',
    'SR15NetGameReplicateFireGunMsg',
    'SR15NetGameReplicatePullPinMsg',
    'SR15NetGameSetMagazineState',
    'SR15NetGameSpawnMagazine',
    'SR15NetGameSynchronizeCreateMagazine',
    'SR15NetGameplaySensorTriggered',
    'SR15NetGameplayTeleportPlayer',
    'SR15NetGameplayTeleportPlayerRequest',
    'SR15NetHealBubbleParamUpdate',
    'SR15NetIdsRequest',
    'SR15NetIdsResponse',
    'SR15NetItemEquippedNotification',
    'SR15NetLeaderboardMsg',
    'SR15NetLoadoutNumberNotification',
    'SR15NetNewUnlocks',
    'SR15NetPayloadPosStateUpdateReliable',
    'SR15NetPayloadRequestAutodriveChangeReliable',
    'SR15NetPayloadRequestSpoofedClaimChangeReliable',
    'SR15NetPayloadTrackpointReachedReliable',
    'SR15NetPayloadUpdateMiscStateReliable',
    'SR15NetPayloadUpdatedAutodriveReliable',
    'SR15NetPayloadUpdatedSpoofedClaimersReliable',
    'SR15NetPhysicsSimStateEvent',
    'SR15NetPlayEmote',
    'SR15NetPlayerEquipmentUpdate',
    'SR15NetPlayerPausedMsg',
    'SR15NetPlayerStateMsg',
    'SR15NetPooledActorCreate',
    'SR15NetPooledActorDestroy',
    'SR15NetPropertyModifiersAdd',
    'SR15NetReliablePostScoreUpdate',
    'SR15NetReliableStatUpdate',
    'SR15NetReliableTeamStatUpdate',
    'SR15NetReplicatedClientReady',
    'SR15NetReplicatedDeltaAck',
    'SR15NetReplicatedDeltaUnreliableVars',
    'SR15NetRequestUserApplyEntrant',
    'SR15NetSaveLoadoutFailure',
    'SR15NetSaveLoadoutPartial',
    'SR15NetSaveLoadoutRequest',
    'SR15NetSaveLoadoutSuccess',
    'SR15NetSensorPing',
    'SR15NetServerFrameMsg',
    'SR15NetServerInfo',
    'SR15NetSetBits',
    'SR15NetSetOwner',
    'SR15NetSocketLink',
    'SR15NetSocketUnlink',
    'SR15NetTetherActivate',
    'SR15NetTetherConnected',
    'SR15NetTetherConnectingUpdate',
    'SR15NetTetherConnectionBroken',
    'SR15NetTetherFoundTarget',
    'SR15NetTetherParamUpdate',
    'SR15NetTierRewardMsg',
    'SR15NetTopAwardsMsg',
    'SR15NetTouchOwnershipDeclined',
    'SR15NetTouchOwnershipGranted',
    'SR15NetTouchOwnershipTransferMailbox',
    'SR15NetTouchOwnershipTransferReliable',
    'SR15NetTouchStateEvent',
    'SR15NetUIDecSettingMsg',
    'SR15NetUIIncSettingMsg',
    'SR15NetUIReplJSONSettings',
    'SR15NetUIUserChangedSettings',
    'SR15NetUnreliableStatUpdate',
    'SR15NetUserAssignTeamRequest',
    'SR15NetUserAssignTeamResponse',
    'SR15NetUserPingEstMsg',
    'SR15NetUserVoipEvent',
    'SR15RemoveStun',
    'SR15RequestDebugInputPlayback',
    'SR15RequestDebugInputRecording',
    'SR15StartDebugInputPlayback',
    'SR15StartDebugInputRecording',
    'SR15StunMsg',
    'SR15StunPlayerMsg',
]


if __name__ == "__main__":
    unittest.main()
