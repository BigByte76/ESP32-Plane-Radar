#!/usr/bin/env python3
"""Rebuild the web portal's pages as static HTML for screenshots.

WiFiManager assembles each page at runtime from the string templates in its
wm_strings_en.h. This script parses those templates from the installed library
and assembles the same pages the device serves on the LAN (menu, Wi-Fi +
settings form, firmware update), filling in this firmware's custom fields the
way wifi_setup.cpp does. Sample values (SSID, IP, location) are made up.

Usage: portal_pages.py OUT_DIR   (writes OUT_DIR/*.html and OUT_DIR/pages.txt)
"""
import html
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WM = ROOT / ".pio/libdeps/esp32-8048s050/WiFiManager"

# ---- Sample device state ----
HOSTNAME = "plane-radar"
IP = "192.168.1.50"
SSID = "HomeNetwork"
LAT, LON = "51.470000", "-0.454300"
USE_MILES = False
SHOW_RUNWAYS = True
SCHED_ON = True
DISP_OFF, DISP_ON = "23:00", "07:00"
TZ = "GMT0BST,M3.5.0/1,M10.5.0"
NOW = "Sat 14:05 GMT"
SCAN = [("HomeNetwork", -48, True), ("HomeNetwork-5G", -55, True),
        ("Neighbour", -71, True), ("CoffeeShop Guest", -83, False)]


def c_string_constants(text):
    """name -> value for `const char NAME[] PROGMEM = "..." "...";`."""
    # Whole-line // comments sit between the string pieces of some templates.
    text = re.sub(r"(?m)^\s*//[^\n]*$", "", text)
    out = {}
    for m in re.finditer(r'const char\s+(\w+)\[\]\s+PROGMEM\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);',
                         text):
        out[m.group(1)] = "".join(unescape(s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"',
                                                                  m.group(2)))
    return out


def string_array(text, name):
    m = re.search(name + r'\[\]\s*PROGMEM\s*=\s*\{(.*?)\};', text, re.S)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [unescape(s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', body)]


def unescape(s):
    return bytes(s, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


strings_src = (WM / "wm_strings_en.h").read_text()
S = c_string_constants(strings_src)
S.update(c_string_constants((WM / "wm_consts_en.h").read_text()))
MENU = string_array(strings_src, "HTTP_PORTAL_MENU")
TOKENS = ["wifi", "wifinoscan", "info", "param", "close", "restart", "exit", "erase",
          "update", "sep", "custom"]
DEFAULT_MENU = ["wifi", "info", "exit", "sep", "update"]  # WiFiManager.h _menuIdsDefault


def head(title):
    return (S["HTTP_HEAD_START"].replace("{v}", title) + S["HTTP_SCRIPT"] + S["HTTP_STYLE"]
            + S["HTTP_HEAD_END"].replace("{c}", ""))


def root_main(title):
    return S["HTTP_ROOT_MAIN"].replace("{t}", title).replace("{v}", f"{HOSTNAME} - {IP}")


def status():
    return S["HTTP_STATUS_ON"].replace("{i}", IP).replace("{v}", html.escape(SSID))


# ---- Custom fields, as wifi_setup.cpp registers them ----
LABEL_NONE, LABEL_BEFORE, LABEL_AFTER = 0, 1, 2


def timezones():
    src = (ROOT / "src/services/display_schedule.cpp").read_text()
    block = src[src.index("kTimezones[] = {"):src.index("};", src.index("kTimezones[] = {"))]
    return re.findall(r'\{"([^"]*)",\s*"([^"]*)"\}', block)


def tz_select():
    opts, matched = [], False
    for label, posix in timezones():
        sel = posix == TZ
        matched = matched or sel
        opts.append(f"<option value='{posix}'{' selected' if sel else ''}>{label}</option>")
    return ("<br/><label for='tzsel'>Time zone</label>"
            "<select id='tzsel' onchange=\"var t=document.getElementById('tz');"
            "t.type=this.value?'hidden':'text';"
            "if(this.value){t.value=this.value}else{t.focus()}\">"
            + "".join(opts)
            + f"<option value=''{'' if matched else ' selected'}>Other (enter POSIX TZ)</option></select>"
            ), matched


def checkbox(checked):
    return 'type="checkbox"' + (" checked" if checked else "")


def params():
    tz_html, tz_matched = tz_select()
    tz_attrs = "type='hidden'" if tz_matched else \
        "type='text' placeholder='e.g. EST5EDT,M3.2.0,M11.1.0'"
    coord = ' type="number" step="0.000001"'
    return [
        ("radar_lat", "Latitude (deg)", LAT, 20, coord, LABEL_BEFORE),
        ("radar_lon", "Longitude (deg)", LON, 20, coord, LABEL_BEFORE),
        ("use_miles", "Display distances in miles", "T", 2, checkbox(USE_MILES), LABEL_AFTER),
        ("show_runways", "Show airport runways", "T", 2, checkbox(SHOW_RUNWAYS), LABEL_AFTER),
        (None, None, None, 0,
         f"<hr><h3>Display schedule</h3><small>Device time now: {html.escape(NOW)}</small><br/>",
         LABEL_BEFORE),
        ("sched_on", "Turn display off on a schedule", "T", 2, checkbox(SCHED_ON), LABEL_AFTER),
        ("disp_off", "Display off at", DISP_OFF, 8, ' type="time"', LABEL_BEFORE),
        ("disp_on", "Display on at", DISP_ON, 8, ' type="time"', LABEL_BEFORE),
        (None, None, None, 0, tz_html, LABEL_BEFORE),
        ("tz", "", TZ, 63, tz_attrs, LABEL_NONE),
        (None, None, None, 0,
         "<small>While the display is off, tap the screen to wake it for a minute.</small>",
         LABEL_BEFORE),
    ]


def param_out():
    """WiFiManager::getParamOut()."""
    page = ""
    for i, (pid, label, value, length, custom, place) in enumerate(params()):
        if pid is None:
            page += custom
            continue
        if place == LABEL_BEFORE:
            item = S["HTTP_FORM_LABEL"] + S["HTTP_FORM_PARAM"]
        elif place == LABEL_AFTER:
            item = S["HTTP_FORM_PARAM"] + S["HTTP_FORM_LABEL"]
        else:
            item = S["HTTP_FORM_PARAM"]
        for tok, val in (("{I}", f"{S.get('S_parampre', 'param_')}{i}"), ("{i}", pid),
                         ("{n}", pid), ("{p}", "{t}"), ("{t}", label), ("{l}", str(length)),
                         ("{v}", value), ("{c}", custom)):
            item = item.replace(tok, val)
        page += item
    return page


def scan_items():
    """WiFiManager::getScanItemOut() with default display options."""
    tpl = S["HTTP_ITEM"].replace("{qp}", S["HTTP_ITEM_QP"]).replace("{h}", "h", 1)
    tpl = tpl.replace("{qi}", S["HTTP_ITEM_QI"]).replace("{h}", "")
    out = ""
    for ssid, rssi, locked in SCAN:
        perc = 0 if rssi <= -100 else 100 if rssi >= -50 else 2 * (rssi + 100)
        q = round(1 + (perc - 0) * (4 - 1) / 100)
        out += (tpl.replace("{V}", html.escape(ssid)).replace("{v}", html.escape(ssid))
                .replace("{r}", str(perc)).replace("{q}", str(int(q)))
                .replace("{i}", "l" if locked else ""))
    return out


def page_root():
    menu = "".join(MENU[TOKENS.index(t)] for t in DEFAULT_MENU)
    return (head("WiFiManager") + root_main("WiFiManager") + S["HTTP_PORTAL_OPTIONS"] + menu
            + status() + S["HTTP_END"])


def page_wifi():
    form_wifi = S["HTTP_FORM_WIFI"].replace("{v}", SSID).replace("{p}", S["S_passph"])
    return (head(S["S_titlewifi"]) + scan_items()
            + S["HTTP_FORM_START"].replace("{v}", "wifisave") + form_wifi
            + S["HTTP_FORM_WIFI_END"] + S["HTTP_FORM_PARAM_HEAD"] + param_out()
            + S["HTTP_FORM_END"] + S["HTTP_SCAN_LINK"] + status() + S["HTTP_END"])


def page_update():
    return head("WiFiManager") + root_main("WiFiManager") + S["HTTP_UPDATE"] + S["HTTP_END"]


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "portal")
    out.mkdir(parents=True, exist_ok=True)
    # (name, html, viewport height): heights fit each page at the 560 px width.
    pages = [("menu", page_root(), 540), ("settings", page_wifi(), 1100),
             ("update", page_update(), 270)]
    for name, body, _ in pages:
        (out / f"{name}.html").write_text(body, encoding="utf-8")
    (out / "pages.txt").write_text("".join(f"{n} {h}\n" for n, _, h in pages))
    print(f"wrote {len(pages)} pages to {out}")


if __name__ == "__main__":
    main()
