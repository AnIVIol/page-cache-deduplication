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
    plt.title(f"Memory saving vs sharing ({test_type})")
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUTDIR / f"mem_{test_type}.png")
    plt.clf()


# -------------------------
# 2) READ PLOTS
# -------------------------

# Clean normal vs shared labels
read["type"] = read["file_type"].fillna("normal")

# ---- (A) Trend: shared performance vs %shared ----
shared = read[read["file_type"].str.contains("shared", na=False)]

for size in sorted(shared["size"].unique()):
    s = shared[shared["size"] == size]

    for mode in s["file_type"].unique():
        sub = s[s["file_type"] == mode].sort_values("%shared")
        plt.plot(sub["%shared"], sub["avg_ms"], marker='o', label=mode)

    plt.xlabel("%shared")
    plt.ylabel("avg_ms")
    plt.title(f"Read latency vs sharing (size={size})")
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUTDIR / f"read_trend_{size}.png")
    plt.clf()


# ---- (B) Bar: normal vs random vs shared ----
agg = read.groupby("file_type")["avg_ms"].mean().sort_values()

plt.bar(agg.index.astype(str), agg.values)
plt.ylabel("avg_ms")
plt.title("Read performance comparison (lower is better)")
plt.xticks(rotation=30)
plt.tight_layout()
plt.savefig(OUTDIR / "read_comparison.png")
plt.clf()


# -------------------------
# 3) WRITE PLOTS
# -------------------------

# ---- (A) Trend: shared vs %shared ----
shared = write[write["mode"].str.contains("shared", na=False)]

for size in sorted(shared["size"].unique()):
    s = shared[shared["size"] == size]

    for mode in s["mode"].unique():
        sub = s[s["mode"] == mode].sort_values("%shared")
        plt.plot(sub["%shared"], sub["avg_ms"], marker='o', label=mode)

    plt.xlabel("%shared")
    plt.ylabel("avg_ms")
    plt.title(f"Write latency vs sharing (size={size})")
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUTDIR / f"write_trend_{size}.png")
    plt.clf()


# ---- (B) Bar: overall write modes ----
agg = write.groupby("mode")["avg_ms"].mean().sort_values()

plt.bar(agg.index.astype(str), agg.values)
plt.ylabel("avg_ms")
plt.title("Write performance comparison (lower is better)")
plt.xticks(rotation=30)
plt.tight_layout()
plt.savefig(OUTDIR / "write_comparison.png")
plt.clf()


print(f"All useful plots saved in: {OUTDIR.resolve()}")
