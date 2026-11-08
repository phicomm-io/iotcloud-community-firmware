# G3 screen layouts and appearance

For BRY88BBA15G RC6 ESP32 firmware and its companion screen MCU. Get the [latest compatible versions]({{SITE_URL}}en/#releases) using the [installation guide](daikin-g3-guide), then open Settings → Screen. The [interactive demo]({{SITE_URL}}demos/daikin-g3/#settings/screen) uses simulated data; the [screen design preview]({{SITE_URL}}dakin_lvgl/) preserves the display proportions.

## Compose pages

The 480 × 854 portrait display uses a 4-column × 6-row grid, 16 px outer margins and 8 px gaps. Limits are eight pages, 32 components in total and eight per page. Choose Add composed page or Edit layout to drag and resize. Keyboard movement, 20 undo steps and browser-local drafts are supported. A draft becomes the device layout only after submission and device confirmation.

| Component | Typical use |
| --- | --- |
| Clock | Time and date, following synchronized time and screen language |
| Value | Temperature, humidity, CO₂, PM2.5, TVOC, HCHO or an external value; TVOC supports ppb or grade |
| Text | Custom body, title or external text |
| Chart | Line, area, bar, stacked area, stacked bar and pie |
| Weather | Current conditions, icons, location and forecast fetched by ESP32 |
| Reminder | Unread notices from schedules or API pushes |

Sizes use rows × columns. Weather supports 1×4 and 2×4, clocks 1×4 and readings 1×2. Square readings place the title beside a smaller unit and scale the reading with the component. A full-row 1×4 reading places its title and unit on the left and the reading on the right. A 2×4 chart reserves the left column for title/value and the right three columns for plotting. Hide a fullscreen component's title to keep the page title and gain content space.

## History and external data

Local history samples every minute, with 1-, 6- or 24-hour ranges and 32, 64, 128 or 256 points. Short histories stretch across the plot; missing readings leave gaps. History lives in screen MCU RAM and starts accumulating again after a restart.

External data supports three series of 32 points, each with a TTL. `null` means missing data; stacked plots require matching units and nonnegative values. See [weather, reminders and external data](daikin-g3-services) for configuration and push protocols.

## Themes and wallpapers

Choose one of six complete themes or customize its colors. Background, title, value, unit and body colors can independently override the theme; reset restores inheritance. Wallpapers support presets and uploads. Cards overlay the image with adjustable background and edge opacity while text remains opaque.

Use the [browser wallpaper converter]({{SITE_URL}}tools/screen-wallpaper/) to crop a local image to display dimensions and generate the required RGB565 / BWP resource, then upload it under Resources and updates. Conversion runs in the browser without submitting the original to an image service. Unavailable resources use a solid background.

## Brightness, sleep and language

Display settings adjust live brightness and its limit, up to 100%. Configure separate external-power and battery idle timeouts; zero disables idle sleep. Choose idle brightness and a composed idle page.

Up to eight weekly plans support overnight periods, using the start day's weekday. The first matching plan wins. Plans and reminders use a saved fixed timezone without automatic DST. The first short touch while asleep only wakes the screen; subsequent touches change pages.

Chinese/English settings translate built-in labels and weather descriptions. Device names, city labels and custom body text retain their entered content. Custom Chinese characters require the full font and can briefly show placeholders during background loading; see [startup and resource diagnostics](daikin-g3-diagnostics). The page strip offers 6, 12 and 20 px widths, defaulting to 12 px.

## Change pages through ESP32

Display → Remote paging provides previous, next and go-to controls. Disabled pages are skipped; paging wakes the screen without rewriting the layout or sleep policy. The current page appears after success. Stock MCUs do not support remote paging, so their controls are disabled.
