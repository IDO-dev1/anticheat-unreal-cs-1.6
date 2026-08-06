# Live Unreal Scanner v0.7 — Context Core

מודול Metamod ל־CS 1.6/ReHLDS שמבצע ניתוח server-side בלבד. אין Ban או Kick אוטומטי.

## מה חדש ב־v0.7

- סריקה אוטומטית של כל שחקן אנושי; בוטים מדולגים.
- בחירת האויב הנראה הקרוב ביותר לכוונת באמצעות TraceLine.
- `TARGET_AIM_SNAP` דורש שיפור משמעותי בכיוון וסיום קרוב למטרה נראית.
- Aim Type 5 מותאם מתקבל רק כאשר הכוונת נעולה על אויב נראה.
- AutoAttack מתקבל רק כאשר אויב נמצא בקונוס הכוונת.
- מודל reaction-time שמחייב לפחות 8 דגימות מהירות עם שונות נמוכה.
- detector יחיד מוגבל בניקוד; ציון גבוה דורש ראיות מסוגים שונים.
- `liveac_scan <player> <seconds>` מחזיר דוח רק לאדמין שהפעיל אותו.
- אירועים ודוחות סריקה נכתבים ל־`addons/liveac/logs/panel_events.jsonl` עבור הפאנל.

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

## אדמין במשחק

הוסף SteamID, ללא מרכאות או flags, אל:

```text
cstrike/addons/liveac/liveac_admins.ini
```

לדוגמה:

```text
STEAM_0:0:780558973
```

ואז הרץ `liveac_reload_admins` בקונסולת השרת או בצע restart.

## בנייה

העלה את כל תוכן הפרויקט ל־GitHub והפעל את workflow בשם `Build Linux i386`.
ה־artifact מכיל `live_unreal_scanner_mm_i386.so` וקובצי ההגדרות.

## מגבלות

זו מערכת התראה ובדיקת אדמין, לא הוכחה מוחלטת לרמאות. אין אפשרות להבטיח אחוז דיוק לפני כיול על מאגר גדול של שחקנים נקיים וצ'יטים. v0.7 מוסיפה הקשר מטרה ונראות כדי להפחית false positives, אך עדיין דורשת בדיקות שרת אמיתיות.
