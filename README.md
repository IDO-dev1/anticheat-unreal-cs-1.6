# Live Unreal Scanner v0.4.0

בסיס נקי למודול Metamod Linux i386 עם מנוע זיהוי לייב מותאם מרעיונות UnrealDemoScanner.

## בנייה

1. החלף את כל קבצי הפרויקט בגרסה זו.
2. אל תעלה את `build/` או `deps/`.
3. בצע commit ו-push.
4. פתח GitHub Actions והפעל **Build Linux i386**.
5. הורד את artifact בשם `live-unreal-scanner-v0.4.0-linux-i386`.

ה-workflow מוודא שהקובץ הסופי הוא ELF 32-bit לפני האריזה.

## התיקונים המרכזיים

- `usercmd_s` מקבל הגדרה מלאה דרך `common/usercmd.h`.
- כל כותרות C++ נטענות לפני `extdll.h`, ולאחר HLSDK מבוטלים המאקרואים `min/max`.
- חתימות `CmdStart` ו-`Meta_Query` תואמות ל-HLSDK/Metamod.
- גם `liveac_core` וגם המודול נבנים עם `-m32`, למניעת ערבוב 64/32-bit בזמן הקישור.
- אין Python, HTTP או המתנה לרשת בתוך HLDS.

## התקנה

הוסף ל-`cstrike/addons/metamod/plugins.ini`:

```text
linux addons/liveac/dlls/live_unreal_scanner_mm_i386.so
```

לאחר הפעלה מחדש הרץ `meta list`.

> גרסה זו מיועדת להתראות ואיסוף ראיות בלבד, ללא Ban/Kick אוטומטי.

## Admin commands (server console / RCON)

```text
liveac_help
liveac_status
liveac_status <slot|partial-name>
liveac_reset <slot|partial-name|all>
```

Bots (`auth=BOT`) are ignored. A raw angle snap no longer adds score by itself; it must be followed repeatedly by an attack within the configured timing window.
