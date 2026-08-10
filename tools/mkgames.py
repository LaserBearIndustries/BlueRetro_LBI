"""Turn BlueRetroWebCfg's gameid.db into the list the companion app reads.

Only the 16 hex character ids are kept. Those are the 8 byte binary form the
GameCube and N64 use, which is what a GameCube adapter will ever be asked to
look up; the SLUS/SCUS style ids in the same table are PlayStation and would
only be dead weight on the card.

N64 ids share the format and are kept with them, since nothing distinguishes
the two and a 64 bit value colliding across systems is not a real concern.
"""
import binascii
import re
import sqlite3
import sys

src, dst = sys.argv[1], sys.argv[2]

rows = sqlite3.connect(src).execute("select id, name from games").fetchall()

# Ids the database has no reason to carry. The companion's own matters because
# launching it is a launch like any other, so it heads the recent list every
# time; without this it sits there as sixteen hex characters, in the one slot
# nobody should be picking.
rows = [
    ("7E4B33591DF690B4", "BlueRetro Companion (this app)"),
] + rows

seen = {}
skipped_fmt = 0
skipped_dup = 0
skipped_null = 0

for gid, name in rows:
    gid = (gid or "").strip().upper()
    name = (name or "").strip()

    if len(gid) != 16 or not re.fullmatch(r"[0-9A-F]{16}", gid):
        skipped_fmt += 1
        continue
    if gid == "0" * 16:
        # Homebrew and passthrough carts that never filled the field in. Real
        # entries, but they all share one id, so a title would be a coin toss.
        skipped_null += 1
        continue
    if not name:
        continue
    if gid in seen:
        skipped_dup += 1
        continue

    # The reader splits on the first '=' and takes the rest of the line, so a
    # title may contain '=' but not a newline.
    seen[gid] = name.replace("\r", " ").replace("\n", " ")

with open(dst, "w", encoding="ascii", errors="replace", newline="\n") as f:
    for gid in sorted(seen):
        f.write("%s=%s\n" % (gid, seen[gid]))

print("rows in db      : %d" % len(rows))
print("wrong format    : %d  (PlayStation and friends)" % skipped_fmt)
print("all-zero id     : %d" % skipped_null)
print("duplicate id    : %d" % skipped_dup)
print("written         : %d" % len(seen))
