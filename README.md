# Bins Display for TTGO/LilyGO T-Display

Self-contained ESP32 reminder display for St Albans bin collections.

## What it does

- Connects to Wi-Fi using a captive portal.
- Defaults to postcode `AL15SR` and UPRN `10001062494`.
- Looks up a representative UPRN automatically when the postcode is changed.
- Fetches live collection data from the St Albans public Veolia NoticeBoard endpoint.
- Caches the latest collection dates locally.
- Shows a large `PUT OUT TONIGHT` alert from 18:00 the evening before collection until 12:00 on collection day.
- Shows refuse, recycling, date/time, and Wi-Fi status when idle.
- Includes a versioned status page with Wi-Fi, IP, postcode, UPRN, last fetch, uptime, and health.
- Refreshes on boot and every 6 hours.

The data endpoint used is:

`https://gis.stalbans.gov.uk/NoticeBoard9/VeoliaProxy.NoticeBoard.asmx/GetServicesByUprnAndNoticeBoard`

## Hardware target

The default PlatformIO environment targets the common TTGO/LilyGO T-Display ESP32 1.14 inch ST7789 board:

- ESP32
- CH9102F USB serial
- 16MB flash
- ST7789 135 x 240 display

If your board turns out to be an ESP32-S3 T-Display, the display pin mapping and board target will need changing.

## First boot setup

1. Flash the firmware.
2. The board starts a Wi-Fi access point named `Bins Display Setup`.
3. Connect to it from a phone or laptop.
4. Open `http://192.168.4.1` if the setup page does not open automatically.
5. Enter Wi-Fi details.
6. Leave postcode as `AL15SR` or change it.
7. Leave UPRN as `10001062494`, clear it, or change the postcode to trigger an automatic lookup.

The live St Albans collection endpoint requires a UPRN. When the postcode changes, the firmware asks the St Albans NoticeBoard quick-search service for the first address in that postcode and stores its UPRN. This assumes properties in the same postcode share the same collection schedule.

If the access point does not appear, the ESP may already have saved Wi-Fi credentials from a previous sketch. Hold the left button while resetting or reconnecting USB. The board will clear saved setup and force the `Bins Display Setup` portal to stay open.

## Buttons

- Left button: cycle through the five display pages.
- Right button: refresh collection data now.
- Hold right button: toggle alert preview mode.

## Versioning

Each PlatformIO build increments the patch version in `VERSION` and regenerates `include/version.h`. Record the resulting version in `CHANGELOG.md` for the build you flash.

## Windows flashing

Install PlatformIO Core, then from this folder run:

```powershell
pio run -t upload
```

On this machine I installed PlatformIO with Python. If `pio` is not on PATH, use:

```powershell
& "$env:APPDATA\Python\Python313\Scripts\pio.exe" run -t upload
```

To watch serial logs:

```powershell
pio device monitor
```

If `pio device list` only shows Bluetooth COM ports, the board is not visible over USB yet. Try another USB cable or press the board reset/boot button while connecting.

If upload fails, hold the left button while plugging in USB, release it after upload starts, and try again.
