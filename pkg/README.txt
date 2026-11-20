DCS INSTANT CAM v1.4
A handheld instant camera for DCS World, in VR and on a flat screen, for every aircraft.
=====================================================================

WHAT IT DOES
  Raise a camera in the cockpit, aim it with the mouse, zoom, and take a
  photo. The print slides out and develops in front of you, and a framed copy
  with a handwritten caption (aircraft, altitude, speed, heading, mission
  time, position) is saved to disk.

REQUIREMENTS
  - DCS World on DirectX 11 (current DCS). Vulkan DCS is not supported yet.
  - VR: native OpenXR (the default since DCS 2.9).
  - Flat screen: works in fullscreen and windowed mode. Not compatible with
    tools that replace DCS's screen output in unusual ways; ReShade is fine.
  - Works with Pimax, Quest (Link / Virtual Desktop), Varjo, WMR, etc.
  - Quad Views / foveated rendering is supported: photos use the sharp
    focus area when the camera points where you are looking.

HOW IT LOOKS
  VR: you hold the camera in front of you, with a live screen on the back.
  Flat screen: you hold the camera up in front of what you want to shoot -
  it follows the mouse, and its screen shows exactly what the photo will
  capture. (Set FollowAim=0 in InstantCam.ini to keep the camera in the
  lower-right corner with a viewfinder frame on the scene instead.)

CONTROLS (defaults)
  Mouse 4 (side/back button) .... raise / lower the camera
  Move mouse .................... aim the camera (VR) / move the frame (flat)
  Scroll wheel .................. zoom (24 - 200 mm)
  Left click .................... take the photo
  Middle click .................. recenter the camera to where you look
  Right click ................... lower the camera

  While the camera is raised, DCS does not receive the mouse, so you cannot
  click cockpit switches by accident. Everything goes back to normal when you
  lower it. The mouse is only captured while DCS is the active window.

  HOTAS: open Saved Games\InstantCam\InstantCam.ini and set the [Keys]
  section (e.g. F13-F24), then map those keys to HOTAS buttons in your
  joystick software (Joystick Gremlin, VKB, Thrustmaster TARGET, ...).

FILM PACKS
  The camera holds a 10-shot film pack, shown on the orange counter.
  When it blinks 0 the pack is empty: lower and raise the camera to load a
  new one. Set FilmPack=0 in InstantCam.ini for unlimited shots.

TRAY MENU
  DCS Instant Cam starts with Windows and sits in the tray (camera icon by
  the clock). Right-click it to:
  - open the photos folder or the caption editor
  - set your note for new photos (e.g. CAW UK)
  - switch follow-aim, film packs, the extra caption line and the sound
  - choose the photo resolution (1024 / 1536 / 2048)
  - turn "Start with Windows" on or off
  Changes apply in DCS within a couple of seconds, no restart needed.

EDITING CAPTIONS AFTER THE MISSION
  Tray menu > Edit photo captions (or double-click the tray icon).
  Pick a photo, change the handwritten lines, watch the preview, Save.
  Only the handwriting is rewritten; the picture is never changed.
  Photos taken before v1.4 have no stored caption - just type new lines.

WHERE THE PHOTOS GO
  Saved Games\InstantCam\Photos\IC_<date>_<time>.png   - every photo
  Saved Games\InstantCam\Latest.png                    - always the newest one

VIEWING PHOTOS IN THE COCKPIT (OpenKneeboard)
  In OpenKneeboard add a tab:
    - "Folder"     -> Saved Games\InstantCam\Photos   (the whole album)
    - or "File"    -> Saved Games\InstantCam\Latest.png (just the last shot)

PHOTOS AND CAPTIONS
  Photos get an instant-film look (soft corners, faded colours, pink
  highlights, grain) on a worn paper card, with handwritten notes:
    Near Banak - 31 Mar 2004, 07:47
    Pilot Gonzo | FA-18C hornet | your own note
  - The place is the nearest airfield on the map ("Near Incirlik", or
    "34 nm NE of Incirlik" when you're further away).
  - Date and time are the mission's, not your PC clock.
  - Add your own note with Caption= in InstantCam.ini.
  - CaptionDetails=1 adds a line with altitude, speed and heading.
  The installer adds one line to Saved Games\DCS\Scripts\Export.lua (a
  backup Export.lua.instantcam.bak is kept) and the file
  Scripts\Hooks\InstantCamHook.lua. If a multiplayer server disables player
  export, captions show only the date and time.

GOOD TO KNOW
  - VR: the camera only sees what DCS renders for your eyes, so it shows
    static if you aim it somewhere you are not looking.
  - Flat screen: photos are taken from the screen, so anything shown on it
    (labels, messages, kneeboard) can appear in the photo.
  - Zoom is digital: the camera can only use the detail DCS renders in
    your view, so at 150-200 mm photos get softer. 24-85 mm is sharpest.
  - The camera and the print never appear in your photos.
  - Client side only: nothing is needed on the server, and other pilots
    don't need it.

TROUBLESHOOTING
  Log file: Saved Games\InstantCam\InstantCam.log
  To switch the VR part off without uninstalling, set the environment variable
  DISABLE_XR_APILAYER_INSTANTCAM=1. To switch the flat-screen part off, delete
  Saved Games\DCS\Scripts\Hooks\InstantCamHook.lua.

CREDITS
  Handwriting font: Shadows Into Light by Kimberly Geswein,
  SIL Open Font License (see ShadowsIntoLight-OFL.txt).
