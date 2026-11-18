# ws-pull

Copies the weather station's readings from the display to a Linux computer on the same network, every hour, into one CSV file per month. The display keeps about 23 days, so the computer can be off for up to about three weeks without losing anything. Meant for a desktop that's on a few hours a day.

Python 3.10 or newer (standard library only) and systemd. Written for Linux Mint 22 (Ubuntu 24.04 base) and newer.

## Install

From a copy of this folder:

```sh
sudo sh install.sh http://192.168.1.50      # the display's address
```

If the display's web panel has a password, put it in `/etc/ws-pull/password` (one line, nothing else):

```sh
sudo nano /etc/ws-pull/password
```

Then a first run, and its log:

```sh
sudo systemctl start ws-pull
journalctl -u ws-pull -n 20
```

The timer runs it 5 minutes after every boot and at every full hour (`systemctl list-timers ws-pull.timer`). Running `install.sh` again updates the script and keeps the settings, the password and the archive. `sudo sh uninstall.sh` removes it and keeps the archive; `--purge` removes that too.

Give the display a fixed address (a DHCP reservation on the router). `http://esp32flight.local` also works where avahi runs.

## What each run does

1. Asks the display for the 15-minute slots changed since the last run (`GET /api/readings/history?fmt=json&cursor=…`). The display's cursor makes sure nothing is missed: a slot that changes after it was read (an outdoor reading arriving late, a slot still filling) comes again. After a display restart it sends everything it has once more.
2. Checks the answer (see "Security").
3. Merges the slots into `/var/lib/ws-pull/archive/YYYY-MM.csv` (UTC months). A slot that comes again replaces its row.
4. Keeps the display's new cursor, only after the files are safely written.

If the display doesn't answer, it tries 3 times, 30 s apart, then waits for the next hour.

## The archive

`/var/lib/ws-pull/archive/`, readable by root and the `ws-pull` group:

- `YYYY-MM.csv`: one row per 15-minute slot, oldest first, with the display's calibrated values. Columns:
  - `ts` (Unix seconds) and `time` (the same in UTC, ISO 8601): the quarter hour the slot's readings are nearest to (the row for 10:30 holds the readings from 10:22:30 to 10:37:30).
  - `sht_t`, `sht_rh`: outdoor temperature and humidity (SHT40). `bme_t`, `bme_rh`, `bme_p`: the BME280, pressure in hPa.
  - `p_sea`: 1 when `bme_p` is the pressure at sea level, 0 when it's the pressure at the house (the display has no altitude: no fixed home location set, or not looked up yet).
  - `bat_mv`, `rssi`, `readings`, `late`, `low_battery`: the outdoor unit's battery, signal and flags.
  - `in_t`, `in_rh`: the indoor sensor.
  - Any field the display adds later (more sensors) becomes a new column at the end.
  - Missing values are empty; true/false are 1/0.
- `status.json`: the last try, the last success, and the newest slot.
- A month file that can't be read is renamed to `….damaged-<time>`, and the next run asks the display for everything it still has.

To pull everything again: `sudo rm /var/lib/ws-pull/cursor`, then `sudo systemctl start ws-pull`.

## Files

| Path | What |
|---|---|
| `/opt/ws-pull/ws_pull.py` | the script (owned by root) |
| `/etc/ws-pull/ws-pull.conf` | settings (from `ws-pull.conf.example`) |
| `/etc/ws-pull/password` | the panel password, root only; systemd hands it to the service (`LoadCredential=`) |
| `/etc/systemd/system/ws-pull.service`, `.timer` | the service and its timer |
| `/var/lib/ws-pull/` | the cursor and the archive |

## Security

The display is treated as untrusted:

- The answer is limited to 8 MB and 60 s. No redirects, no proxies.
- Every value must be a number, true/false or null. Field names must be short, lowercase, letters, digits and `_`, with at most 64 fields per slot. Anything else rejects the whole answer: nothing is written and the cursor stays.
- Slots with a timestamp before 2025, or more than 2 days ahead, are skipped.
- Nothing from the display reaches a shell or a file name.

The service runs as its own system user in a systemd sandbox (`systemd-analyze security ws-pull.service` shows it):

- It can write only `/var/lib/ws-pull`.
- It can reach only private network addresses: no internet.
- It has no privileges.

The worst a hostile display can do is put junk numbers into the archive.

**Firewall on the desktop** (ufw, which Linux Mint includes): no incoming connections except over the VPN that reaches this computer from outside (here Tailscale's `tailscale0`). ws-pull only makes outgoing connections, so it needs no rule.

```sh
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow in on tailscale0
sudo ufw enable
```

**Copying the archive elsewhere** (e.g. a home server): give that machine an SSH account in the `ws-pull` group, limited to reading the folder (`rrsync -ro /var/lib/ws-pull/archive` as the key's forced command). The desktop then needs no way into that machine.

## Tests

```sh
python3 -I -B test_ws_pull.py
```

They run against a fake display on 127.0.0.1 and write only to a temporary folder.
