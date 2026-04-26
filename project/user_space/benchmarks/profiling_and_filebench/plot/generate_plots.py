import pandas as pd
import matplotlib.pyplot as plt
import os

def generate_profiling_plots(csv_file):
    if not os.path.exists(csv_file):
        print(f"Error: {csv_file} not found. Run 04_profiling_latency.sh first.")
        return

    df = pd.read_csv(csv_file)
    plt.figure(figsize=(10, 7))
    
    plt.plot(df['size_mb'], df['hash_ms'], marker='o', label='Hash Time', color='g')
    plt.plot(df['size_mb'], df['comp_ms'], marker='s', label='Compare Time', color='orange')
    plt.plot(df['size_mb'], df['merge_ms'], marker='^', label='Merge Time', color='m')
    plt.plot(df['size_mb'], df['total_active_ms'], marker='x', label='Total Active', color='r', linestyle='--')
    
    plt.title('Deduplication Time Components vs Dataset Size', fontsize=14)
    plt.ylabel('Time (ms)', fontsize=12)
    plt.xlabel('File Size (MB)', fontsize=12)
    plt.xscale('log', base=2)
    plt.grid(True, which="both", ls="-", alpha=0.5)
    plt.legend()

    plt.tight_layout()
    output_png = os.path.join('plot', 'profiling_detailed.png')
    plt.savefig(output_png)
    print(f"Detailed profiling graph saved as {output_png}")

if __name__ == "__main__":
    generate_profiling_plots('result/profiling_results.csv')
