# Signing in on Quest

The first time you start the game, and whenever your saved sign-in can no longer be used, the
headset asks you to sign in to EchoVRCE with a short code.

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
3. When the headset says "Signed in to EchoVRCE. Restart the game to finish.", close the game and
   start it again. It logs in with the saved sign-in from then on. If the screen does not change,
   wait about ten seconds after the web page confirms your sign-in (the headset checks every three
   seconds), then restart the game.

A code works for 5 minutes. If it runs out, the headset shows a new one in its place; use the
newest code. After six codes without a sign-in it stops and says "Sign-in timed out. Restart the
game to try again." (or "No sign-in code could be shown." if none of them could be displayed):
restart the game to get a new code.

## If the game closes before you finish

Start it again and enter the new code it shows. The old code no longer signs this headset in.
