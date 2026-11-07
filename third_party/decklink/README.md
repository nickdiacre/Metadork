# Blackmagic DeckLink SDK headers (16.0)

The `Mac/include`, `Win/include` and `Linux/include` folders of the Blackmagic DeckLink SDK 16.0, unmodified, so the
app builds without downloading the SDK. Copyright Blackmagic Design; each file carries its own licence notice, which
permits use, reproduction and distribution. Blackmagic's SDK EULA (clause 0.1) exempts exactly these three folders
from its restrictions on copying and distribution. The rest of the SDK (documentation, samples) is not included:
get it from https://www.blackmagicdesign.com/developer/ if you need it.

At runtime the app needs Blackmagic **Desktop Video** (the driver for DeckLink / UltraStudio devices) installed; the
SDK itself is not needed.

To build against a different SDK version, pass `-DDECKLINK_SDK_DIR=/path/to/SDK` (the folder containing Mac/, Win/
and Linux/).
