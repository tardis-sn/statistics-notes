from __future__ import print_function

from astropy.modeling import Model, Parameter
import math
from math import log
import numpy as np
import scipy
from scipy.special import gammaincc, gammaln
from scipy.stats import gaussian_kde
from scipy.stats import norm as gaussian

'''
Implement likelihood that estimates uncertainty due to tardis simulation
'''

def log_pochhammer(a, b):
    '''
    Pochhammer symbol (a)_b implemented for integer ``b``.
    '''
    assert a > 0
    assert b >= 0
    return gammaln(a+b) - gammaln(a)


def poisson_posterior_predictive(N, n):
    '''
    Poisson posterior predictive probability distribution to observe ``N`` given ``n`` on the log scale.

    A Jeffrey's prior is assumed for the rate parameter. The derivation is given in arXiv:1112.2593
    '''
    return math.exp(log_pochhammer(N + n + 1, N + n) - log_pochhammer(n + 1, n) \
           - (3 * N + n + 0.5) * math.log(2) - gammaln(N + 1))


def binary_search_min(data, key, low=None, high=None):
    '''
    Find the index of the smallest element in ``data`` that is >= ``key``.
    '''

    # update low and high until element found
    if low is None:
        low = 0
    if high is None:
        high = len(data) - 1

    assert low < high

    # first element is the right one
    if data[0] >= key:
        return low
    if data[-1] < key:
        raise KeyError('Key ' + str(key) + ' exceeds all elements')

    while low < (high - 1):
        mid = (low + high) // 2
        midval = data[mid]
        if midval >= key:
            high = mid
        if midval < key:
            low = mid

    return high


def binary_search_max(data, key, low=None, high=None):
    '''
    Find the index of the first element in ``data`` that is > ``key``.
    '''

    # update low and high until element found
    if low is None:
        low = 0
    if high is None:
        high = len(data) - 1

    assert low < high

    # first element is the right one
    if data[-1] <= key:
        return high
    if data[0] > key:
        raise KeyError('Key ' + str(key) + ' less than all elements')

    while low < (high - 1):
        mid = (low + high) // 2
        midval = data[mid]
        if midval <= key:
            low = mid
        if midval > key:
            high = mid

    return low


def binary_search_min_max(data, minkey, maxkey, low=None, high=None):
    '''
    Find the indices of the smallest element exceeding ``minkey`` and of the
    first element exceeding ``maxkey``.

    :param data:
    :param minkey:
    :param maxkey:
    :param low:
    :param high:
    :return:
    '''
    imin = binary_search_min(data, minkey, low, high)
    imax = binary_search_max(data, maxkey, imin, high)
    return (imin, imax)


def expand_bin(N, imin, imax, K):
    '''
    Expand bin defined by lower index ``imin`` and upper index ``imax``
    to contain ``N`` elements. The indices range from 0 to K.

    Attempt to add elements symmetrically on either side. This stops once a
    boundary is hit. If ``len(data) < N``, less element are in the expanded bin.

    :param K: 1D data
    :param imin:
    :param imax:
    :param N:
    :return: tuple (jmin, jmax)
    '''
    assert imin >= 0
    assert imax >= imin
    assert imax <= K
    assert N >= 0
    assert K >= 0

    # combine less elements if data not long enough
    N = min(N, K)

    # elements already in
    n = imax - imin

    # grow to the left first, try to add rest of needed elements, then
    # repeat on the right. If not enough, add more to the left
    jmultimin = max(0, imin - (N - n) // 2)
    jmultimax = min(K, imax + (N - (imax - jmultimin)))
    missing = N- (jmultimax - jmultimin)
    if missing > 0:
        jmultimin = max(0, jmultimin - missing)

    assert jmultimax - jmultimin == N

    return (jmultimin, jmultimax)


def bin_indices(flux, data):
    '''

    :param flux:
    :param data:
    :return:
    '''
    pass

def prob_L_given_N_empirical(N, lsamples):
    '''
    Approximate probability of luminosity ``L`` from generating sums of ``N`` samples
    from ``lsamples`` and interpolating the probability density through Gaussian kernel
    density estimation.

    :param L:
    :param N:
    :param lsamples:
    :param mean:
    :param sigma:
    :return:
    '''

    # todo move loop to cython/C
    summed_samples = np.empty(len(lsamples) // N, dtype=lsamples.dtype)
    assert len(summed_samples) > 1, "Need more than one sample for KDE"
    for i in range(len(summed_samples)):
        summed_samples[i] = lsamples[i:i+N].sum()

    return gaussian_kde(summed_samples, bw_method='scott')

def prob_L_given_N(N, lsamples, mean, variance, N_critical=10):
    '''
    Redirector to prob_L_given_N_empirical or prob_L_given_N_CLT depending on ``N``.

    '''

    assert N > 0

    if N >= N_critical:
        return gaussian(N * mean, math.sqrt(N * variance)).pdf
    else:
        kde = prob_L_given_N_empirical(N, lsamples)
        return lambda L: kde(L)[0] if np.isscalar(L) else kde(L)


def prob_L_given_theta(L, lsamples, imin, imax, nsum=400, eps=1e-3):
    '''P(L|\theta) estimated from a single tardis simulation
    for a single bin.

    :param L: Luminosity (scalar)
    :param lsamples: Array of packet luminosities from tardis
    :param imin: index of first packet in the frequency bin
    :param imax: index of one-past-the-last element in the frequency bin
    :param nsum: number of samples used for KDE interpolation
    :param eps: Relative precision. The contribution for different possible
           number of packets observed is truncated when its contribution to
           the total probability is estimated to be less than ``eps``.
    :return: probability
    '''

    # number of simulated packets and sum of luminosities in the bin i
    ni = imax - imin
    li = lsamples[imin:imax].sum()

    # obtain multibin of luminosities
    # make it large enough to accurately estimate the distribution
    jmultimin, jmultimax = expand_bin(nsum, imin, imax, len(lsamples))
    lmultibin = lsamples[jmultimin:jmultimax]

    # sample mean and standard deviation
    mean, variance = np.mean(lmultibin), np.var(lmultibin)

    # approximate the sum over N, the number of packets one could have seen:
    # 1. Start with N=n, this should be the biggest single contribution
    # 2. Then expand to left and right, add up terms until negligible
    # 3. If N large enough, use central limit theorem
    prob_N_up = poisson_posterior_predictive(ni, ni)
    prob_N_down = prob_N_up
    res = prob_N_up * prob_L_given_N(ni, lmultibin, mean, variance)(L)
    go_up = True
    go_down = True if ni > 1 else False
    N_up, N_down = ni, ni

    while go_up or go_down:
        # store contribution of this expansion
        contrib = 0

        if go_up:
            # posterior predictive P(N+1|n)/P(N|n) = [2(N+n)+1] / [4 (N+1)]
            prob_N_up *= (2. * (N_up + ni) + 1) / (4 * (N_up + 1))

            N_up += 1
            contrib += prob_N_up * prob_L_given_N(N_up, lsamples, mean, variance)(L)

        if go_down:
            # lowest contribution is from N=1. For N=0 packets, there is
            # definitely no contribution to the luminosity
            if N_down == 2:
                go_down = False
            # posterior predictive ratio P(N+1|n)/P(N|n) = 4N / [2(N+n)-1]
            prob_N_down *= (4. * N_down) / (2 * (N_down + ni) - 1)

            N_down -= 1
            contrib += prob_N_down * prob_L_given_N(N_down, lsamples, mean, variance)(L)

        print("N_up: %d, N_down: %d, res: %g, contrib: %g" %
              (N_up, N_down, res, contrib))

        # compare contribution and quit
        if contrib / res < eps:
            go_down = go_up = False

        assert N_up < 2 * ni

        # always use known contribution
        res += contrib

    return res

def amoroso_log_likelihood(parameters, samples, invert=False):#samples, a, mu, sigma, l):
    '''
    -log(likelihood) if all ``samples`` follow an Amoroso distribution in the
    Lawless parametrization.

    :param parameters: a, mu, sigma, l
    :param samples: the samples

    :return: log(likelihood), gradient
    '''
    a, mu, sigma, l = parameters

    N = len(samples)
    lsqinv = l**2
    # scalar
    res = log(math.fabs(l) / sigma) - gammaln(lsqinv) - mu / (l * sigma) + lsqinv * log(lsqinv)
    res *= N

    # invert the centering if ``a`` is the maximum instead of a minimum
    # vector like
    centered = np.log(a - samples) if invert else np.log(samples - a)

    tmp = np.isnan(centered)
    if tmp.any():
        raise ValueError("Encountered nan in centered samples at" +
                         str(np.where(tmp)[0]))

    v = centered.copy()
    v *= (1.0 - 1.0 / (l * sigma))
    tmp = np.exp(-l / sigma * centered)
    v += (lsqinv * math.exp(l * mu / sigma)) * tmp

    res -= v.sum()

    # invert log(likelihood)
    return -res #, gradient


def amoroso_max_likelihood(samples, initial_guess=None, invert=None):
    '''
    Extract the four parameters of an Amoroso distribution through maximum
    likelihood.

    The usual parametrization is that of Crooks, `Survey of simple
    continuous, univariate probability distributions` (2014). But internally,
    the parametrization due to Lawless is used to improve convergence and the
    values in that parametrization are returned.

    :param samples: 1D samples
    :return: (a, mu, sigma, lambda)
    '''
    # todo compute sample_moments and solve to get first estimate
    if initial_guess is None:
        initial_guess = [samples.max(), 0.5, 0.5, 0.5]

    bounds = [(None, 1.05 * samples.max()),
              (None, None),
              (None, None),
              (0, None)]

    return scipy.optimize.minimize(amoroso_log_likelihood, initial_guess,
                                   args=(samples, invert),
                                   bounds=bounds, options=dict(disp=True))


def alpha_mu_moment(k, alpha, mu, rhat=1.0):
    '''
    Highly accurate closed-form approximations to the sum of alpha-mu variates and applications, Eq. (2)
    '''
    return math.exp(k * log(rhat) + gammaln(mu + k / alpha) - k / alpha * log(mu) - gammaln(mu))


def alpha_mu_log_likelihood(parameters, samples, grad=False):
    '''negative log likelihood because scipy wants to minimize'''
    a, rhat, alpha, mu = parameters

    gradient = np.zeros(4)

    if grad:
        error = 1e10, gradient
    else:
        error = 1e10

    if alpha < 0.0:
        print("Returning error for alpha < 1")
        return error

    if mu <= 0.0:
        print("Returning error for negative mu")
        return error

    if rhat == 0.0:
        print("Returng error for zero rhat")
        return error

    # data-independent part
    res = log(alpha) - gammaln(mu) + mu * log(mu) - log(math.fabs(rhat))

    samples = np.asarray(samples)

    # handle scalar and array samples uniformly
    N = 1
    if hasattr(samples, 'len'):
        N = len(samples)
        res *= N

    # standardize with location and scale parameter, negative scale means a is the maximum
    standardized = samples - a
    standardized /= rhat

    # can't take log of negative number, so stop here
    if np.isnan(standardized).any():
        print("returning inf for nan in standardized")
        return error

    if (standardized <= 0).any():
        print("returning inf for negative standardized")
        # print(samples, a, rhat, standardized)
        return error

    # add the data-dependent parts
    res += (alpha * mu - 1) * np.log(standardized).sum()
    res -= mu * np.power(standardized, alpha).sum()

    if grad:
        gradient[0] = -(alpha * mu - 1) * (rhat * standardized).sum() \
                  + alpha * mu / rhat * np.power(standardized, alpha - 1).sum()
        gradient[1] = N / alpha + mu * np.log(standardized).sum() \
                  - mu * np.power(standardized, alpha).dot(np.log(standardized))
        gradient[2] = N * (1.0 + log(mu) + scipy.special.digamma(mu)) \
                  + alpha * np.log(standardized).sum() - np.power(standardized, alpha).sum()
        gradient[3] = -N / rhat * alpha * mu + mu * alpha / rhat * standardized.dot(np.power(standardized, alpha - 1))

        return -res, -gradient
    else:
        # negate
        return -res


def alpha_mu(x, alpha, mu, rhat, a=0.0):
    res = alpha_mu_log_likelihood((a, rhat, alpha, mu), x, grad=False)
    return np.exp(-res)
    # return alpha * math.pow(mu, mu) * np.power(x, alpha * mu - 1) / math.pow(rhat, alpha * mu) / math.exp(gammaln(mu)) * np.exp((-mu / math.pow(rhat, alpha)) * np.power(x, alpha))

def alpha_mu_max_likelihood(samples, initial_guess=None):
    '''
    Extract the four parameters of an alpha-mu distribution through maximum
    likelihood.

    Follow Benevides da Costa, Yacoub and Silveira Santos Filho (2008) and
    add the additional location parameter ``a``.

    :param samples: 1D samples
    :param invert: float; if ``-1``, consider all samples ``<= a``, else
     all samples `` >= a``.
    :return: (a, alpha, mu, rhat)
    '''
    #
    if initial_guess is None:
        initial_guess = [1.005 * samples.max(), 1.5 * samples.std(), 1.7, 2.2]
        # for negative skew, use negative scale parameter
        if scipy.stats.skew(samples) < 0:
            initial_guess[1] *= -1

    bounds = [(None, None),
              (0.0, None) if initial_guess[1] > 0 else (None, 0.0),
              (1.0, None),
              (0.0, None)
              ]

    print("initial guess", initial_guess)

    kwargs = dict(bounds=bounds, jac=True, options=dict(disp=True, maxiter=100))
    # kwargs['method'] = 'L-BFGS-B' #; kwargs['options']['factr'] = 10
    # kwargs['method'] = 'Newton-CG'
    kwargs['method'] = 'Powell'
    # kwargs['method'] = 'CG'
    # kwargs['method'] = 'COBYLA'; kwargs['options']['rhobeg'] = 0.01 # doesn't stop anymore!
    kwargs['args'] = (samples, kwargs['jac'])
    return scipy.optimize.minimize(alpha_mu_log_likelihood, initial_guess, **kwargs)

def alpha_mu_max_likelihood_nlopt(samples):
    import nlopt

    opt = nlopt.opt(nlopt.LN_BOBYQA, 4)
    opt.set_max_objective(lambda x, grad: alpha_mu_log_likelihood(x, samples))


    bounds_low  = [v[0]  for v in f.values]
    bounds_high = [v[-1] for v in f.values]

    m = samples.max()
    std = samples.std()
    skew = scipy.stats.skew(samples)
    opt.set_lower_bounds([m, 0, 0, 10 * std if skew < 0 else 0])
    opt.set_upper_bounds([1.05 * m, 20, 20, 0 if skew < 0 else 10 * std])

    tol = 1e-12
    opt.set_ftol_abs(tol)
    opt.set_xtol_rel(sqrt(tol))
    opt.set_maxeval(1000)

    if initial_guess is None:
        initial_guess = np.array([1.01 * m, 1.1, 1.03, std])
        # for negative skew, use negative scale parameter
        if skew < 0:
            initial_guess[2] *= -1

    xopt = opt.optimize(initial_guess)
    fmin = opt.last_optimum_value()

    print(" Found", xopt, ", min. f =", fmin, "reached after")


def lawless_to_crooks(a, mu, sigma, l):
    '''Convert the Amoroso parameters from the Lawless to the Crooks
    parametrization.

    :return: (a, theta, alpha, beta)
    '''
    return a, math.exp(mu + sigma / l * math.log(l**2)), 1.0 / l**2, l / sigma

def amoroso_cdf(x, parameters):
    '''Cumulative of the Amoroso at x given parameters (a, theta, alpha, beta)'''
    a, theta, alpha, beta = parameters
    z =  (x - a) / theta
    # avoid NaN
    if theta < 0:
        if x >= a:
            return 1.0
        else:
            return gammaincc(alpha, z**beta)
    else:
        if x <= a:
            return 1.0
        else:
            return 1 - gammaincc(alpha, z**beta)

counter = 0
def amoroso_binned_log_likelihood(parameters, bin_edges, bin_counts):
    '''Poisson fit for an Amoroso distribution with parameters (a, theta, alpha, beta).

    Parameters
    ----------
    parameters : array-like
        values of the Amoros parameters
    bin_edges : array-like
        edges of the binned samples. Assume it has (N+1) elements.
    bin_counts : array-like
        The number of samples in each bin (N elements).

    '''
    # print(parameters)
    a, theta, alpha, beta = parameters
    if alpha < 0:
        return np.inf
        # raise ValueError("Negative alpha: %g" % alpha)
    if beta < 0:
        return np.inf
        # raise ValueError("Negative beta: %g" % beta)

    N = bin_counts.sum()
    res = 0.0
    left_cum = amoroso_cdf(bin_edges[0], parameters)
    for i, (xi, ni) in enumerate(zip(bin_edges[1:], bin_counts)):
        # probability of ni events in bin i is N x Amoroso prob
        right_cum = amoroso_cdf(xi, parameters)
        # print("prob in bin ", i, ":", right_cum - left_cum)
        res += scipy.stats.poisson.logpmf(ni, N * (right_cum - left_cum))
        # if math.isnan(res):
        #     print(ni, N, left_cum, right_cum, a, xi)
        #     print(bin_edges)
        #     print(bin_counts)
        #     raise ValueError("Encountered NaN for parameters" + str(parameters) + " in step %d" % i \
        #                      + " at %g for a = %g with right cum = %g, logpmf = %g" % (xi, a, right_cum, scipy.stats.poisson.logpmf(ni, N * (right_cum - left_cum))))
        # remember result for next iteration
        left_cum = right_cum

    # print("result", res)
    # can arise if samples >= a. Then the probabibility is zero, or -inf on log scale
    if math.isnan(res):
        res = -np.inf

    # negate result for minimization
    res *= -1.0
    return res


def amoroso_binned_max_log_likelihood(samples, initial_guess=None):
    if initial_guess is None:
        initial_guess = np.array([1.005 * samples.max(), samples.std(), 1.1, 1])
        # for negative skew, use negative scale parameter
        if scipy.stats.skew(samples) < 0:
            initial_guess[1] *= -1

    # initial_guess = np.array([ 1.44631991, -0.02302599,  1.370993,    1.00922993])
    # initial_guess = np.array([ 1.44506214, -0.02157434,  1.28101393,  0.90385331])

    bounds = [(None, None),
              (0.0, None) if initial_guess[1] > 0 else (None, 0.0),
              (0, None),
              (0.0, None)
              ]

    # bin the data with Bayesian blocks
    # from astroML.plotting import hist
    # bin_counts, bin_edges, _ = hist(samples, bins='blocks')

    from matplotlib.pyplot import hist
    bin_counts, bin_edges, _ = hist(samples, bins=50)

    print()
    print("initial guess", initial_guess, "f", amoroso_binned_log_likelihood(initial_guess, bin_edges, bin_counts))

    # scipy.optimize
    # kwargs = dict(bounds=bounds, options=dict(disp=True, maxiter=100),
    #               method='Powell',
    #               args=(bin_edges, bin_counts))
    # return scipy.optimize.minimize(amoroso_binned_log_likelihood, initial_guess, **kwargs)

    # nlopt
    import nlopt

    # best results with LN_COBYLA, LN_SBPLX, GN_CRS2_LM
    # not good: LN_BOBYQA, LN_PRAXIS, GN_DIRECT_L, GN_ISRES, GN_ESCH
    # opt = nlopt.opt(nlopt.GN_CRS2_LM, 4)
    # opt = nlopt.opt(nlopt.LN_SBPLX, 4)
    opt = nlopt.opt(nlopt.LN_COBYLA, 4)
    opt.set_min_objective(lambda x, grad: amoroso_binned_log_likelihood(x, bin_edges, bin_counts))

    opt.set_lower_bounds([0.95 * bin_edges[0], 0.0 if initial_guess[1] > 0.0 else -20.0, 0.0, 0.0])
    opt.set_upper_bounds([1.05 * bin_edges[-1], 50.0 if initial_guess[1] > 0.0 else 0.0, 10, 10.0])

    tol = 1e-12
    opt.set_ftol_abs(tol)
    opt.set_xtol_rel(math.sqrt(tol))
    opt.set_maxeval(1500)

    xopt = opt.optimize(initial_guess)
    fmin = opt.last_optimum_value()

    print("Mode", repr(xopt), ", min. f =", fmin)


class TARDISBayesianLogLikelihood(Model):
    inputs = ('packet_nu', 'packet_energies')
    outputs = ('loglikelihood')

    def __init__(self, wavelength, flux):
        super(TARDISBayesianLogLikelihood, self).__init__()
        self.wavelength = wavelength
        self.flux = flux

    def evaluate(self, packet_nu, packet_energies):
        # sort by nu
        indices = packet_nu.argsort()
        # todo this creates copies!
        packet_nu = packet_nu[indices]
        packet_energies = packet_energies[indices]

        # log(likelihood)
        ll = 0

        # todo run in parallel?
        # divide into bins

        # todo run in parallel?
        # compute log likelihood for each bin
        for b in bins:
            ll += math.log(prob_L_given_theta(L, packet_nu, packet_energies, b[0], b[1]))

        return ll
