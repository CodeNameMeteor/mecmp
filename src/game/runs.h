#pragma once
// Finished runs: dashes and the game's other named challenges (deliveries, GridNodes, ...).
//
// How the game reports one (Ghidra: NamedChallenge_*): when a named challenge ends, the server's stats
// code (0x1439deb10) compares it with your record and sends PamSocialPlayNamedChallengeFinishedMessage
// (category 0x54776fc6, type 0xd71d027f) to the player: +0x48 ContentId, +0x50 improvement, +0x54 had a
// record, +0x58 the main stat (ms), +0x60 new record. The mod listens for that category with a listener
// object of its own, registered through the game's MessageManager_registerListener (no game code is
// changed), on both the client and the server message managers (whichever dispatches it; repeats are
// dropped). The challenge's name and kind come from PamServerNamedChallengeEntity objects (creator hook):
// +0x80 = PamNamedChallengeData {+0x10 Sid (a localisation id), +0x58 main stat code, +0x98 ContentId}.
// The name is localised with the game's own lookup (the UI's "localize").

namespace mecmp::game::runs {

// Creator hook: a PamServerNamedChallengeEntity was created.
void onNamedChallengeEntity(void* entity);
// Creator hook: a PamServerChallengeMarkerEntity (a player-made time trial) was created; names a finish.
void onChallengeMarker(void* entity);
// Each tick on the client / server game thread: registers the listener once that context exists.
void clientTick();
void serverTick();
void levelChanged(); // the run we were in (shown on the others' name tags) ends

} // namespace mecmp::game::runs
