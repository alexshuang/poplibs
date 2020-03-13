#!/usr/bin/env python3
# Copyright (c) 2020 Graphcore Ltd, All rights reserved.

"""
A tool to run all poplibs benchmarks and update expected results.
"""

import argparse
import collections
import subprocess
import csv
import os
import sys
import tempfile
import re
from bench import read_results_file, CHANGED_RESULT_PATTERN, TestKey, Expected

BENCHMARK_RESULTS_NOTICE="""\
# This file is automatically generated and updated by update_bench.py
# Do not modify this by hand.
"""

def write_results(out_path, expected):
    with open(out_path, 'w') as results_file:
        results_file.write(BENCHMARK_RESULTS_NOTICE)
        results_writer = csv.writer(results_file,
                                    delimiter=',',
                                    lineterminator=os.linesep)
        for test_key in sorted(expected.keys()):
            entry = expected[test_key]
            results_writer.writerow(
                [test_key.target, test_key.config, test_key.name,
                 entry.cycles, entry.total_memory, entry.max_tile_memory])

def updated_results_iter(cmd):
    print('Collecting updates with command: ', *cmd)
    proc=subprocess.Popen(cmd, stdout=subprocess.PIPE)
    for line in proc.stdout:
        match = CHANGED_RESULT_PATTERN.match(line.decode('utf-8'))
        if match:
            result = match.groups()
            yield (TestKey._make(result[0:3]), Expected._make(result[3:]))

def main():
    parser = argparse.ArgumentParser(description="Benchmark results updater")
    parser.add_argument(
        "--expected_csv",
        help='Path to a file containing csv with expected results for '
             'benchmarks'
    )
    parser.add_argument("test", nargs=argparse.REMAINDER,
        help="Command to run tests"
    )
    args = parser.parse_args()

    expected_dict = read_results_file(args.expected_csv)
    
    num_updates = 0
    for test_key, expected in updated_results_iter(args.test):
        expected_dict[test_key] = expected
        print(f'Updating {test_key} with results {expected}')
        write_results(args.expected_csv, expected_dict)
        num_updates += 1

    # Finally, write out the new CSV if there were any more updates
    print(f'Done. Updated {num_updates} benchmark results')

if __name__ == "__main__":
    main()
