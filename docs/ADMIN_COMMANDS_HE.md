# פקודות אדמין – Live Unreal Scanner v0.5

## הרשאה מתוך המשחק

ערוך את הקובץ:

```text
cstrike/addons/liveac/liveac_admins.ini
```

הוסף SteamID מדויק, אחד בכל שורה:

```text
STEAM_0:1:12345678
STEAM_0:0:87654321
```

אחרי שינוי הקובץ, בקונסולת השרת/RCON:

```text
liveac_reload_admins
```

קונסולת השרת ו-RCON מורשים תמיד. שחקן שאינו ברשימה יקבל `Access denied`.

## פקודות

```text
liveac_help
liveac_status
liveac_status IDO
liveac_scan IDO
liveac_scan IDO 60
liveac_watch IDO
liveac_unwatch IDO
liveac_unwatch all
liveac_top
liveac_reset IDO
liveac_reset all
liveac_reload_admins
```

`liveac_scan IDO 60` מתחיל חלון בדיקה של 60 שניות. משך אפשרי: 15–300 שניות. בסיום יודפס דוח אוטומטי לקונסולה של האדמין שהפעיל את הבדיקה. הפקודה אינה נותנת Ban או Kick.

`liveac_watch IDO` שולח לקונסולת האדמין כל אירוע detector חדש של השחקן עד `liveac_unwatch`.
