# Roadmap

## Next: joining each other, more players
- Sessions: one player hosts, others join with a short code; the host tells everyone about
  everyone (full mesh), so 3+ players see each other.
- NAT traversal (UDP hole punching through a tiny rendezvous server) so players in different
  houses can join without Tailscale/ZeroTier or port forwarding.
- Version handshake with a clear message on mismatch (protocol field already in every packet).

## In-game menu (ImGui)
- Host / join by code, list of connected players.
- Pick your name and your own look; your choice is sent to the others.
- Names shown above each player's head.

## Looks
- Send each player's chosen model to the others (VisualUnlockAsset Identifier as the wire id).
- All ghost colours and patterns (the Projection/* customizations).

## Later / maybe
- Collision between players.
- Smoothing over bad connections (extrapolation, jitter buffer tuning).
