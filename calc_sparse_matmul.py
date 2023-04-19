import argparse
import numpy as np


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--actual', required=True, help='output filename of static sparse custom op')
    parser.add_argument('--expected', required=True, help='output filename of poplibs static matmul tool')
    parser.add_argument('--atol', type=float, default=0, help='atol')
    parser.add_argument('--rtol', type=float, default=1e-7, help='rtol')
    args = parser.parse_args()

    actual = np.array([eval(o) for o in open(args.actual).read().split()])
    expected = np.array([eval(o) for o in open(args.expected).read().split()])
    assert(len(actual) == len(expected))
    # np.testing.assert_allclose(actual, expected, atol=args.atol, rtol=args.rtol)

    n_elems = len(actual)
    diff = np.abs(actual - expected)
    mismatched_elements = np.count_nonzero(~np.isclose(actual, expected))
    mismatch_ratio = mismatched_elements / n_elems
    print(f'\n{"/".join(args.actual.split("/")[-4:-2])}: ({mismatched_elements} / {n_elems}) {round(mismatch_ratio * 100, 1)}% mismatched elements, max abs diff = {np.max(diff)}\n')

