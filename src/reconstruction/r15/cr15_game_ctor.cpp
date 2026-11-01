// CR15Game::CR15Game
// Quest libr15.so VA 0x12fce34 (RVA 0x12ece34); PC echovr.exe: no matching function located.
// ReVault binary 576, raw parent 100221436, reconstruction node 100600184; tier: reconstructed; grade: M, assessed by revault-codex.
// @0x12fce34 — NRadEngine::NRadGame::CR15Game::CR15Game
/* @confidence: M */
// This constructor also initializes the many unrelated fields shown in raw node
// 100221436; this reconstruction records the settings defaults relevant to T1/T1b.
void NRadEngine::NRadGame::CR15Game::CR15Game(
    CR15Game* self, CMainArgs const* args) {
    CRxGame::CRxGame(reinterpret_cast<CRxGame*>(self), args);

    // The Quest image loads the current and pending SR15GameSettings defaults
    // from adjacent 16-byte literals. Their second 32-bit word is flags.
    // T1/T1b change that word from 0x1003 to 0x3003, setting bit 13
    // (MatchTagDisplay) in both copies so applying pending settings on a fresh
    // install cannot clear the current default.
    //
    // Current settings literal at 0x2b9fe40 -> self + 0x8c04:
    //   { 0, 0x1003, 1, 0 }
    // Pending settings literal at 0x2b9fe60 -> self + 0x8ce4:
    //   { 0x1003, 1, 0, 0xa }
    //
    // These are image-data defaults loaded by the SIMD loads/stores in the
    // constructor (current at 0x12fd204..0x12fd21c; pending at 0x12fd27c..).
    // They are not unconditional runtime assignments: LoadSettings can later
    // replace the defaults from settings_mp_v2_quest.json.
}
