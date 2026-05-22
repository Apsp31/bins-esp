# Changelog

## Unreleased

- Fix page 4 alert layout so it clearly shows which bin type to put out.
- Add automatic representative UPRN lookup from the St Albans NoticeBoard postcode search.
- Add captive portal setup with postcode, UPRN fallback, and idle status options.
- Fetch live St Albans refuse, recycling, food, and garden collection dates directly from the council Veolia endpoint.
- Cache collection dates locally and refresh on boot, on button press, and every six hours.
- Add four display layouts selectable by the left button.
- Add night-before/morning-of alert views for putting bins out.
- Add right-button long-press alert preview mode.
- Add forced setup mode by holding the left button during boot.
- Add PlatformIO build configuration for the ESP32 T-Display style board.
