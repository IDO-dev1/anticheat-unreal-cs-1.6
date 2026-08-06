# התקנה — Live Unreal Scanner v0.1

1. העלה את תיקיית הפרויקט ל־GitHub.
2. פתח Actions והרץ `Build Linux Metamod SO`.
3. הורד את ה־artifact וחלץ לשרת.
4. הוסף לקובץ `cstrike/addons/metamod/plugins.ini`:

```text
linux addons/liveac/dlls/live_unreal_scanner_mm_i386.so
```

5. הפעל מחדש ובדוק בקונסול:

```text
meta list
```

הראיות נכתבות ל־`liveac_evidence.log` מתוך תיקיית העבודה של HLDS.

## חשוב

- מיועד ל־HLDS/ReHLDS ‏32-bit ול־Metamod/Metamod-r.
- אין HTTP, אין Python ואין thread שממתין לרשת.
- v0.1 מתריע בלבד ולא מבצע kick/ban.
- זהו אב־טיפוס למדידה וכיול, לא הוכחה מוחלטת לצ'יטים.
