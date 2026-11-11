Metadork
===================

Reads camera metadata (lens focus / iris / zoom, timecode, record state, clip names, tilt / roll ...) from an SDI feed
on a Blackmagic DeckLink or UltraStudio device and sends it to Stage Precision, Unreal Engine and other tools over
FreeD, OSC and OpenTrackIO.

Manual: https://github.com/nickdiacre/sdi-metadata-viewer/blob/main/docs/MANUAL.md

Before you start
- Install Blackmagic Desktop Video (the DeckLink / UltraStudio driver): https://www.blackmagicdesign.com/support
- Only one application can use a DeckLink input at a time.

No camera? Drop one of the files in sample-captures/ onto the app window to replay a real recording.

macOS: the app is not notarised. The first time, right-click "Metadork.app" > Open > Open.
Windows: SmartScreen may warn about an unrecognised app: More info > Run anyway.

sdimeta_probe is a command-line diagnostic: it lists devices and prints which metadata packets reach the computer
(sdimeta_probe --list, sdimeta_probe 10 --watch).

Free and open source (MIT). If it's useful to you: https://buymeacoffee.com/nickdiacre
