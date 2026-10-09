# Signing in on Quest

The first time you start the game, and whenever your saved sign-in can no longer be used, the
headset asks you to sign in to EchoVRCE with a short code.

This applies to Quest builds whose sentinel starts NEVR token auth (`QuestTokenAuth`); the sentinel
built from `src/quest/sentinel/` on its own does not, and shows the game's own login error instead.
That the game's login error screen displays the sign-in text has not been confirmed on a headset;
in those builds `device_login.txt` (below) holds the same page and code whether or not the screen
shows them.

## What you see

When the game cannot log in yet, its login error screen shows:

```
Sign in to play: on a phone or computer, open
echovrce.com/login/device
and enter the code XXXX-XXXX
Each code lasts 5 minutes, then a new one is issued.
```

The same page and code are in `device_login.txt` in the game's files folder on the headset,
`/sdcard/Android/data/com.readyatdawn.r15/files/device_login.txt`, with a link that has the code
filled in and the time the code expires (UTC). You can read it with a file browser on the headset
or with `adb pull`.

## What to do

1. On a phone or computer, open `https://echovrce.com/login/device` and sign in with Discord.
2. Enter the code shown in the headset.
3. When the headset says "Signed in to EchoVRCE. Select RETRY to finish.", select RETRY. The game
   logs in with the saved sign-in, now and every time it starts. If the screen does not change,
   wait about ten seconds after the web page confirms your sign-in (the headset checks every three
   seconds); if it still says the code, restart the game.

A code works for 5 minutes. If it runs out, the headset shows a new one in its place; use the
newest code. After six codes without a sign-in it stops and says "Sign-in timed out. Restart the
game to try again." (or "No sign-in code could be shown." if none of them could be displayed):
restart the game to get a new code.

## If the game closes before you finish

Start it again and enter the new code it shows. The old code no longer signs this headset in.
