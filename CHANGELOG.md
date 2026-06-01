# Changelog

## [0.1.12] - 2026-06-01

- Bake in the CYD colour calibration values selected on-device: red `07FF`, green `E0FF`, blue `FFE0`, yellow `001F`.
- Route semantic UI colours through the calibrated palette across alerts, status, clock, setup, and collection pages.
- Remove the temporary serial colour reporter used to read back calibration values.

## [0.1.10] - 2026-06-01

- Add a CYD-only colour swatch calibration page using touch selection for red, green, blue, and yellow.
- Persist selected RGB565 colour values and show them on the calibration page for read-back.
- Use the calibrated red value for the analog alert warning background.

## [0.1.9] - 2026-06-01

- Replace the CYD analog alert colour-name workaround with an explicit byte-swapped red background value.

## [0.1.8] - 2026-06-01

- Make the CYD analog clock hands thicker for better readability.
- Fix the CYD analog alert page so it shows the bin type using a text-capable font.
- Correct the CYD analog alert background colour so the warning renders red on the panel.

## [0.1.7] - 2026-06-01

- Replace the seventh CYD page with a large analog clock, day, date, and next collection summary.
- Add a dedicated red bins warning alert for the CYD analog clock page.

## [0.1.6] - 2026-06-01

- Add two CYD-only large-screen display pages: a next-collection dashboard and a two-card refuse/recycling view.
- Expand the CYD page cycle to seven pages while keeping smaller displays on the original five pages.
- Add a wide CYD alert layout for the large-screen-only pages.

## [0.1.5] - 2026-06-01

- Remove conflicting CYD TFT width and height overrides so the ILI9341 driver can use its native geometry.
- Switch CYD touch input to the dedicated XPT2046 controller on the ESP32-2432S028 touch SPI pins.
- Use the CYD-compatible ILI9341 variant and HSPI TFT bus setup.

## [0.1.4] - 2026-06-01

- Configure the CYD target as native 320x240 landscape instead of rotating a portrait coordinate system.
- Further reduce CYD TFT SPI speed and force unused shared SPI chip-select pins inactive to reduce white-dot artefacts.
- Read CYD touch using raw XPT2046 pressure and coordinates instead of relying on calibrated touch conversion.

## [0.1.3] - 2026-06-01

- Rotate the Cheap Yellow Display target to landscape orientation.
- Lower CYD display SPI speed to reduce visible tearing.
- Fix the TFT_eSPI touch frequency build flag and apply CYD touch calibration data.
- Add CYD single-button long press refresh while keeping short press as page cycling.

## [0.1.2] - 2026-06-01

- Add a Cheap Yellow Display PlatformIO environment for the ESP32 2.8 inch 320x240 ILI9341 touchscreen board.
- Add CYD touch controls: left-half tap cycles pages, right-half tap refreshes, and right-half long press toggles alert preview.
- Scale display layouts for 320x240 screens while preserving the original T-Display layout.

## [0.1.1] - 2026-05-23

- Add automatic firmware patch version bumping during PlatformIO builds.
- Show the firmware version, Wi-Fi, IP, postcode, UPRN, last fetch, uptime, and health on a fifth status page.
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
