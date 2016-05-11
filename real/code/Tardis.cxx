// ***************************************************************
// This file was created using the bat-project script.
// bat-project is part of Bayesian Analysis Toolkit (BAT).
// BAT can be downloaded from http://mpp.mpg.de/bat
// ***************************************************************

#include "Tardis.h"

#include <BAT/BCMath.h>

#include <TH1.h>
#include <TCanvas.h>

#include <H5Cpp.h>

#include <gsl/gsl_poly.h>
#include <gsl/gsl_multimin.h>
#include <gsl/gsl_sf_psi.h>

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>

using std::cout;
using std::cerr;
using std::endl;
using namespace H5;

// #define USE_TEST_QUADRATIC 1

namespace
{

inline double logGamma(double X, double alpha, double beta)
{
    return alpha * log(beta) - lgamma(alpha) + (alpha - 1) * log(X) - beta * X;
}

inline double logNegativeBinomial(const unsigned N, const unsigned n, const double a)
{
    const double tmp = N + n - a + 1;
    return lgamma(tmp) - lgamma(N + 1) - lgamma(n - a + 1) - tmp * std::log(2);
}

// mathematica output
#define Power(a, b) std::pow(a, b)
#define Sqrt(x) std::sqrt(x)

/*
 * All-very-large approximation
 */
double logF(double X, double N, unsigned n, double r, double mean, double var)
{
    const double diffn = N - n;
    const double diffX = X - N * mean;
    return -log(2 * M_PI) - 0.5 * log(2 * r * N * var)
    - 0.5 * diffn * diffn / ( 2 * r) - 0.5 * diffX * diffX / (N * var);
}
double solveGradient(double X, unsigned n, double r, double mean, double var)
{
    /* solve cubic polynomial */
    // transform to standard form with unit leading coefficient
    const double a = r * (mean * mean / var - 1);
    const double c = - r * X * X /var ;

    std::array<double, 3> roots;
    int nRealRoots = gsl_poly_solve_cubic(a, r, c, &roots[0], &roots[1], &roots[2]);
    int nPositiveRealRoots = 0;

    std::string msg = Form(" for X=%g, r=%g, mean=%g, var=%g:\n"
            "%g, %g, %g", X, r, mean, var, roots[0], roots[1], roots[2]);

    if (nRealRoots == 1) {
        if (roots[0] <= 0)
            throw std::runtime_error(std::string("Only found negative root") + msg);
        return roots[0];
    }

    auto N = -std::numeric_limits<double>::infinity();
    double maxf = N;

    for (auto& r : roots) {
        // find positive root
        if (r > 0) {
            ++nPositiveRealRoots;
            double tmp = logF(X, N, n, r, mean, var);
            cerr << r << " " << tmp << endl;
            if (std::isnan(tmp))
                continue;
            if (tmp > maxf) {
                maxf = tmp;
                N = r;
            }
        }
    }
    if (nPositiveRealRoots == 0)
        throw std::runtime_error(std::string("Found no positive root with ") + msg);
    if (N < 0)
        cerr << "Found no valid root, return -inf" << msg << endl;

    if (nPositiveRealRoots > 1)
        cerr << "Found multiple positive roots for" << msg << endl;

    return N;
}
double hessian(double X, double N, double r, double var)
{
    return (-Power(N,-2) + 1/r + (2*Power(X,2))/(Power(N,3)*var))/2.;
}

/** 1D */
double logLaplace(double logf, double hessianDeterminant)
{
    return logf + 0.5 * log(2 * M_PI / hessianDeterminant);
}


Tardis& tardis(void* m)
{
    return *static_cast<Tardis*>(m);
}

inline void update_alpha_gradient(const double alpha, const double beta,
        const double x, const double nu, const double N,
        gsl_vector* vec, const Tardis& m)
{
//    double tmp = (log(beta) - gsl_sf_psi(N * alpha) + log(x)) * N;
    double tmp = log(beta);
    tmp -= gsl_sf_psi(N * alpha);
    tmp += log(x);
    tmp *= N;
    for (auto i = 0u; i < m.orderAlpha; ++i) {
        vec->data[i] += tmp;
        tmp *= nu;
    }
}

inline void update_beta_gradient(const double alpha, const double beta,
        const double x, const double nu, const double N,
        gsl_vector* vec, const Tardis& m)
{
    double tmp = N * alpha / beta - x;
    for (auto i = 0u; i < m.orderBeta; ++i) {
        vec->data[m.orderAlpha + i] += tmp;
        tmp *= nu;
    }
}

/*
 * free standing functions for gsl
 */
#ifdef USE_TEST_QUADRATIC
void test_quadratic_and_gradient(const gsl_vector * v, void *,
        double *f, gsl_vector *df)
{
    *f = 0;
    for (auto i = 0u; i < v->size; ++i) {
        const double&  x = v->data[i];
        *f +=  x * x;
        df->data[i] = 2.0 * x;
    }
    cout << "quadratic " << *f << " at ";
    std::copy(v->data, v->data + v->size, std::ostream_iterator<double>(cout, " "));
    cout << endl;
}
double test_quadratic(const gsl_vector *v, void *)
{
    cout << "quadratic called\n";
    // gradient vector
    auto df = gsl_vector_alloc(v->size);
    double res;
    test_quadratic_and_gradient(v, nullptr, &res, df);

    gsl_vector_free(df);
    return res;
}

void
test_quadratic_gradient(const gsl_vector *v, void *, gsl_vector *df)
{
    cout << "quadratic gradient called\n";
    double res;
    test_quadratic_and_gradient(v, nullptr, &res, df);
}
#endif
void log_likelihood_and_gradient(const gsl_vector * v, void *model,
        double *f, gsl_vector *df)
{
    Tardis& m = tardis(model);

    *f = 0;
    gsl_vector_set_zero(df);

    const auto invalid = -std::numeric_limits<double>::infinity();
    double res = 0;
#pragma omp parallel for reduction(+:res) schedule(static)
    for (unsigned n = 0; n < m.samples.size(); ++n) {
        const auto& s = m.samples[n];

        // alpha(lambda_j)
        const double alpha_n = m.alphaNu_p(v->data, s.nu);
        const double beta_n = m.betaNu_p(v->data, s.nu);

        // use samples to check parameter space for a value inconsistent with prior
        if (alpha_n <= m.alphaMin || beta_n <= m.betaMin)
            res = invalid;

        // cout << "alpha_n " << alpha_n << ", beta_n "<< beta_n << endl;
        const double extra = ::logGamma(s.en, alpha_n, beta_n);
        // const double extra = ::logGamma(1 - s.en / maxEn, alpha_n, beta_n);
        if (!std::isfinite(extra))
            res = invalid;

        // results are different from run to run with more than 2 threads
        // addition not commutative with floating point
        // change by 50% possible on linear scale if scale added only once
        // => apply part of scale each time to retain precision
        res += extra;

        /* gradient (no update of N component) */
        update_alpha_gradient(alpha_n, beta_n, s.en, s.nu, 1.0, df, m);
        update_beta_gradient(alpha_n, beta_n, s.en, s.nu, 1.0, df, m);
    }

    if (m.target == Tardis::Target::Default)
        goto end;

    // need a scope to initialize variables with goto above
    {
        const auto alpha = m.alphaNu_p(v->data, m.nuPrediction);
        const auto beta = m.betaNu_p(v->data, m.nuPrediction);
        const auto N = gsl_vector_get(v, m.orderN());
        const auto& X = m.XPrediction;
        const auto& nu = m.nuPrediction;
        const auto& n = m.nPrediction;
        const auto& a = m.a;

        /* P(X | N alpha, beta) */

        /* function */
        res += logGamma(X, N * alpha, beta);

        /* gradient */

        update_alpha_gradient(alpha, beta, X, nu, N, df, m);
        update_beta_gradient(alpha, beta, X, nu, N, df, m);

        // N almost the same as alpha_0
        df->data[m.orderN()] += (log(beta) - gsl_sf_psi(N * alpha) + log(X)) * alpha;

        if (m.target == Tardis::Target::Gamma)
            goto end;


        /* P(N | n) */

        /* function */
        res += logNegativeBinomial(N, n, a);

        /* gradient (only N) */
        df->data[m.orderN()] += gsl_sf_psi(N + n -a + 1) - gsl_sf_psi(N + 1) - log(2);

        if (m.target == Tardis::Target::NBGamma)
            goto end;
    }

    end:
    // to minimize, flip definition of function
    *f = -res;
    if (std::isfinite(*f))
        gsl_vector_scale(df, -1);
    else
        gsl_vector_set_all(df, *f);
#if 0
    cout.precision(15);
    cout << "-llh = " << *f;
    cout << " at ( ";
    for (auto i = 0; i < m.GetNParameters(); ++i)
        cout << gsl_vector_get(v, i) << " ";
    cout << ")" << endl;
    cout << "gradient: ";
    std::copy(df->data, df->data + df->size, std::ostream_iterator<double>(cout, " "));
    cout << endl;
#endif
}

double log_likelihood(const gsl_vector *v, void *model)
{
    // gradient vector
    auto df = gsl_vector_alloc(v->size);
    double res;
    log_likelihood_and_gradient(v, model, &res, df);

    gsl_vector_free(df);
    return res;
}

void
log_likelihood_gradient(const gsl_vector *v, void *model, gsl_vector *df)
{
    double res;
    log_likelihood_and_gradient(v, model, &res, df);
}

}
#if 0
template <>
double Tardis::alphaNu(const double*& x, const double& nu)
{
    return Polyn(&x, &x + orderAlpha, nu);
}
#endif
// ---------------------------------------------------------
Tardis::Tardis(const std::string& name, const std::string& fileName,
               unsigned run, unsigned maxElements)
: BCModel(name),
  npoints(10),
  nuMax(0.5),
  alphaMin(1.0),
  betaMin(0.0),
  evidence(0.0),
  a(0.5),
  target(Target::Default),
  nuPrediction(-1),
  XPrediction(-1),
  NPrediction(0)
{
    AddParameter("alpha0",  1, 2, "#alpha_{0}");
//    AddParameter("alpha1", -3, 3, "#alpha_{1}");    // GetParameter(1).Fix(0);
#if 1
//    AddParameter("alpha2", -10, 10, "#alpha_{2}");  // GetParameter(2).Fix(0);
#else
    AddParameter("alpha2", -30, 30, "#alpha_{2}");  // GetParameter(2).Fix(0);
    AddParameter("alpha3", -50, 50, "#alpha_{3}");  // GetParameter(3).Fix(0);
#endif
//    AddParameter("alpha4", -30, 30, "#alpha_{4}"); // GetParameter(3).Fix(0);

    orderAlpha = GetNParameters();

    AddParameter("beta0",  0, 100, "#beta_{0}");
//    AddParameter("beta1", -200, 50, "#beta_{1}");  // GetParameter(order + 1).Fix(0);
//    AddParameter("beta2",  0, 250, "#beta_{2}"); // GetParameter(order + 2).Fix(0);
    // AddParameter("beta3",  -300, 300, "#beta_{3}"); // GetParameter(order + 3).Fix(0);

    orderBeta = GetNParameters() - orderAlpha;

#if 0
    for (unsigned i = 0; i <= npoints; ++i) {
        double nu =  double(i) * nuMax / npoints;
        AddObservable(Form("alpha(%g)",nu), 1, 2,   Form("#alpha(%g)", nu));
    }
    for (unsigned i = 0; i <= npoints; ++i) {
        double nu =  double(i) * nuMax / npoints;
        AddObservable(Form("beta(%g)", nu), 0, 100, Form("#beta(%g)", nu));
    }

    AddObservable("Gamma(0.01|nu=0.2)", 25, 40);
#endif
    Vec energies = ReadData(fileName, "energies", run, maxElements);
    Vec nus = ReadData(fileName, "nus", run, maxElements);
    assert(energies.size() == nus.size());

    // remove negative energies
    samples.reserve(energies.size());
    for (unsigned i = 0; i < energies.size(); ++i) {
        if (energies[i] > 0) {
            samples.push_back(Point{energies[i], nus[i]});
        }
    }
    samples.shrink_to_fit();

    cout << "Parsed " << nus.size() << " elements from " << fileName
         << ", retain " << samples.size() << " positive-energy elements\n";

    // sort by frequency
    std::sort(samples.begin(), samples.end(),
              [](const Tardis::Point& s1, const Tardis::Point& s2)
              { return s1.nu < s2.nu; } );

    // rescale and flip the energies
    // rescale frequencies

    auto enOrder =
            [](const Tardis::Point& s1, const Tardis::Point& s2)
            { return s1.en < s2.en; };

    auto maxElem = std::max_element(samples.begin(), samples.end(), enOrder);

    const auto energyScale =  (1 + 1e-6) * maxElem->en; //1e38;
    const auto frequencyScale = 1e15;

    const double maxNu = samples.back().nu;
    cout << "Before data transformation" << endl;
    cout << "Max. energy = " << maxElem->en << endl;
    cout << "Max. frequency = " << maxNu << endl;

    for (auto& s : samples) {
        // rescale data
        // s.en /= energyScale;
        s.en = 1.0 - s.en / energyScale;
        // very subtle but important: need a constant independent of
        // the data here to avoid bias: some simulations have outliers
        // with very large frequency
        s.nu /= frequencyScale;
    }

    // frequency sorting maintained but energy flipped => search again
    cout << "After data transformation with frequency scale " << frequencyScale
            << " and energy scale " << energyScale << endl;
    maxElem = std::max_element(samples.begin(), samples.end(), enOrder);
    cout << "Max. energy = " << maxElem->en << endl;
    cout << "Max. frequency = " << samples.back().nu << endl;

    // set index to the parameter itself and not beyond by one
    // so param[orderEnmax] works as expected
    // AddParameter ("maxEn", (1 + 1e-10) * maxElem->en, 1.01 * maxElem->en, "E_{max}");
    // orderEnmax = GetNParameters() - 1;

    // GetParameter(orderEnmax).Fix((1 + 1e-6) * maxElem->en);

#if 0
    // plot data
    // create new histogram
    TH1D hist("data", ";x;N", 2000, 0.0, 1);
    // hist.SetStats(kFALSE);
    for (auto& s : samples)
        hist.Fill(s.nu);

    for (int i = 1; i <= hist.GetNbinsX(); ++i)
        cout << "bin " << i << ": [" << hist.GetBinLowEdge(i)
             << ", " << hist.GetBinLowEdge(i+1)
             << "], value = " << hist.GetBinContent(i)
             << endl;

    TCanvas c;
    hist.Draw();
    c.Print("samples.pdf");
#endif
}

// ---------------------------------------------------------
Tardis::~Tardis()
{
}

double Tardis::MinPolyn(Vec::const_iterator begin, Vec::const_iterator end)
{
    const int npar = std::distance(begin, end);
    double a0, a1, a2;
    switch (npar) {
    case 1: {
        return *begin;
    }
    case 2: {
        a0 = *begin;
        a1 = *(++begin);
        if (a1>= 0)
            return a0;
        else
            return a0 + a1;
    }
    case 3: {
        a0 = *begin;
        a1 = *(++begin);
        a2 = *(++begin);
        if ((a2 == 0 && a1 == 0) || (a2 == 0 && a1 > 0) || (a2 > 0 && a1 >= 0) || (a2 < 0 && a1 > -a2))
            return a0;
        if (a2 == 0 && a1 < 0)
            return a0 + a1;
        if ((a2 > 0 && a1 <= -2 * a2) || (a2 < 0 && a1 <= -a2))
            return a0 + a1 + a2;
        return (-a1 * a1 + 4 * a0 * a2)/ (4 * a2);
    }
    default:
        // TODO implement this
        // throw std::logic_error("Cubic and higher order not implemented");
        return 3;
    }
}

Tardis::Vec Tardis::ReadData(const std::string& fileName, const std::string& dataSet,
                             unsigned run, unsigned maxElements)
{
    // identify the run of tardis = row in column
//    static const hsize_t run = 9;

    H5File file(fileName.c_str(), H5F_ACC_RDONLY );
    DataSet dataset = file.openDataSet(dataSet);
    /*
     * Get dataspace of the dataset.
     */
    DataSpace dataspace = dataset.getSpace();

    /*
     * Get the dimension size of each dimension in the dataspace and
     * display them.
     */
    hsize_t dims_out[2];
    dataspace.getSimpleExtentDims( dims_out, NULL);
//    cout << "rank " << ndims << ", dimensions " <<
//        (unsigned long)(dims_out[0]) << " x " <<
//        (unsigned long)(dims_out[1]) << endl;

    // read one row from large matrix into 1D array
    static const int rankOut = 1;
    hsize_t N = dims_out[1];
    if (maxElements > 0)
        N = std::min(N, hsize_t(maxElements));

    std::array<hsize_t, 2> offsetIn = {{ run, 0 }};
    std::array<hsize_t, 2> countIn = {{1, N}};
    dataspace.selectHyperslab(H5S_SELECT_SET, &countIn[0], &offsetIn[0]);

    // reserve memory to hold the data, resize so vector know how many
    // elements it has because hdf5 just writes directly to the heap buffer
    std::vector<double> buffer(N);
    // buffer.reserve(N);
    // buffer.resize(N);

    // define memory buffer layout
    DataSpace memspace(rankOut, &N);

    // where to write into the buffer: start at beginning and write up to the end
    hsize_t offset = 0;
    hsize_t count = N;
    memspace.selectHyperslab(H5S_SELECT_SET, &count, &offset);

    // read and possibly convert the data
    dataset.read(&buffer[0], PredType::NATIVE_DOUBLE, memspace, dataspace);

    return buffer;
}

void Tardis::PreparePrediction()
{
    Tardis::Vec v(GetNParameters(), 0.0);
    v[0] = 1.5;
    v[orderAlpha] = 60;
    // v.at(orderEnmax) = GetParameter(orderEnmax).GetLowerLimit();

    FindMode(v);

    // change normalization to avoid overflow
//    rescale(-LogEval(GetBestFitParameters()));
    // running minuit again might after redefining the
    // target density
    FindMode(GetBestFitParameters());
    evidence = Integrate(BCIntegrate::kIntLaplace);
}

bool Tardis::SearchStep(unsigned N, unsigned n, double& res, double precision, std::vector<double>& init)
{
    // N>0 required to search
    if (N == 0)
        return false;

    NPrediction = N;

    // run minuit manually assuming that current position and step
    // size are good gives a factor 2-3 in speed
    //    FindMode(GetBestFitParameters());
    auto& min = GetMinuit();
    assert(min.NDim() == init.size());
    assert(GetNParameters() == init.size());
    // set values manually even though SetVariableValues() exist
    // but that produces undesired warnings
    for (unsigned i = 0; i < init.size(); ++i)
        min.SetVariableValue(i, init[i]);
    //    min.SetVariableValues(&init[0]);

    min.Minimize();

    // copy result back
    std::copy(min.X(), min.X() + GetNParameters(), init.begin());

    const double latest = std::exp(logNegativeBinomial(N, n, a)) * Integrate(BCIntegrate::kIntLaplace);
    res += latest;

    cout << "total = " << res << ", P(" << XPrediction << "|" << N
    << ") = " << latest
    << endl;

    return (latest / res) > precision;
}

double Tardis::PredictSmall(unsigned n, double X, double nu, double Xmean, double precision)
{
    FixPredicted(Target::Gamma, n, X, nu);

    // start N at mode of NegativeBinomial if invalid Xmean given
    double guess = floor(n - a + 1);
    if (Xmean > 0)
        guess = X / Xmean;
//    double guess = X /

    unsigned N = std::max(1.0, guess);
    NPrediction = N;

    double res = 0;

    // index difference down/up
    unsigned Nup = N, Ndown = N;

    std::vector<double> initUp(GetBestFitParameters());
    auto initDown = initUp;
    SearchStep(N, n, res, precision, initUp);

    // continue searching up or down
    bool goUp = true, goDown = true;

    // now move up or down
    while (goUp || goDown) {
        if (goUp)
            goUp = SearchStep(++Nup, n, res, precision, initUp);

        if (goDown)
            goDown = SearchStep(--Ndown, n,res, precision, initDown);
    }

    nuPrediction = -1;

    unsigned totalN = Nup - Ndown;
    if (Ndown == 0)
        --totalN;
    cout << "Total number of calls " << totalN << endl;

    Unfix();

    return res / evidence;
}

double Tardis::PredictMedium(unsigned n, double X, double nu)
{
    try {
        GetParameter("N");
    } catch (std::out_of_range& e) {
        // assume optimization over alpha and beta has been done
        // then add N as a new parameter, optimize again
        Vec oldMode = GetBestFitParameters();
        cout << "Adding N with start value " << n << endl;
        AddParameter("N", 1, 3 * n);
        oldMode.push_back(n);
        FindMode(oldMode);
        evidence = Integrate(BCIntegrate::kIntLaplace);
    }

    FixPredicted(Target::NBGamma, n, X, nu);

    auto& min = GetMinuit();
    min.Minimize();

    const double res = Integrate(BCIntegrate::kIntLaplace) / evidence;
    cout << "Medium res for X = " << X << " = " << res << endl;

//    Unfix();

    return res;
}

double Tardis::PredictVeryLarge(unsigned n, double X, double nu)
{
    const double r = n - a + 1;

    const auto & mode = GetBestFitParameters();
    const double alpha = alphaNu(mode, nu);
    const double beta = betaNu(mode, nu);

    const double mean = alpha / beta;
    const double var = mean / beta;
    const auto N = solveGradient(X, n, r, mean, var);

    const double logF = ::logF(X, N, n, r, mean, var);
    const double hessianDeterminant = ::hessian(X, N, r, var);
    const double res = ::logLaplace(logF, hessianDeterminant);

    if (std::isnan(res))
        return 0.0;

    return std::exp(res);
}

std::tuple<unsigned, double> Tardis::SumX(double numin, double numax) const
{
    // find first element in bin
    auto min = std::lower_bound(samples.begin(), samples.end(), numin,
                                [](const Tardis::Point& s1, double value)
                                { return s1.nu < value; } );
    // first element outside of bin
    auto max = std::upper_bound(min, samples.end(), numax,
                                [](double value, const Tardis::Point& s1)
                                { return value < s1.nu; } );
    // sum energies in bin
    const double X = std::accumulate(min, max, 0.0,
        [](const double & res, const Tardis::Point& s2)
        { return res + s2.en; });

    unsigned n = std::distance(min, max);
    cout << "Found " << n
    << " elements in bin [" << numin << ", " << numax
    << "] with X = " << X << endl;

    return std::tuple<unsigned, double> {n, X};
}

void Tardis::minimize_gsl()
{
    int iter = 0;
    int status;
    const unsigned ndim = GetNParameters();
    cout << "npar " << GetNParameters() << endl;

    const gsl_multimin_fdfminimizer_type *T;
    gsl_multimin_fdfminimizer *s;

    gsl_multimin_function_fdf my_func;

    my_func.n = ndim;
#ifdef USE_TEST_QUADRATIC
    my_func.f = test_quadratic;
    my_func.df = test_quadratic_gradient;
    my_func.fdf = test_quadratic_and_gradient;
#else
    my_func.f = log_likelihood;
    my_func.df = log_likelihood_gradient;
    my_func.fdf = log_likelihood_and_gradient;
#endif
    my_func.params = this;

    /* Starting point */
    gsl_vector *v = gsl_vector_alloc(ndim);
    gsl_vector_set_zero(v);

    switch (GetNParameters()) {
    case 2:
        gsl_vector_set(v, 0,  1.450339e+00);
        gsl_vector_set(v, 1,  6.17191e+01);
        break;
    case 5:
        gsl_vector_set(v, 0,  1.30339e+00);
        gsl_vector_set(v, 1, 0);
        gsl_vector_set(v, 2, 0);
        gsl_vector_set(v, 3,  6.597191e+01);
        gsl_vector_set(v, 4, -1.151438e+01);
        break;
    case 6:
        gsl_vector_set(v, 0,  1.60339e+00);
        gsl_vector_set(v, 1, -1.73325e-01);
        gsl_vector_set(v, 2, -4.89087e-02);
        gsl_vector_set(v, 3,  4.73753e-02);
        gsl_vector_set(v, 4,  7.597191e+01);
        gsl_vector_set(v, 5, -1.151438e+01);
        break;
    default:
        cout << " Don't know how to initialize GSL." << endl;
        return;
        break;
    }

    // T = gsl_multimin_fdfminimizer_vector_bfgs2;
   T = gsl_multimin_fdfminimizer_conjugate_fr;
    s = gsl_multimin_fdfminimizer_alloc (T, ndim);
    cout << "npar " << GetNParameters()
         << " ndim " << ndim
         << " v->size " << v->size <<  endl;
    cout << "Initializing minimizer with ";
    std::copy(v->data, v->data + ndim, std::ostream_iterator<double>(cout, " "));
    cout << endl;

    gsl_multimin_fdfminimizer_set(s, &my_func, v, 0.1, 1e-3);
    cout.precision(10);
    double fprevious = std::numeric_limits<double>::infinity();
    bool done = false;
    do
    {
        ++iter;
        status = gsl_multimin_fdfminimizer_iterate (s);

        if (status) {
            cout << "what(): " << gsl_strerror(status) << endl;
            break;
        }

//        status = gsl_multimin_test_gradient (s->gradient, 1e-3);
        if (std::isfinite(s->f)) {
            if (std::abs((s->f - fprevious) / s->f) < 1e-7) {
                cout << "Converged\n";
                done = true;
            }
        }

        cout << iter << " " << s->f << " ";
        for (unsigned i = 0; i < ndim; ++i) {
            cout << gsl_vector_get(s->x, i) << " ";
        }
        cout << endl;
        cout << "Gradient: ";
        for (unsigned i = 0; i < ndim; ++i) {
                    cout << gsl_vector_get(s->gradient, i) << " ";
                }
        cout << endl;

        // save result for comparison in next iteration
        fprevious = s->f;
    }
    while (!done && iter < 3000);

    gsl_multimin_fdfminimizer_free (s);
    gsl_vector_free (v);
}