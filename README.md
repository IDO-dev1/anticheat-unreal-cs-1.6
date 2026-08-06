# Live Unreal Scanner v0.8 — Verifiable Scan Reports

מודול Metamod ל־CS 1.6/ReHLDS שמנתח שחקנים אנושיים בלבד ומספק לאדמין דוח סריקה מפורט.

## השינוי המרכזי

`liveac_scan` כבר לא מחזיר `CLEAN` רק מפני שלא נוצר אירוע. לכל קבוצת detector מוצגים מוני דגימות ומצב:

- `PASS` — נאספו מספיק נתונים ולא נמצאה חריגה.
- `WARNING` — נאספו מספיק נתונים ונמצאה ראיה.
- `INSUFFICIENT` — לא היו מספיק מצבי משחק כדי לבדוק.

הדוח כולל Aim, Fire, Movement ו־Speed וכן נכתב ל־`addons/liveac/logs/panel_events.jsonl`.

## פקודות

```text
liveac_help
liveac_status [slot|name]
liveac_scan <slot|name> [15-300]
liveac_watch <slot|name>
liveac_unwatch <slot|name|all>
liveac_top
liveac_reset <slot|name|all>
liveac_reload_admins
```

## אדמין

הוסף SteamID, שורה אחת ללא מרכאות, אל:

```text
cstrike/addons/liveac/liveac_admins.ini
```

לאחר מכן הרץ `liveac_reload_admins` או הפעל מחדש את השרת.

## בנייה

העלה את כל הפרויקט ל־GitHub והפעל את workflow בשם `Build Linux i386`. הפלט הוא artifact בשם `live-unreal-scanner-v0.8.0-linux-i386`.

## בטיחות

המערכת מתריעה ושומרת ראיות בלבד. אין Ban/Kick אוטומטי. Speed כרגע הוא מדד דיווח שמרני (`>520` units/s במספר דגימות), ולא ראיה עצמאית לבאן.

## v1.0 hardening changes

Developed by **IDO** and released under the **MIT License**.

- `liveac.cfg` is now parsed at runtime (`key=value`).
- `liveac_reload_config` reloads tuning without restarting the server and resets detector state.
- JSONL fields are escaped, including player names and evidence details.
- Text logs remove control characters to prevent forged log lines.
- Log directories are created once during plugin initialization; no shell or `system()` call is used on the game thread.
- Evidence, panel, and admin-audit logs rotate to `.1` at 25 MB.
- Ambiguous partial player names are rejected and numeric slots are displayed.
- SteamID values are cached per report/event.
- `liveac_menu` displays the in-game administration command menu.
- ReChecker is not duplicated. Use ReChecker separately for configured client resource/hash checks; LiveAC focuses on behavioral detection.
- No automatic kick or ban.

## Bot target testing

`liveac.cfg` supports two independent bot options:

```ini
liveac_allow_bot_targets=1
liveac_allow_bot_scan=0
```

With these recommended values, bots are not analyzed as suspects, but they are valid enemy targets for Aim and Fire measurements of human players. This allows private testing against YaPB without deterministic bot aim producing false-positive suspect records. Set `liveac_allow_bot_scan=1` only in a controlled development environment. Reload changes with `liveac_reload_config`.
