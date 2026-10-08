# Roadmap

## Done
- Host / join with a join code; UPnP port mapping; host forwards everyone to everyone (0.4.0).

## Next
- Optional relay/rendezvous fallback for hosts behind carrier-grade NAT.

## In-game menu (ImGui)
- Done (0.5.0): host / join by code, players list, name, model for other players.
- Next: names above each player's head (needs the game camera's view-projection matrix).
- Your own look, sent to the others.

## Looks
- Send each player's chosen model to the others (VisualUnlockAsset Identifier as the wire id).
- All ghost colours and patterns (the Projection/* customizations).

## Later / maybe
- Collision between players.
- Smoothing over bad connections (extrapolation, jitter buffer tuning).
