import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
table = (root / "main/diagnostics.cpp").read_text()

sources = subprocess.run(["git", "ls-files", "--cached", "--others",
                          "--exclude-standard", "*.cpp"], cwd=root, check=True,
                         capture_output=True, text=True).stdout.split()

missing = []
for name in sources:
    text = (root / name).read_text()
    used = set(re.findall(r"ESP_LOG[EWIDV]\(\s*([A-Za-z_]\w*)\s*,", text))
    used |= set(re.findall(r"ESP_(?:RETURN|GOTO)_ON_\w+\([^;]*?,\s*([A-Z_]\w*)\s*,\s*\"", text))
    for tag in used:
        found = re.search(r"constexpr char " + tag + r"\[\]\s*=\s*\"([^\"]+)\"", text)
        if found and '"%s"' % found.group(1) not in table:
            missing.append((found.group(1), name))

if missing:
    for tag, name in sorted(set(missing)):
        print("unmapped log tag: %-12s (%s)" % (tag, name))
    print("FAIL: add them to a tag set in main/diagnostics.cpp")
    sys.exit(1)
print("ALL PASS")
