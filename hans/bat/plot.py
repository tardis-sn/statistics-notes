import numpy as np
from matplotlib import pyplot as plt
from scipy.integrate import simps
import sys

def plot_estimate(run):
    a = np.loadtxt("out%s.txt" % run)

    run = int(run)

    # modify data to include the origin
#     a = np.vstack((np.zeros(2), a))

    X = a.T[0]
    P = np.nan_to_num(a.T[1])

    # check normalization of pdf using Simpson's rule
    norm = simps(P, X)
    print("Normalization", norm)

    # improve estimates by renormalizing to one
    P /= norm

    mean = simps(P * X, X)
    print("Mean", mean)

    stddev = np.sqrt(simps(P * (X - mean)**2, X))
    print("std. dev", stddev)

    mode = X[np.argmax(P)]
    print("Mode", mode)

    plt.plot(X, P)
    plt.xlabel("$X$")
    plt.ylabel("$P(X)$")

    replicas = np.loadtxt("X.out")
    print("Observed %d packets and X = %g in run %d" % (replicas[run,0], replicas[run,1], run))
    print("Mean number of packets in bin", replicas.T[0].mean())
    plt.hist(replicas.T[1], bins=15, normed=True)
    plt.vlines(replicas[run, 1], 0, P.max(), color='red', linestyle='dashed')

    plt.title("Mode = %g\nmean x = %g +- %g\nsample mean %g +- %g" % (mode, mean, stddev, replicas.T[1].mean(), replicas.T[1].std()))

    plt.tight_layout()
    # plt.show()
    plt.savefig("X%s.pdf" % run)

    plt.clf()
    plt.hist(replicas.T[0], bins=15, normed=False)
    plt.xlabel("#packets in bin")
    plt.tight_layout()
    plt.savefig("npackets.pdf")

if __name__ == '__main__':
    plot_estimate(sys.argv[1])

# Local Variables:
# compile-command: "python3 plot.py"
# End: