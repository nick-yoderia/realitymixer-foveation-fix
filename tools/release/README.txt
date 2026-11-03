Reality Mixer foveation fix

Fixes Reality Mixer boxes fading out outside the sharp area of SteamVR 2.19's
foveated stream (Steam Link / Steam Frame).

  Install.cmd    finds Reality Mixer, closes it, installs the fix, restarts it
  Uninstall.cmd  puts Reality Mixer's original openvr_api.dll back

A Reality Mixer update or Steam's "Verify integrity of game files" removes the
fix; run Install.cmd again. Log: %TEMP%\rmfix.log
