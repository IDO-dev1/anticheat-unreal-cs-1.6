# תיקון Build v0.2.2

השגיאה `invalid use of incomplete type const struct usercmd_s` נגרמה משום ש־`eiface.h` מכיל רק forward declaration.

הפתרון שנוסף ב־`src/plugin.cpp`:

```cpp
#include <extdll.h>
#include <usercmd.h>
```

`usercmd.h` נמצא תחת `hlsdk/common`, שכבר מוגדר ב־CMake וב־GitHub Actions.

חשוב להשאיר את כותרות C++ הסטנדרטיות לפני `extdll.h`, כדי למנוע התנגשויות של המאקרואים `min` ו־`max` של HLSDK.
