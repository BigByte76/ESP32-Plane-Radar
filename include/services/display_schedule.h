#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Scheduled display off/on (night mode), configured from the web portal.
 *
 * Local time comes from SNTP plus a POSIX TZ string. While the clock is not
 * yet synced, the display stays on. The off window may span midnight
 * (e.g. off 23:00, on 07:00). Equal off/on times disable the schedule.
 */
namespace services::display_schedule {

/** A named time zone for the portal dropdown. */
struct TimezoneOption {
  const char* label;
  const char* posix;
};

/** Common time zones, west to east. */
extern const TimezoneOption kTimezones[];
extern const size_t kTimezoneCount;

/** Load settings from NVS and start SNTP. Call once in setup(). */
void init();

/** Call every loop(): switches the backlight when the window starts or ends.
 *  Returns true while the display is off (skip redraws). */
bool update();

/** Thread-safe: true while the display is off (read by the ADS-B task). */
bool isOff();

/** Wake the display for config::kDisplayWakeMs (touch/BOOT tap while off). */
void wakeTemporarily();

/** Turn the backlight on and keep it on until reboot (e.g. during OTA). */
void forceOn();

bool enabled();
uint16_t offMinutes();
uint16_t onMinutes();
const char* timezone();

/** "HH:MM" for minutes after midnight. */
void formatHhMm(char* buf, size_t len, uint16_t minutes);
/** Current local time for the portal, e.g. "22:14 (UTC0)" or "not synced yet". */
void formatNow(char* buf, size_t len);

/** Save portal fields. Invalid times, or an empty TZ or one with characters
 *  outside letters, digits and <>+-,.:/, keep the previous value. */
void saveFromPortal(const char* enabled_checkbox, const char* off_hhmm,
                    const char* on_hhmm, const char* tz);

/** Erase the schedule (BOOT long-press wipe). */
void clear();

}  // namespace services::display_schedule
