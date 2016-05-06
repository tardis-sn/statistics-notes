// ***************************************************************
// This file was created using the bat-project script.
// bat-project is part of Bayesian Analysis Toolkit (BAT).
// BAT can be downloaded from http://mpp.mpg.de/bat
// ***************************************************************

#ifndef __BAT__TARDIS__H
#define __BAT__TARDIS__H

#include <BAT/BCModel.h>

#include <string>

// ---------------------------------------------------------
class Tardis : public BCModel
{

public:
    /* using Vec = std::valarray<double>; */
    using Vec = std::vector<double>;

    // Constructor
    Tardis(const std::string& name, const std::string& fileName, unsigned run = 9, unsigned maxElements = 0);

    // Destructor
    ~Tardis();

    // Overload LogLikelihood to implement model
    virtual double LogLikelihood(const std::vector<double>& parameters);

    // Overload LogAprioriProbability if not using built-in 1D priors
    virtual double LogAPrioriProbability(const std::vector<double> & parameters);

    virtual void CalculateObservables(const std::vector<double>& parameters);

    /**
     * Multiply the likelihood by this factor to avoid overflows.
     *
     * @param scale factor on the log(!) scale
     */
    void rescale(double scale)
    { this->scale = scale; }

    /**
     * Set state to update the prior such that minuit gets the correct
     * target density for prediction and compute the evidence.
     */
    void PreparePrediction();

    /**
     * Predict X for small n at a given nu (=bin center).
     *
     * Algorithm: Start computing for N=n, then go to n-1,
     * n+1. Continue until contributions are negligible. Bear in mind
     * N>0! Stay on log scale as much as possible?
     *
     * @param Xmean If positive, use floor(X / Xmean) as starting guess for most likely N
     * @param precision Stop searching if contribution from current point is less than `precision * currentValue`
     */
    double PredictSmall(unsigned n, double X, double nu, double Xmean = -1, double precision = 1e-2);

    double PredictMedium(unsigned n, double X, double nu);

    double PredictVeryLarge(unsigned n, double X, double nu);

    /**
     * Compute sum of X in frequency bin
     */
    std::tuple<unsigned, double> SumX(double numin, double numax) const;

    /**
     * Polynomial as a function of nu with coefficients given in range
     */
    template <typename T>
    static double Polyn(T first, T last, const double& nu)
    {
        double res = 0.;
        double power = 1.;
        for (; first != last; ++first) {
            res += (*first) * power;
            power *= nu;
        }
        return res;
    }

    double alphaNu(const Vec& x, const double& nu)
    {
        return Polyn(std::begin(x), std::begin(x) + orderAlpha, nu);
    }

    double betaNu(const Vec& x, const double& nu)
    {
        return Polyn(std::begin(x) + orderAlpha, std::begin(x) + orderAlpha + orderBeta, nu);
    }

    size_t Nsamples() const
    {
        return samples.size();
    }

private:
    enum class Target { Default, Gamma, NBGamma };

    Vec ReadData(const std::string& fileName, const std::string& dataSet, unsigned run, unsigned maxElements = 0);

    /**
     * @param init use as initial position, write back updated results from minimization
     * @return for N posited events and n observed events, check if the contribution to res = sum_N P(X|N) is negligible with latest/ res < precision
     */
    bool SearchStep(unsigned N, unsigned n, double& res, double precision, Vec& init);

   /**
     * Minimum of polynomial given by coefficients in range. The argument is assumed to lie in [0,1].
     */
    static double MinPolyn(Vec::const_iterator begin, Vec::const_iterator end);

    struct Point
    {
         double en, nu;
    };

    void FixPredicted(Target target, unsigned n, double X, double nu)
    {
        this->target = target;
        nPrediction = n;
        XPrediction = X;
        nuPrediction = nu;
    }

    void Unfix()
    {
        target = Target::Default;
        nPrediction = 0;
        XPrediction = -1;
        nuPrediction = -1;
    }

    std::vector<Point> samples;

    unsigned orderAlpha, orderBeta;
    const unsigned npoints;
    const double nuMax, alphaMin, betaMin;
    double scale;
    double evidence;

    // parameter of prior on Poisson parameter
    double a;

    ///> prediction
    Target target;
    double nuPrediction, XPrediction;
    unsigned nPrediction, NPrediction;
};
// ---------------------------------------------------------

#endif
