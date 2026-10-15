# Dedicated server

`mecmp_server` is a small program that runs a MECMP session without the game: one shared room that
players join with the server's address, the same way they join a friend's session. It runs on Windows or
Linux (for example a VPS), needs no game files, and has nothing to do with the game's own online services:
it is the mod's own UDP protocol on its own port.

A player who hosts from the menu is still the easiest way for a group of friends. A server is for a
session that is always there.

## Run it

Windows: `mecmp_server.exe`. Linux: build it (below) and run `./mecmp_server`.

```
mecmp_server --name "My server" --port 47800 --max-players 16
```

| Option | Default | |
|---|---|---|
| `--port` | 47800 | UDP port. Open it in the firewall (and forward it on a home router). |
| `--name` | MECMP server | Shown in the players' menu (at most 23 bytes). |
| `--max-players` | 16 | The game shows at most 15 other players, so 16 is the useful maximum. |
| `--log` | mecmp_server.log | Log file (joins, leaves, names, chat). `--log ""` for none. |
| `--config` | mecmp_server.ini | Settings file, read first; options on the command line win. |
| `--bans` | bans.txt | Banned addresses (below). `--bans ""` for none. |
| `--mirror` | off | Test mode: each player also sees a copy of themselves 3 m away. |

`mecmp_server.ini` (optional, next to the program, or `--config <file>`):

```
name=My server
port=47800
max_players=16
log=mecmp_server.log
log_chat=1
motd=Welcome! Be nice.
max_per_ip=4
bans=bans.txt
```

`motd` is a chat line each player gets when they join. `max_per_ip` limits players from one IP address
(a household can share one; more than a few is usually someone filling the server).

## Discord

The server can post to a Discord channel when it goes online or offline and when players join and leave,
so your community sees who is on (and where to join):

1. In Discord: *Server Settings → Integrations → Webhooks → New Webhook*, pick the channel, *Copy Webhook URL*.
2. In `mecmp_server.ini`:

```
webhook_url=https://discord.com/api/webhooks/...
; optional (comments go on their own line):
; the name the posts appear under (default: the server's name)
webhook_name=Beat Link
; the address the "online" post tells players to join (default: the public address the server finds)
public_address=play.example.com:47800
; one message that always shows who is online and where to join, edited as players come and go
; (kept across restarts: its id is saved in webhook_status_id_file, next to the log)
webhook_live=1
webhook_status_id_file=webhook_status_id.txt
; posts (0 turns one off): players joining and leaving, server online / offline, finished dashes and
; time trials (with the time the player's game reports)
webhook_joins=1
webhook_status=0
webhook_results=1
```

The live message looks like this, and is updated within a few seconds of anyone joining or leaving:

> 🟢 **Beat Link** — 3/16 online: Meteor, Foie, Nollz
> Join: `play.example.com:47800` (MECMP menu → Join)

When the server stops cleanly it changes to "offline". A killed process or a power cut leaves the last message
as it was until the next start. Delete the message in Discord and the server posts a new one.

Keep the webhook URL private: anyone who has it can post to your channel. The server never sends it to
players or writes it to the log. Posts are gathered for a couple of seconds and sent together, at most about
28 a minute (Discord allows 30 per webhook); player names cannot ping anyone. On Linux the webhook needs libcurl
(`sudo apt install libcurl4t64` on Ubuntu 24.04, `libcurl4` on older systems); without it the server runs and logs that
the webhook is off.

When the server empties again, the join and leave messages posted since it was last empty are deleted, so the
channel shows the current session's players. The live status message and finished runs stay.

Console commands (type them in the server's window):

| Command | |
|---|---|
| `list` | players: id, name, address, time connected, packets |
| `kick <id or name>` | removes a player; their address is refused for 60 s. A name several players share is refused: use the id |
| `ban <id, name or IP>` | removes the player and refuses their IP address from now on (saved in bans.txt). A name several players share is refused: use the id |
| `unban <IP>` | lifts a ban |
| `bans` | lists banned addresses |
| `say <text>` | a chat message from the server |
| `status` | totals |
| `quit` | stops the server (Ctrl+C works too) |

## Join it

A few seconds after it starts, the server logs the addresses to join: on this PC, on the same
network, and from the internet (with a join code). On a home connection the internet address only
works once the UDP port is forwarded on the router to the PC the log names. On a VPS the public
address is the machine's own, and that is the only line.

In the MECMP menu, add the server's address under *Servers*: the list shows its name, how many
players are on it and the round trip, and has a *Join* button. Or type the address in the join field (`play.example.com` or
`203.0.113.7`, with `:port` when it is not 47800) and press *Join*. Or set `join=play.example.com:47800`
in `mecmp.ini`. The menu then shows *Server: <name>*.

Everyone on a server must run the same MECMP protocol version as the server (the server's first log
line says which). Packets from other versions are ignored.

## Test it alone

Run the server on your own PC on another port and join it with the mirror on:

```
mecmp_server --port 47801 --mirror
```

then join `127.0.0.1:47801` in game. Your own moves come back as a second player 3 m away, like the
menu's solo test, but through the server.

## A Linux VPS

Any small VPS works. A full server (16 players) sends about 1 MB/s (each player's ~150-byte update,
30 times a second, to 15 others); an emptier one much less. Pick a plan with enough monthly transfer
for how busy you expect it to be.

Build (needs `g++` and `cmake`):

```
git clone https://github.com/CodeNameMeteor/mecmp.git
cd mecmp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mecmp_server
```

On Linux only the server is built. Open the port, for example with ufw: `sudo ufw allow 47800/udp`.

Run it as a service (`/etc/systemd/system/mecmp.service`):

```
[Unit]
Description=MECMP server
After=network-online.target

[Service]
User=mecmp
WorkingDirectory=/opt/mecmp
ExecStart=/opt/mecmp/mecmp_server --config /opt/mecmp/mecmp_server.ini
Restart=always

[Install]
WantedBy=multi-user.target
```

```
sudo useradd --system --home /opt/mecmp mecmp
sudo mkdir -p /opt/mecmp && sudo cp build/mecmp_server /opt/mecmp/
sudo chown -R mecmp /opt/mecmp
sudo chmod 600 /opt/mecmp/mecmp_server.ini   # it holds the Discord webhook URL, if you set one
sudo systemctl enable --now mecmp
journalctl -u mecmp -f      # the log
```

A service has no console. Bans still work: edit `bans.txt` (one IP per line, `#` starts a comment)
and the server picks the change up within 5 s. For `list`, `kick` and `say`, run the server in a terminal
instead (for example inside `tmux`).

## Bans

`bans.txt` holds one IPv4 address per line; anything after the address is a comment (the `ban`
command writes the player's name and the date there). Banned players are dropped at once and told
"You are banned from this server." when they try again. Players who are refused because the server
is full, or their address has too many players, are told so too (at most once every 15 s).

A ban is by IP address, so it also hits others behind the same address, and a player whose address
changes can come back. There are no accounts to ban instead.

## What the server checks

- Packets must be MECMP packets of the server's protocol version and a known type, at most 1 KB.
- Joining takes two packets. A new address's HELLO is answered with a challenge (a cookie that proves the address);
  the player answers with it, and only then is it a player. Nothing else from a new address is heard.
- A player is an address. Their first packet ties their player id to that address: packets with
  another player's id are dropped, so nobody can move or speak as someone else. A restarted game
  (new id, same address) is accepted with its HELLO. A player who times out keeps its id reserved for
  a minute: another address cannot use that id meanwhile, and the player's own address can come back.
  After the minute the id is free.
- At most 80 packets a second per player (bursts up to 160); a client sends about 31.
- Only the server may say it hosts: the host, server and members-may-pull-in flags are cleared from
  forwarded HELLOs. Summons are forwarded as they are, and players do not trust a summon's "vouched"
  flag from a server, so there a hard summon only asks.
- A player silent for 10 s is dropped.
- At most `max_per_ip` players from one IP address; banned addresses get nothing but the notice.

Traffic is not encrypted and there are no accounts: names are whatever players type. Chat is written
to the server log (`log_chat=0` turns that off).
