#!/usr/bin/env python3
"""Make the SD-card package that starts a program on the Akai MPC Sample.

usage: make_autorun.py NAME PROGRAM OUTDIR [EXTRA_FILE ...]

Writes OUTDIR/autorun/ (the folder the card needs at its top) and OUTDIR/NAME.zip (that
folder, zipped). In the folder:

  autorun.json   what the MPC Sample's script runner reads: run autorun.sh, without asking
  autorun.sh     the start-up script (below)
  autorun.check  the runner's check that the two are as written: the SHA-256, in hex, of
                 autorun.json, then autorun.sh, then the runner's fixed salt
  NAME           the program, and any extra files

The card is mounted so nothing on it can run, so the script makes it runnable, copies the
program into memory (/run) and starts it there. Started at power-on by the stock runner, it
hands the program to systemd and returns at once, so the MPC carries on starting; the program
then waits for the stock app to come up, stops it and takes over the screen and the panel,
and starts the stock app again when it exits. Started from a boot menu (which has stopped the
stock app already), it runs the program in place.
"""
import hashlib, json, os, re, shutil, stat, sys, zipfile

SALT = b"f7bed1c9-7e90-4731-9194-c996666b4331"  # the stock runner's, for autorun.check

SCRIPT = r'''#!/bin/sh
# Starts @NAME@ on the MPC Sample. Made by tools/make_autorun.py.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
name=@NAME@

# The card is mounted noexec: find its mount point (the longest one that holds this folder)
# and mount it again with exec, so the program's own files there can run if it needs them.
mount_point=
best=0
while read -r dev point rest; do
    point=$(printf '%b' "$point") # spaces come as \040
    case "$here/" in
        "$point"/*) if [ ${#point} -gt "$best" ]; then mount_point=$point; best=${#point}; fi ;;
    esac
done < /proc/mounts
[ -n "$mount_point" ] && mount -o remount,exec "$mount_point" 2>/dev/null || true

# Already running: nothing to do.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet "$name.service"; then
    exit 0
fi

cp "$here/$name" "/run/$name"
chmod 755 "/run/$name"

# What runs the program: with MPC_APP_DIR its folder on the card; the stock app (ac50)
# stopped first, unless a boot menu has done that, and started again after.
cat > "/run/$name-start.sh" <<'EOF'
#!/bin/sh
export MPC_APP_DIR="$1"
prog=$2
menu=${3:-}
has_systemd() { command -v systemctl >/dev/null 2>&1; }
if [ -z "$menu" ] && [ ! -f /run/mpc-boot-thingy.pid ] && has_systemd; then
    tries=0
    while [ "$tries" -lt 45 ]; do # up to 45 s for the stock app to come up, then stop it
        if systemctl is-active --quiet ac50.service; then
            "/run/$prog" --codec-volume 0 || true # the codec glides down first (it waits for it): no click as the stock app stops
            systemctl stop ac50.service || true
            break
        fi
        tries=$((tries + 1))
        sleep 1
    done
fi
code=0
"/run/$prog" || code=$?
"/run/$prog" --codec-volume 255 || true # back up, whatever happened, for the stock app
if [ ! -f /run/mpc-boot-thingy.pid ] && has_systemd; then
    systemctl start ac50.service || true
fi
exit "$code"
EOF
chmod 755 "/run/$name-start.sh"

# From a boot menu: run here and now (its pid file, or its hand-off unit, says so).
if [ -f /run/mpc-boot-thingy.pid ] || grep -q 'mpc-boot-autorun\.service' /proc/self/cgroup 2>/dev/null; then
    exec /bin/sh "/run/$name-start.sh" "$here" "$name" menu
fi

# From the stock runner at power-on: queue it and return, so the MPC finishes starting.
systemd-run --unit="$name" --collect --no-block /bin/sh "/run/$name-start.sh" "$here" "$name"
'''


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    name, program, out = sys.argv[1], sys.argv[2], sys.argv[3]
    extras = sys.argv[4:]
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name) or name.startswith("autorun"):
        sys.exit(f"make_autorun: not a usable name: {name}")
    if not out or os.path.abspath(out) in ("/", os.path.abspath(".")):
        sys.exit(f"make_autorun: refusing to write into {out!r}")
    pkg = os.path.join(out, "autorun")
    shutil.rmtree(pkg, ignore_errors=True)
    os.makedirs(pkg)

    doc = {"value0": {"scripts": [{"path": "autorun.sh", "askQuestion": False, "message": f"Start {name}?",
                                   "yesButtonText": "Start", "noButtonText": "Cancel"}], "files": []}}
    js = (json.dumps(doc, indent=2) + "\n").encode()
    sh = SCRIPT.replace("@NAME@", name).encode()
    check = hashlib.sha256(js + sh + SALT).hexdigest().encode()

    files = {"autorun.json": js, "autorun.sh": sh, "autorun.check": check}
    for fname, data in files.items():
        with open(os.path.join(pkg, fname), "wb") as f:
            f.write(data)
    shutil.copyfile(program, os.path.join(pkg, name))
    for extra in extras:
        base = os.path.basename(extra)
        if base.startswith(".") or base in files or base == name:
            sys.exit(f"make_autorun: refusing to add {base}")
        shutil.copyfile(extra, os.path.join(pkg, base))
    for fname in ["autorun.sh", name] + [os.path.basename(e) for e in extras]:
        p = os.path.join(pkg, fname)
        os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    zpath = os.path.join(out, f"{name}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for fname in sorted(os.listdir(pkg)):
            info = zipfile.ZipInfo(f"autorun/{fname}", date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o755 if fname in ("autorun.sh", name) or fname in map(os.path.basename, extras) else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(os.path.join(pkg, fname), "rb") as f:
                z.writestr(info, f.read())
    print(zpath)


if __name__ == "__main__":
    main()
