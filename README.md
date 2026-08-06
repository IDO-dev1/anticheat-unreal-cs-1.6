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
