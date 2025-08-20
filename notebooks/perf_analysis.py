#!/usr/bin/env python3

"""
This script processes and analyzes performance log files to visualize system latency.
It handles two types of latency data:
1. RDTSC: Measures the duration of specific, localized operations.
2. TTT (Time-To-Travel): Measures the latency between two different events in the system's critical path.

The script generates and displays plots for each type of analysis.
example usage:
    python3 perf_analysis.py ../exchange*.log "../*_1.log" --cpu-freq 2.60
"""

import argparse
import glob
import pandas as pd
import plotly.graph_objects as go
import session_info

# Suppress pandas' chained assignment warning
pd.options.mode.chained_assignment = None

def parse_log_files(log_patterns, cpu_freq):
    """
    Parses log files matching the given patterns to extract RDTSC and TTT latency data.

    Args:
        log_patterns (list): A list of file path patterns (e.g., ['../exchange*.log']).
        cpu_freq (float): The CPU frequency in GHz for converting RDTSC cycles to nanoseconds.

    Returns:
        tuple: A tuple containing two pandas DataFrames: (rdtsc_df, ttt_df).
    """
    rdtsc_data = []
    ttt_data = []

    # Flatten the list of lists of files
    all_files = [file for pattern in log_patterns for file in glob.glob(pattern)]

    for filename in all_files:
        print(f"Processing {filename}...")
        with open(filename, "r", encoding="utf-8") as f:
            for line in f:
                tokens = line.strip().split(' ')
                if len(tokens) != 4:
                    continue

                try:
                    time, log_type, tag, latency_val = tokens
                    latency = float(latency_val)
                except (ValueError, IndexError):
                    continue

                if log_type == 'RDTSC':
                    latency_ns = latency / cpu_freq
                    rdtsc_data.append({'timestamp': time, 'tag': tag, 'latency': latency_ns})
                elif log_type == 'TTT':
                    ttt_data.append({'timestamp': time, 'tag': tag, 'latency': latency})

    # Create and process RDTSC DataFrame
    rdtsc_df = pd.DataFrame(rdtsc_data)
    if not rdtsc_df.empty:
        rdtsc_df = rdtsc_df.drop_duplicates().sort_values(by='timestamp')
        rdtsc_df['timestamp'] = pd.to_datetime(rdtsc_df['timestamp'], format='%H:%M:%S.%f')

    # Create and process TTT DataFrame
    ttt_df = pd.DataFrame(ttt_data)
    if not ttt_df.empty:
        ttt_df = ttt_df.drop_duplicates().sort_values(by='timestamp')
        ttt_df['timestamp'] = pd.to_datetime(ttt_df['timestamp'], format='%H:%M:%S.%f')
        
    return rdtsc_df, ttt_df

def plot_rdtsc(rdtsc_df):
    """Generates and displays latency plots for each RDTSC tag."""
    if rdtsc_df.empty:
        print("No RDTSC data to plot.")
        return

    for tag in rdtsc_df['tag'].unique():
        print(f"\nAnalyzing RDTSC for tag: {tag}")
        fig = go.Figure()
        t_df = rdtsc_df[rdtsc_df['tag'] == tag].copy()
        t_df = t_df[t_df['latency'] > 0]

        if len(t_df) < 2:
            print(f"Not enough data points for tag {tag} to plot.")
            continue

        # Filter out top and bottom 1% outliers
        q_hi = t_df['latency'].quantile(0.99)
        q_lo = t_df['latency'].quantile(0.01)
        t_df = t_df[(t_df['latency'] < q_hi) & (t_df['latency'] > q_lo)]

        mean = t_df['latency'].mean()
        print(f"  Observations: {len(t_df)}, Mean Latency: {mean:.2f} ns")

        unit = 'nanoseconds'
        if mean >= 1000:
            unit = 'microseconds'
            t_df['latency'] /= 1000

        rolling_window = max(1, len(t_df) // 100)
        fig.add_trace(go.Scatter(x=t_df['timestamp'], y=t_df['latency'], mode='lines', name=tag))
        fig.add_trace(go.Scatter(x=t_df['timestamp'], y=t_df['latency'].rolling(rolling_window).mean(), name=f'{tag} (mean)'))
        
        fig.update_layout(
            title=f'RDTSC Performance: {tag} ({unit})',
            height=750,
            width=1000,
            hovermode='x unified',
            legend=dict(yanchor="top", y=0.99, xanchor="left", x=0.01)
        )
        fig.show()

def plot_hops(ttt_df):
    """Generates and displays hop latency plots based on predefined paths."""
    if ttt_df.empty:
        print("No TTT data to plot.")
        return

    HOPS = [
        ['T1_OrderServer_TCP_read', 'T2_OrderServer_LFQueue_write'],
        ['T2_OrderServer_LFQueue_write', 'T3_MatchingEngine_LFQueue_read'],
        ['T3_MatchingEngine_LFQueue_read', 'T4_MatchingEngine_LFQueue_write'], ['T3_MatchingEngine_LFQueue_read', 'T4t_MatchingEngine_LFQueue_write'],
        ['T4_MatchingEngine_LFQueue_write', 'T5_MarketDataPublisher_LFQueue_read'], ['T4t_MatchingEngine_LFQueue_write', 'T5t_OrderServer_LFQueue_read'],
        ['T5_MarketDataPublisher_LFQueue_read', 'T6_MarketDataPublisher_UDP_write'], ['T5t_OrderServer_LFQueue_read', 'T6t_OrderServer_TCP_write'],
        ['T7_MarketDataConsumer_UDP_read', 'T8_MarketDataConsumer_LFQueue_write'], ['T7t_OrderGateway_TCP_read', 'T8t_OrderGateway_LFQueue_write'],
        ['T6_MarketDataPublisher_UDP_write', 'T7_MarketDataConsumer_UDP_read'], ['T6t_OrderServer_TCP_write', 'T7t_OrderGateway_TCP_read'],
        ['T8_MarketDataConsumer_LFQueue_write', 'T9_TradeEngine_LFQueue_read'], ['T8t_OrderGateway_LFQueue_write', 'T9t_TradeEngine_LFQueue_read'],
        ['T9_TradeEngine_LFQueue_read', 'T10_TradeEngine_LFQueue_write'], ['T9t_TradeEngine_LFQueue_read', 'T10_TradeEngine_LFQueue_write'],
        ['T10_TradeEngine_LFQueue_write', 'T11_OrderGateway_LFQueue_read'],
        ['T11_OrderGateway_LFQueue_read', 'T12_OrderGateway_TCP_write'],
        # exchange <-> client
        ['T12_OrderGateway_TCP_write', 'T1_OrderServer_TCP_read'],
        ['Ticker_Received', 'Order_Sent'],  # tick-to-trade延迟路径
    ]

    for tags in HOPS:
        tag_p, tag_n = tags
        print(f"\nAnalyzing HOP: {tag_p} -> {tag_n}")
        
        t_df = ttt_df[(ttt_df['tag'] == tag_n) | (ttt_df['tag'] == tag_p)].copy()
        t_df['latency_diff'] = t_df['latency'].diff()
        
        # Filter for valid diffs at the destination tag
        t_df = t_df[(t_df['latency_diff'] > 0) & (t_df['tag'] == tag_n)]

        if len(t_df) < 2:
            print(f"  Not enough data points for this hop to plot.")
            continue
        
        # Filter outliers
        q_hi = t_df['latency_diff'].quantile(0.99)
        q_lo = t_df['latency_diff'].quantile(0.01)
        t_df = t_df[(t_df['latency_diff'] < q_hi) & (t_df['latency_diff'] > q_lo)]

        mean = t_df['latency_diff'].mean()
        print(f"  Observations: {len(t_df)}, Mean Hop Latency: {mean:.2f} ns")

        unit = 'nanoseconds'
        if mean >= 1_000_000:
            unit = 'milliseconds'
            t_df['latency_diff'] /= 1_000_000
        elif mean >= 1_000:
            unit = 'microseconds'
            t_df['latency_diff'] /= 1_000

        fig = go.Figure()
        tag_name = f'{tag_p} -> {tag_n}'
        rolling_window = max(1, len(t_df) // 100)
        fig.add_trace(go.Scatter(x=t_df['timestamp'], y=t_df['latency_diff'], mode='lines', name=tag_name))
        fig.add_trace(go.Scatter(x=t_df['timestamp'], y=t_df['latency_diff'].rolling(rolling_window).mean(), name=f'{tag_name} (mean)'))

        fig.update_layout(
            title=f'Hop Performance: {tag_name} ({unit})',
            height=750,
            width=1000,
            hovermode='x unified',
            legend=dict(yanchor="top", y=0.99, xanchor="left", x=0.01)
        )
        fig.show()

def main():
    """Main function to parse arguments and run the analysis."""
    parser = argparse.ArgumentParser(description="Analyze performance logs.")
    parser.add_argument(
        'log_patterns',
        nargs='+',
        help='One or more log file patterns to process (e.g., "../exchange*.log").'
    )
    parser.add_argument(
        '--cpu-freq',
        type=float,
        default=2.60,
        help='CPU frequency in GHz for RDTSC conversion (default: 2.60).'
    )
    args = parser.parse_args()

    rdtsc_df, ttt_df = parse_log_files(args.log_patterns, args.cpu_freq)
    
    plot_rdtsc(rdtsc_df)
    plot_hops(ttt_df)

    print("\n--- Session Info ---")
    session_info.show()

if __name__ == "__main__":
    main()