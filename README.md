# Live Unreal Scanner v0.6

Server-side Metamod anti-cheat research build.

- Automatic analysis of all human players
- Bots ignored
- Private manual scan result to requesting admin
- Admin SteamIDs in `addons/liveac/liveac_admins.ini`
- Evidence log and JSONL panel feed
- Per-detector score caps and diversity gates to reduce false 100/100 scores
- No automatic ban or kick

Commands: `liveac_help`, `liveac_scan IDO 60`, `liveac_status IDO`, `liveac_watch IDO`, `liveac_top`.

Panel input: `addons/liveac/logs/panel_events.jsonl`.
