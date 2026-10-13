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
| `--mirror` | off | Test mode: each player also sees a copy of themselves 3 m away. |

`mecmp_server.ini` (optional, next to the program, or `--config <file>`):

```
name=My server
port=47800
max_players=16
log=mecmp_server.log
log_chat=1
```

Console commands (type them in the server's window):

| Command | |
|---|---|
| `list` | players: id, name, address, time connected, packets |
| `kick <id or name>` | removes a player; their address is refused for 60 s |
| `say <text>` | a chat message from the server |
| `status` | totals |
| `quit` | stops the server (Ctrl+C works too) |

## Join it

In the MECMP menu, type the server's address in the join field (`play.example.com` or
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
sudo systemctl enable --now mecmp
journalctl -u mecmp -f      # the log
```

A service has no console, so `list`, `kick` and `say` need the server running in a terminal (for
example inside `tmux`) instead.

## What the server checks

- Packets must be MECMP packets of the server's protocol version and a known type, at most 1 KB.
- A player is an address. Their first packet ties their player id to that address: packets with
  another player's id are dropped, so nobody can move or speak as someone else. A restarted game
  (new id, same address) is accepted with its HELLO.
- At most 80 packets a second per player (bursts up to 160); a client sends about 31.
- Only the server may say it hosts: the host and server flags are cleared from forwarded HELLOs.
- A player silent for 10 s is dropped.

Traffic is not encrypted and there are no accounts: names are whatever players type. Chat is written
to the server log (`log_chat=0` turns that off).
