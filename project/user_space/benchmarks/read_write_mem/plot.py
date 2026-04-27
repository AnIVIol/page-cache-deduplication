import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

OUTDIR = Path("plots")
OUTDIR.mkdir(exist_ok=True)

# -------------------------
# LOAD DATA
# -------------------------
mem = pd.read_csv("mem_report.csv")
read = pd.read_csv("read_report.csv")
write = pd.read_csv("write_report.csv")

# -------------------------
# 1) MEM PLOT (KEEP ONLY THIS)
# -------------------------
for test_type in mem["test_type"].unique():
    subset = mem[mem["test_type"] == test_type]

    for size in sorted(subset["size_mb"].unique()):
        s = subset[subset["size_mb"] == size].sort_values("share_percent")

        plt.plot(s["share_percent"], s["saving_kb"], marker='o', label=f"{size}MB")

    plt.axhline(0)
    plt.xlabel("share_percent")
    plt.ylabel("saving_kb")
    plt.title(f"Memory saving before and after merging({test_type} files)")
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUTDIR / f"mem_{test_type}.png")
    plt.clf()


# -------------------------
# 2) READ PLOTS (FINAL FIX)
# -------------------------

for size in sorted(read["size"].unique()):
    plt.figure()

    s = read[read["size"] == size]

    shared = s[s["file_type"].str.contains("shared", na=False)]
    normal = s[s["file_type"].str.contains("normal", na=False)]

    color_map = {}

    # ---- Plot shared curves ----
    for mode in sorted(shared["file_type"].unique()):
        sub = shared[shared["file_type"] == mode].sort_values("%shared")

        line, = plt.plot(sub["%shared"], sub["avg_ms"], marker='o', label=mode)
        color_map[mode] = line.get_color()

    # ---- Plot baselines (mean of normal) ----
    for mode in sorted(normal["file_type"].unique()):
        sub = normal[normal["file_type"] == mode]

        if sub.empty:
            continue

        y = sub["avg_ms"].mean()

        # map normal → corresponding shared
        if "rand" in mode:
            shared_key = "shared_rand"
        else:
            shared_key = "shared"

        color = color_map.get(shared_key, None)

        plt.axhline(
            y=y,
            linestyle="--",
            color=color,
            label=f"{mode} (baseline)"
        )

    plt.xlabel("%shared")
    plt.ylabel("avg_ms")
    plt.title(f"Read latency for seq. and rand. reads for normal and shared file \n(size={size} MB)")
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUTDIR / f"read_trend_{size}.png")
    plt.close()


# -------------------------
# 3) WRITE PLOTS (FIXED)
# -------------------------

shared = write[write["mode"].str.contains("shared", na=False)]
normal = write[write["mode"].str.contains("normal", na=False)]

for size in sorted(write["size"].unique()):
    plt.figure()

    s_shared = shared[shared["size"] == size]
    s_normal = normal[normal["size"] == size]

    # store colors so baseline matches line color
    color_map = {}

    # ---- Plot shared curves ----
    for mode in sorted(s_shared["mode"].unique()):
        sub = s_shared[s_shared["mode"] == mode].sort_values("%shared")

        line, = plt.plot(sub["%shared"], sub["avg_ms"], marker='o', label=mode)
        color_map[mode] = line.get_color()

    # ---- Plot baselines (normal) ----
    for mode in sorted(s_normal["mode"].unique()):
        sub = s_normal[s_normal["mode"] == mode]

        if sub.empty:
            continue

        y = sub["avg_ms"].mean()

        # map normal -> corresponding shared
        if "seq" in mode:
            shared_key = "shared_seq"
        elif "rand" in mode:
            shared_key = "shared_rand"
        else:
            continue

        color = color_map.get(shared_key, None)

        plt.axhline(
            y=y,
            linestyle="--",
            color=color,
            label=f"{mode} (baseline)"
        )

    plt.xlabel("%shared")
    plt.ylabel("avg_ms")
    plt.title(f"Write latency for sequential and random reads for normal and shared file \nsize={size} MB)")
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUTDIR / f"write_trend_{size}.png")
    plt.close()


print(f"All useful plots saved in: {OUTDIR.resolve()}")
