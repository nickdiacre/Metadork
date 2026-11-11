# Metadork icon

`make_icon.swift` draws the icon (Material-style film camera with two reels on dark grey) at 1024 x 1024. To regenerate
`metadork-1024.png`, `Metadork.icns` (macOS app bundle) and `metadork.ico` (Windows exe / window icon):

```bash
swift packaging/icon/make_icon.swift packaging/icon/metadork-1024.png
# then: iconutil for the .icns (16-512 @1x/@2x iconset) and a PNG-in-ICO for 16-256 px
```
