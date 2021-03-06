#include "MACE.h"
#include "MOO.h"
#include "util.h"
#include "MVMO.h"
#include "NLopt_wrapper.h"
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <gsl/gsl_qrng.h>
#include <omp.h>
#include <chrono>
#include <set>
#include <fstream>
#include <iomanip>
using namespace std;
using namespace std::chrono;
using namespace Eigen;
MACE::MACE(Obj f, size_t num_spec, const VectorXd& lb, const VectorXd& ub, string log_name)
    : _func(f),
      _lb(lb),
      _ub(ub),
      _scaled_lb(-25), 
      _scaled_ub(25), 
      _a((_ub - _lb) / (_scaled_ub - _scaled_lb)), 
      _b(0.5 * (_ub + _lb)), 
      _log_name(log_name), 
      _num_spec(num_spec), 
      _dim(lb.size()),
      _max_eval(500),
      _tol_no_improvement(10),
      _eval_fixed(_max_eval), 
      _gp(nullptr),
      _eval_counter(0), 
      _have_feas(false), 
      _best_x(VectorXd::Constant(_dim, 1, INF)), 
      _best_y(RowVectorXd::Constant(1, _num_spec, INF)), 
      _eval_x(MatrixXd(_dim, 0)), 
      _eval_y(MatrixXd(_num_spec, 0))
{
    assert(_scaled_lb   < _scaled_ub);
    assert((_lb.array() < _ub.array()).all());
    if(_num_spec > 1)
    {
        cerr << "Num spec > 1, currently I only handle unconstrained problems" << endl;
        exit(EXIT_FAILURE);
    }
    _init_boost_log();
    BOOST_LOG_TRIVIAL(info) << "MACE Created";
}
MatrixXd MACE::_run_func(const MatrixXd& xs)
{
    bool no_improve = true;
    const auto t1            = chrono::high_resolution_clock::now();
    const size_t num_pnts = xs.cols();
    const MatrixXd scaled_xs = _rescale(xs);
    MatrixXd ys(_num_spec, num_pnts);
    BOOST_LOG_TRIVIAL(info) << "X:\n"        << _rescale(xs).transpose();
#pragma omp parallel for
    for(size_t i = 0; i < num_pnts; ++i)
    {
        ys.col(i) = _func(scaled_xs.col(i));
    }

    for(size_t i = 0; i < num_pnts; ++i)
    {
        if(_better(ys.col(i), _best_y))
        {
            _best_x    = scaled_xs.col(i);
            _best_y    = ys.col(i);
            no_improve = false;
        }
        if(_is_feas(ys.col(i)))
            _have_feas = true;
    }
    if(no_improve)
        ++_no_improve_counter;
    else
        _no_improve_counter = 0;
    const auto t2       = chrono::high_resolution_clock::now();
    const double t_eval = static_cast<double>(chrono::duration_cast<milliseconds>(t2 -t1).count()) / 1000.0;
    BOOST_LOG_TRIVIAL(info) << "Time for " << num_pnts << " evaluations: " << t_eval << " sec";
    _eval_counter += num_pnts;
    return ys;
}
MACE::~MACE()
{
    delete _gp;
}
void MACE::_init_boost_log() const
{
    boost::log::add_common_attributes();
#ifndef MYDEBUG
    boost::log::add_file_log(boost::log::keywords::file_name = _log_name, boost::log::keywords::auto_flush = true);
    boost::log::core::get()->set_filter
    (
        boost::log::trivial::severity >= boost::log::trivial::info
    );
#else
    boost::log::add_file_log(
            boost::log::keywords::file_name  = _log_name,
            boost::log::keywords::auto_flush = true
            // boost::log::keywords::format     = "[%TimeStamp%]:\n%Message%"
    );
#endif
}
bool MACE::_is_feas(const VectorXd& v) const
{
    bool feas = true;
    if(v.size() > 1)
        feas = (v.tail(v.size() - 1).array() <= 0).all();
    return feas;
}
bool MACE::_better(const VectorXd& v1, const VectorXd& v2) const
{
    if(_is_feas(v1) and _is_feas(v2))
        return v1(0) < v2(0);
    else if(_is_feas(v1))
        return true;
    else if(_is_feas(v2))
        return false;
    else
        return _violation(v1) < _violation(v2);
}
double MACE::_violation(const VectorXd& xs)  const
{
    return xs.size() == 1 ? 0 : xs.tail(xs.size() - 1).cwiseMax(0).sum();
}

// convert from [_scaled_lb, _scaled_ub] to [_lb, _ub]
MatrixXd MACE::_rescale(const MatrixXd& xs) const noexcept
{
    return _a.replicate(1, xs.cols()).cwiseProduct(xs).colwise() + _b;
}

// convert from [lb, ub] to [_scaled_lb, _scaled_ub]
MatrixXd MACE::_unscale(const MatrixXd& xs) const noexcept
{
    return (xs.colwise() - _b).cwiseQuotient(_a.replicate(1, xs.cols()));
}
void MACE::initialize(string xfile, string yfile)
{
    const MatrixXd dbx = read_matrix(xfile);
    const MatrixXd dby = read_matrix(yfile);
    initialize(dbx, dby);
}
void MACE::initialize(const MatrixXd& dbx, const MatrixXd& dby)
{
    // User can provide data for initializing
    if(_gp != nullptr)
    {
        BOOST_LOG_TRIVIAL(error) << "GP is already created!" << endl;
        exit(EXIT_FAILURE);
    }
    MYASSERT(static_cast<size_t>(dbx.rows()) == _dim);
    MYASSERT(static_cast<size_t>(dby.rows()) == _num_spec);
    MYASSERT(dbx.cols() == dby.cols());
    MYASSERT((dbx.array() <= _ub.replicate(1, dbx.cols()).array()).all());
    MYASSERT((dbx.array() >= _lb.replicate(1, dbx.cols()).array()).all());

    MatrixXd scaled_dbx = _unscale(dbx); // scaled from [lb, ub] to [_scaled_lb, _scaled_ub]
    if (dbx.cols() < 2)
    {
        BOOST_LOG_TRIVIAL(error) << "Size of initial sampling is less than 2" << endl;
        exit(EXIT_FAILURE);
    }
    if(! dby.allFinite())
    {
        BOOST_LOG_TRIVIAL(error) << "There are INF|NAN values in dby" << endl;
        exit(EXIT_FAILURE);
    }
    const size_t best_id = _find_best(dby);
    _best_x              = dbx.col(best_id);
    _best_y              = dby.col(best_id);
    _have_feas           = _is_feas(_best_y);
    _gp                  = new GP(scaled_dbx, dby.transpose());
    _no_improve_counter  = 0;
    _hyps                = _gp->get_default_hyps();
    _gp->set_noise_free(_noise_free);
    if(not _noise_free)
        _gp->set_noise_lower_bound(_noise_lvl);
    BOOST_LOG_TRIVIAL(info) << "Initial DBX:\n" << dbx << endl;
    BOOST_LOG_TRIVIAL(info) << "Initial DBY:\n" << dby << endl;
}
void MACE::initialize(size_t init_size)
{
    // Initialization by random simulation
    const MatrixXd dbx = _doe(init_size);
    const MatrixXd dby = _run_func(dbx);
    initialize(_rescale(dbx), dby);
}
size_t MACE::_find_best(const MatrixXd& dby) const
{
    vector<size_t> idxs = _seq_idx(dby.cols());
    sort(idxs.begin(), idxs.end(), [&](const size_t i1, size_t i2)->bool{
        return _better(dby.col(i1), dby.col(i2));
    });
    return idxs[0];
}
vector<size_t> MACE::_seq_idx(size_t n) const
{
    vector<size_t> idxs(n);
    for(size_t i = 0; i < n; ++i)
        idxs[i] = i;
    return idxs;
}
MatrixXd MACE::_set_random(size_t num) 
{
    return rand_matrix(num, VectorXd::Constant(_dim, 1, _scaled_lb), VectorXd::Constant(_dim, 1, _scaled_ub), _engine);
}
MatrixXd MACE::_doe(size_t num)
{
    MatrixXd sampled(_dim, num);
    
    // DoE to generate points in [0, 1]
    if(_dim <= 40 and _use_sobol)
    {
        // According to the doc of GSL library, the sobol method only works for dimensions less than 40
        gsl_qrng* q = gsl_qrng_alloc(gsl_qrng_sobol, _dim);
        double* vec = new double[_dim];
        for(size_t i = 0; i < num; ++i)
        {
            gsl_qrng_get(q, vec);
            sampled.col(i) = Eigen::Map<VectorXd>(vec, _dim);
        }
        delete[] vec;
        gsl_qrng_free(q);
    }
    else
    {
        sampled.setRandom();
        sampled = 0.5 * (sampled.array() + 1);
    }

    // transform points from [0, 1] to [lb, ub]
    for(size_t i = 0; i < _dim; ++i)
    {
        double a = _scaled_ub - _scaled_lb;
        double b = _scaled_lb;
        sampled.row(i) = a * sampled.row(i).array() + b;
    }
    return sampled;
}
void MACE::set_init_num(size_t n) { _num_init = n; }
void MACE::set_max_eval(size_t n) { _max_eval = n; }
void MACE::set_batch(size_t n) { _batch_size = n; }
void MACE::set_force_select_hyp(bool f) { _force_select_hyp = f; }
void MACE::set_tol_no_improvement(size_t n) { _tol_no_improvement = n; }
void MACE::set_eval_fixed(size_t n) { _eval_fixed = n; }
void MACE::set_seed(size_t s) 
{ 
    _seed = s;
    _engine.seed(_seed); 
}
void MACE::set_gp_noise_lower_bound(double lvl) { _noise_lvl = lvl; }
void MACE::set_mo_record(bool r) {_mo_record = r;}
void MACE::set_mo_gen(size_t gen){_mo_gen = gen;}
void MACE::set_mo_np(size_t np){_mo_np = np;}
void MACE::set_mo_f(double f){_mo_f = f;}
void MACE::set_mo_cr(double cr){_mo_cr = cr;}
VectorXd MACE::best_x() const { return _best_x; }
VectorXd MACE::best_y() const { return _best_y; }
void MACE::optimize()
{
    if(_gp == nullptr)
        initialize(_num_init);
    while(_eval_counter < _max_eval)
    {
        optimize_one_step();
    }
}
MatrixXd MACE::_adaptive_sampling()
{
    assert(_gp != nullptr);
    assert(_gp->trained());
    GP tmp_gp(_gp->train_in(), _gp->train_out());
    tmp_gp.set_fixed(true);
    tmp_gp.set_noise_free(_noise_free);
    tmp_gp.set_noise_lower_bound(_noise_lvl);
    MatrixXd one_step_eval_x = MatrixXd(_dim, _batch_size);
    for(size_t i = 0; i < _batch_size; ++i)
    {
        tmp_gp.train(_gp->get_hyp());
        MVMO::MVMO_Obj f = [&](const VectorXd& x)->double{
            double gpy, gps2;
            tmp_gp.predict(0, x, gpy, gps2);
            return -1 * gps2;
        };
        const VectorXd lb = VectorXd::Constant(_dim, 1, _scaled_lb);
        const VectorXd ub = VectorXd::Constant(_dim, 1, _scaled_ub);
        MVMO mvmo_opt(f, lb, ub);
        mvmo_opt.set_max_eval(_dim * 100);
        mvmo_opt.set_archive_size(25);
        mvmo_opt.optimize();
        VectorXd new_x = mvmo_opt.best_x();
        MatrixXd new_gpy, new_gps2;
        tmp_gp.predict(new_x, new_gpy, new_gps2);
        tmp_gp.add_data(new_x, new_gpy);
        one_step_eval_x.col(i) = new_x;
    }
    return one_step_eval_x;
}
void MACE::blcb()
{
    if(_gp == nullptr)
        initialize(_num_init);
    while(_eval_counter < _max_eval)
    {
        _eval_x = blcb_one_step();
        _eval_y = _run_func(_eval_x);
        _print_log();
        _gp->add_data(_eval_x, _eval_y.transpose());
    }
}
MatrixXd MACE::blcb_one_step() // one iteration of BO, so that BO could be used as a plugin of other application
{
    // Train GP model
    if(_gp == nullptr)
    {
        BOOST_LOG_TRIVIAL(error) << "GP not initialized";
        exit(EXIT_FAILURE);
    }
    
    if(not _have_feas)
    {
        BOOST_LOG_TRIVIAL(error) << "BLCB method is only used for unconstrained optimization";
        exit(EXIT_FAILURE);
    }
    else
    {
        _set_kappa();
        _train_GP();
        GP tmp_gp(_gp->train_in(), _gp->train_out());
        tmp_gp.set_fixed(true);
        tmp_gp.set_noise_free(_noise_free);
        tmp_gp.set_noise_lower_bound(_noise_lvl);
        MatrixXd one_step_eval_x = MatrixXd(_dim, _batch_size);
        for(size_t i = 0; i < _batch_size; ++i)
        {
            tmp_gp.train(_gp->get_hyp());
            MVMO::MVMO_Obj f = [&](const VectorXd& x)->double{
                double gpy, gps2, gps;
                tmp_gp.predict(0, x, gpy, gps2);
                gps = sqrt(gps2);
                double lcb = gpy - _kappa * gps;
                return lcb;
            };
            NLopt_wrapper::func fls = [&](const VectorXd& x, VectorXd& g)->double{
                double gpy, gps2, gps;
                VectorXd grad_y, grad_s2, grad_s;
                tmp_gp.predict_with_grad(0, x, gpy, gps2, grad_y, grad_s2);
                gps    = sqrt(gps2);
                grad_s = 0.5 * grad_s2 / gps;
                g      = grad_y - _kappa * grad_s;
                double lcb =  gpy - _kappa * gps;
                return lcb;
            };
            const VectorXd lb = VectorXd::Constant(_dim, 1, _scaled_lb);
            const VectorXd ub = VectorXd::Constant(_dim, 1, _scaled_ub);
            MatrixXd anchor(_dim, 1 + i);
            anchor << _unscale(_best_x), one_step_eval_x.leftCols(i);
            MVMO mvmo_opt(f, lb, ub);
            mvmo_opt.set_max_eval(_dim * 100);
            mvmo_opt.set_archive_size(25);
            mvmo_opt.optimize(anchor);
            VectorXd new_x = _msp(fls, mvmo_opt.best_x());
            MatrixXd new_gpy, new_gps2;
            tmp_gp.predict(new_x, new_gpy, new_gps2);
            tmp_gp.add_data(new_x, new_gpy);
            one_step_eval_x.col(i) = new_x;
        }
        one_step_eval_x = _adjust_x(one_step_eval_x);
        return one_step_eval_x;
    }
}
void MACE::optimize_one_step() // one iteration of BO, so that BO could be used as a plugin of other application
{
    // Train GP model
    if(_gp == nullptr)
    {
        BOOST_LOG_TRIVIAL(error) << "GP not initialized";
        exit(EXIT_FAILURE);
    }
    _train_GP();
    _set_best_posterior_mean();
    BOOST_LOG_TRIVIAL(trace) << "Best posterior: " << _best_posterior_y.transpose();
    
    // XXX: This is a fast-prototype, possible improvements includes:
    // 1. Use gradient-based MSP to optimize the PF
    // 2. More advanced techniques to incorporate constraints
    // 3. More advanced techniques to transform the LCB function
    
    // TODO: MSP for PF optimization
    MOO::ObjF neg_log_pf = [&](const VectorXd xs)->VectorXd{
        VectorXd obj(1);
        obj << -1 * _log_pf(xs);
        return obj;
    };
    if(not _have_feas)
    {
        // If no feasible solution is found, optimize PF firstly
        MOO pf_optimizer(neg_log_pf, 1, VectorXd::Constant(_dim, 1, _scaled_lb), VectorXd::Constant(_dim, 1, _scaled_ub));
        _moo_config(pf_optimizer);
        pf_optimizer.moo();
        MYASSERT(pf_optimizer.pareto_set().cols() == 1);
        _eval_x = pf_optimizer.pareto_set();
        _eval_x = _adjust_x(_eval_x);
        _eval_y = _run_func(_eval_x);
    }
    else
    {
        if(_no_improve_counter > 0 and _no_improve_counter % _tol_no_improvement == 0)
        {
            // XXX: for unconstrained problem
            assert(_num_spec == 1);
            BOOST_LOG_TRIVIAL(trace) << "Sample points with max uncertainty";
            _eval_x = _adaptive_sampling();
        }
        else
        {
            // If there are feasible solutions, perform MOO to (EI, LCB) functions
            _set_kappa();
            MOO::ObjF mo_acq = [&](const VectorXd xs)->VectorXd{
                VectorXd objs(_acq_pool.size());
                vector<double> acq_vals;
                for(auto name : _acq_pool)
                    acq_vals.push_back(_acq(name, xs));
                return -1*convert(acq_vals);
            };
            MOO acq_optimizer(mo_acq, _acq_pool.size(), VectorXd::Constant(_dim, 1, _scaled_lb), VectorXd::Constant(_dim, 1, _scaled_ub));
            _moo_config(acq_optimizer);
            acq_optimizer.set_anchor(_set_anchor());
            acq_optimizer.set_crowding_space(MOO::CrowdingSpace::Output);
            acq_optimizer.moo();
            MatrixXd ps = acq_optimizer.pareto_set();
            MatrixXd pf = acq_optimizer.pareto_front();
            _eval_x     = _select_candidate(ps, pf);
#ifdef MYDEBUG
            BOOST_LOG_TRIVIAL(trace) << "Pareto set:\n"   << _rescale(ps).transpose() << endl;
            BOOST_LOG_TRIVIAL(trace) << "Pareto front:\n" << pf.transpose() << endl;

            VectorXd true_global = read_matrix("true_global");
            if((size_t)true_global.size() != _dim)
            {
                BOOST_LOG_TRIVIAL(warning) << "True_global read: " << true_global;
                true_global = VectorXd::Zero(_dim, 1);
            }
            true_global = _unscale(true_global);
            MatrixXd y_glb, s2_glb;
            _gp->predict(true_global, y_glb, s2_glb);
            VectorXd acq_glb = mo_acq(true_global);
            BOOST_LOG_TRIVIAL(debug) << "True global: "          << _rescale(true_global).transpose();
            BOOST_LOG_TRIVIAL(debug) << "GPY for true global: "  << y_glb;
            BOOST_LOG_TRIVIAL(debug) << "GPS for true global: "  << s2_glb.cwiseSqrt();
            BOOST_LOG_TRIVIAL(debug) << "Acq for true global: "  << acq_glb.transpose();
            for(long i = 0; i < _eval_x.cols(); ++i)
            {
                BOOST_LOG_TRIVIAL(debug) << "Acq for _eval_x: " << mo_acq(_eval_x.col(i)).transpose()
                    << ", distance to true global: " << (_eval_x.col(i) - true_global).lpNorm<2>();
            }
#endif
        }
        _eval_x = _adjust_x(_eval_x);
        _eval_y = _run_func(_eval_x);
    }
    _print_log();
    _gp->add_data(_eval_x, _eval_y.transpose());
}
void MACE::_print_log()
{
    MatrixXd pred_y, pred_s2;
    _gp->predict(_eval_x, pred_y, pred_s2);
    BOOST_LOG_TRIVIAL(info) << "Pred-S-Eval:";
    for(long i = 0; i < _eval_x.cols(); ++i)
    {
        MatrixXd record(3, _num_spec);
        record << pred_y.row(i), pred_s2.row(i).cwiseSqrt(), _eval_y.col(i).transpose();
        BOOST_LOG_TRIVIAL(info) << record;
        BOOST_LOG_TRIVIAL(info) << "-----";
    }
    BOOST_LOG_TRIVIAL(info) << "Kappa: " << _kappa;
    BOOST_LOG_TRIVIAL(info) << "Best_y: "         << _best_y.transpose();
    BOOST_LOG_TRIVIAL(info) << "No improvement: " << _no_improve_counter;
    BOOST_LOG_TRIVIAL(info) << "Evaluated: "      << _eval_counter;
    BOOST_LOG_TRIVIAL(info) << "=============================================";
}
MatrixXd MACE::_slice_matrix(const MatrixXd& m, const vector<size_t>& idxs) const
{
    MYASSERT((long)*max_element(idxs.begin(), idxs.end()) < m.cols());
    MatrixXd sm(m.rows(), idxs.size());
    for(size_t i = 0; i < idxs.size(); ++i)
        sm.col(i) = m.col(idxs[i]);
    return sm;
}

void MACE::_moo_config(MOO& moo_optimizer) const
{
    moo_optimizer.set_f(_mo_f);
    moo_optimizer.set_cr(_mo_cr);
    moo_optimizer.set_np(_mo_np);
    moo_optimizer.set_gen(_mo_gen);
    moo_optimizer.set_seed(_seed);
    moo_optimizer.set_record(_mo_record);
}

void MACE::_train_GP()
{
    auto train_start = chrono::high_resolution_clock::now();
    _gp->set_fixed(_eval_counter > _eval_fixed);
    if (_force_select_hyp || (_no_improve_counter > 0 && _no_improve_counter % _tol_no_improvement == 0))
    {
        if(_eval_counter <= _eval_fixed)
        {
            BOOST_LOG_TRIVIAL(info) << "Re-select initial hyp" << endl;
            _gp->set_fixed(false);
            _hyps = _gp->select_init_hyp(1000, _hyps);
            _nlz  = _gp->train(_hyps);
            BOOST_LOG_TRIVIAL(info) << _hyps << endl;
        }
    }
    _nlz  = _gp->train(_hyps);
    _hyps = _gp->get_hyp();
    auto train_end          = chrono::high_resolution_clock::now();
    const double time_train = duration_cast<chrono::milliseconds>(train_end - train_start).count();
    BOOST_LOG_TRIVIAL(info) << "Hyps: \n"               << _hyps.transpose();
    BOOST_LOG_TRIVIAL(info) << "nlz for training set: " << _nlz.transpose();
    BOOST_LOG_TRIVIAL(info) << "Time for GP training: " << (time_train/1000.0) << " s";
}

vector<size_t> MACE::_pick_from_seq(size_t n, size_t m)
{
    MYASSERT(m <= n);
    set<size_t> picked_set;
    uniform_int_distribution<size_t> i_distr(0, n - 1);
    while(picked_set.size() < m)
        picked_set.insert(i_distr(_engine));
    vector<size_t> picked_vec(m);
    std::copy(picked_set.begin(), picked_set.end(), picked_vec.begin());
    return picked_vec;
}
double MACE::_pf(const VectorXd& xs) const
{
    MYASSERT(_gp->trained());
    if(_num_spec == 1)
        return 1.0;
    MatrixXd gpy, gps2;
    _gp->predict(xs, gpy, gps2);
    MatrixXd normed = -1 * gpy.cwiseQuotient(gps2.cwiseSqrt());
    double prob = 1.0;
    for(long i = 1; i < gpy.cols(); ++i)
        prob *= normcdf(normed(i));
    return prob;
}
double MACE::_pf(const VectorXd& xs, VectorXd& grad) const
{
    MYASSERT(_gp->trained());
    const double pf  = exp(_log_pf(xs, grad));
    grad            *= pf;
    return pf;
}
double MACE::_log_pf(const VectorXd& xs) const
{
    MYASSERT(_gp->trained());
    if(_num_spec == 1)
        return 0.0;
    MatrixXd gpy, gps2;
    _gp->predict(xs, gpy, gps2);
    MatrixXd normed = -1 * gpy.cwiseQuotient(gps2.cwiseSqrt());
    double log_prob = 0.0;
    for(long i = 1; i < gpy.cols(); ++i)
        log_prob += logphi(normed(i));
    return log_prob;
}
double MACE::_log_pf(const VectorXd& xs, VectorXd& grad) const
{
    MYASSERT(_gp->trained());
    if(_num_spec == 1)
    {
        grad = VectorXd::Zero(xs.size());
        return 0.0;
    }
    double log_prob = 0.0;
    for(size_t i = 1; i < _num_spec; ++i)
    {
        double y, s2, s;
        VectorXd gy, gs2, gs;
        _gp->predict_with_grad(i, xs, y, s2, gy, gs2);
        s      = sqrt(s2);
        gs     = 0.5 * gs2 / sqrt(s2);
        double normed    = -1 * y / s;
        VectorXd gnormed = -1 * (s * gy - y * gs) / s2;
        double lp, dlp;
        logphi(normed, lp, dlp);
        log_prob += lp;
        grad     += dlp * gnormed;
    }
    return log_prob;
}
double MACE::_s2(const VectorXd& x)const
{
    MYASSERT(_gp->trained());
    double  y, s2;
    _gp->predict(0, x, y, s2);
    return s2;
}
double MACE::_s2(const VectorXd& x, VectorXd& grad)const
{
    MYASSERT(_gp->trained());
    double  y, s2;
    VectorXd gy, gs2;
    _gp->predict_with_grad(0, x, y, s2, gy, gs2);
    grad = gs2;
    return s2;
}
double MACE::_pi_transf(const VectorXd& x) const
{
    MYASSERT(_gp->trained());
    double  y, s2;
    _gp->predict(0, x, y, s2);
    // XXX: What about INF/NaN?
    const double s   = sqrt(s2);
    const double tau = _get_tau(0);
    double normed    = (tau - y) / s;
    return normed;
}
double MACE::_pi_transf(const VectorXd& x, VectorXd& grad) const
{
    MYASSERT(_gp->trained());
    const double tau = _get_tau(0);
    double  y, s2, s;
    VectorXd gy, gs2, gs;
    _gp->predict_with_grad(0, x, y, s2, gy, gs2);
    s  = sqrt(s2);
    gs = 0.5 * gs2 / sqrt(s2);
    const double   normed  = (tau - y) / s;
    const VectorXd gnormed = -1 * (s * gy + (tau - y) * gs) / s2;
    grad = gnormed;
    return normed;
}
double MACE::_acq(string name, const VectorXd& x) const
{
    if(_num_spec > 1)
    {
        cerr << "Currently only for unconstrained optimization" << endl;
        exit(EXIT_FAILURE);
    }
    if(name == "pi_transf")
        return _pi_transf(x);
    else if(name == "log_lcb_improv_transf")
        return _log_lcb_improv_transf(x);
    else if(name == "log_ei")
        return _log_ei(x);
    else if(name == "s2")
        return _s2(x);
    else
    {
        BOOST_LOG_TRIVIAL(fatal) << "Unknown acquisition function: " << name;
        exit(EXIT_FAILURE);
    }
}
double MACE::_acq(string name, const VectorXd& x, VectorXd& grad) const
{
    if(name == "pi_transf")
        return _pi_transf(x, grad);
    else if(name == "log_lcb_improv_transf")
        return _lcb_improv_transf(x, grad);
    else if(name == "log_ei")
        return _log_ei(x, grad);
    else if(name == "s2")
        return _s2(x, grad);
    else
    {
        BOOST_LOG_TRIVIAL(fatal) << "Unknown acquisition function: " << name;
        exit(EXIT_FAILURE);
    }
}
double MACE::_ei(const VectorXd& x) const
{
    MYASSERT(_gp->trained());
    double  y, s2;
    _gp->predict(0, x, y, s2);
    const double s      = sqrt(s2);
    const double tau    = _get_tau(0);
    const double normed = (tau - y) / sqrt(s2);
    return s * (normed * normcdf(normed) + normpdf(normed));
}

double MACE::_ei(const VectorXd& x, VectorXd& grad) const
{
    MYASSERT(_gp->trained());
    const double tau = _get_tau(0);
    double  y, s2, s;
    VectorXd gy, gs2, gs;
    _gp->predict_with_grad(0, x, y, s2, gy, gs2);
    s  = sqrt(s2);
    gs = 0.5 * gs2 / sqrt(s2);
    const double   normed    = (tau - y) / sqrt(s2);
    const double   cdfnormed = normcdf(normed);
    const VectorXd gnormed   = -1 * (s * gy + (tau - y) * gs) / s2;
    const double   lambda    = normed * cdfnormed + normpdf(normed);
    grad = s * cdfnormed * gnormed + lambda * gs;
    return s * lambda;
}

double MACE::_log_ei(const VectorXd& x) const
{
    double y, s2;
    _gp->predict(0, x, y, s2);
    const double s      = sqrt(s2);
    const double tau    = _get_tau(0);
    const double normed = (tau - y) / sqrt(s2);
    return normed > -6 ? log(s * (normed * normcdf(normed) + normpdf(normed))) 
                       : log(s) - 0.5 * pow(normed, 2) - log(sqrt(2 * M_PI)) - log(pow(normed, 2) - 1);
    // \lim_{z \to -\infty} \log\big(z\Phi(z) + \phi(z)\big) = \log \phi(z) - \log(z^2 - 1) 
}

double MACE::_log_ei(const VectorXd& x, VectorXd& grad) const
{
    const double tau = _get_tau(0);
    double y, s2, s;
    VectorXd gy, gs2, gs;
    _gp->predict_with_grad(0, x, y, s2, gy, gs2);
    s  = sqrt(s2);
    gs = 0.5 * gs2 / sqrt(s2);
    const double normed = (tau - y) / sqrt(s2);
    const VectorXd gnormed   = -1 * (s * gy + (tau - y) * gs) / s2;
    double log_ei;
    if(normed > -6)
    {
        const double   cdfnormed = normcdf(normed);
        const double   lambda    = normed * cdfnormed + normpdf(normed);
        double ei                = s * lambda;
        grad   = (s * cdfnormed * gnormed + lambda * gs) / ei;
        log_ei = log(ei);
    }
    else
    {
        grad   = gs / s - normed * gnormed - (2 * normed) / (pow(normed, 2) - 1) * gnormed;
        log_ei = log(s) - 0.5 * pow(normed, 2) - log(sqrt(2 * M_PI)) - log(pow(normed, 2) - 1);
    }
    return log_ei;
}
double MACE::_lcb_improv(const VectorXd& x) const 
{
    const double tau   = _get_tau(0);
    double y, s2;
    _gp->predict(0, x, y, s2);
    const double lcb = y - _kappa * sqrt(s2);
    return tau - lcb;
}
double MACE::_lcb_improv(const VectorXd& x, VectorXd& grad) const 
{
    const double tau   = _get_tau(0);
    double y, s2, lcb;
    VectorXd gy, gs2, gs;
    _gp->predict_with_grad(0, x, y, s2, gy, gs2);
    gs   = 0.5 * gs2 / sqrt(s2);
    lcb  = y - _kappa * sqrt(s2);
    grad = -1 * (gy - _kappa * gs);
    return tau - lcb;
}
double MACE::_lcb_improv_transf(const VectorXd& x) const
{
    const double lcb_improve = _lcb_improv(x);
    return lcb_improve > 20 ? lcb_improve : log(1+exp(lcb_improve));
}
double MACE::_lcb_improv_transf(const VectorXd& x, VectorXd& grad) const
{
    const double lcb_improve  = _lcb_improv(x, grad);
    const double val          = lcb_improve > 20 ? lcb_improve : log(1+exp(lcb_improve));
    grad                     *= lcb_improve > 20 ? 1.0 : exp(val) / (1 + exp(val));
    return val;
}
double MACE::_log_lcb_improv_transf(const VectorXd& x) const
{
    const double lcb_improve = _lcb_improv(x);
    if(lcb_improve > 20)
    {
        return log(lcb_improve);
    }
    else if(lcb_improve > -10)
    {
        return log(log(1+exp(lcb_improve)));
    }
    else
    {
        return lcb_improve - 0.5 * exp(lcb_improve);
    }
}
double MACE::_log_lcb_improv_transf(const VectorXd& x, VectorXd& grad) const
{
    const double lcb_improve = _lcb_improv(x, grad);
    double val;
    if(lcb_improve > 20)
    {
        val   = log(lcb_improve);
        grad *= 1.0 / val;
    }
    else if(lcb_improve > -10)
    {
        val   = log(log(1+exp(lcb_improve)));
        grad *= exp(lcb_improve) / (log(1+exp(lcb_improve)) * (1 + exp(lcb_improve)));
    }
    else
    {
        val   = lcb_improve - 0.5 * exp(lcb_improve);
        grad *= (1 - 0.5 * exp(lcb_improve));
    }
    return val;
}
VectorXd MACE::_msp(NLopt_wrapper::func f, const MatrixXd& sp, nlopt::algorithm algo, size_t max_eval)
{
    double best_y   = INF;
    VectorXd best_x = sp.col(0);
#pragma omp parallel for
    for (long i = 0; i < sp.cols(); ++i)
    {
        NLopt_wrapper opt(algo, _dim, _scaled_lb, _scaled_ub);
        opt.set_maxeval(max_eval);
        opt.set_ftol_rel(1e-6);
        opt.set_xtol_rel(1e-6);
        opt.set_min_objective(f);
        VectorXd x = sp.col(i);
        double y   = INF;
        try
        {
            opt.optimize(x, y);
        }
        catch (runtime_error& e)  // this kind of exception can usually be ignored
        {
            if(algo != nlopt::LN_SBPLX)
            {
                VectorXd fg;
                x = _msp(f, x, nlopt::LN_SBPLX, max_eval * 3);
                y = f(x, fg);
            }
        }
        catch (exception& e)
        {
            BOOST_LOG_TRIVIAL(fatal) << "Nlopt exception: " << e.what() << " for sp: " << sp.col(i).transpose()
                                     << ", y = " << y;
            exit(EXIT_FAILURE);
        }
#pragma omp critical
        {
            if (y < best_y)
            {
                best_x = x;
                best_y = y;
            }
        }
    }
    return best_x;
}
MatrixXd MACE::_set_anchor()
{
    const size_t num_rand_samp = 3;
    MatrixXd sp(_dim, 2 + num_rand_samp);
    sp << _unscale(_best_x), _best_posterior_x, _set_random(num_rand_samp);
    MatrixXd random_fluctuation(sp.rows(), sp.cols());
    random_fluctuation.setRandom();
    random_fluctuation *= 1e-3 * (_scaled_ub - _scaled_lb);
    sp += random_fluctuation;
    sp = sp.cwiseMin(_scaled_ub).cwiseMax(_scaled_lb);
    MatrixXd heuristic_anchors(_dim, _acq_pool.size());
    const VectorXd lb = VectorXd::Constant(_dim, 1, _scaled_lb);
    const VectorXd ub = VectorXd::Constant(_dim, 1, _scaled_ub);
    for(size_t i = 0; i < _acq_pool.size(); ++i)
    {
        NLopt_wrapper::func f = [&](const VectorXd& x, VectorXd& grad)->double{
            double val =  -1*_acq(_acq_pool[i], x, grad);
            grad *= -1;
            return val;
        };
        MVMO::MVMO_Obj mvmvo_f = [&](const VectorXd& x)->double{
            return -1*_acq(_acq_pool[i], x);
        };
        MatrixXd mvmo_guess(_dim, i + 1);
        mvmo_guess.col(0)       = _msp(f, sp, nlopt::LD_LBFGS, 40);
        mvmo_guess.rightCols(i) = heuristic_anchors.leftCols(i);
        MVMO mvmo_opt(mvmvo_f, lb, ub);
        mvmo_opt.set_max_eval(_dim * 50);
        mvmo_opt.set_archive_size(25);
        mvmo_opt.optimize(mvmo_guess);
        heuristic_anchors.col(i) = _msp(f, mvmo_opt.best_x(), nlopt::LD_LBFGS, 40);
    }
    // for(size_t i = 0; i <= num_weight; ++i)
    // {
    //     const double alpha = (1.0 * i) / num_weight;
    //     NLopt_wrapper::func f = [&](const VectorXd& x, VectorXd& grad)->double{
    //         double   log_pf, log_ei, log_lcb_improv_transf;
    //         VectorXd glog_pf, glog_ei, glog_lcb_improv_transf;
    //         log_pf                = _log_pf(x, glog_pf);
    //         log_ei                = _log_ei(x, glog_ei);
    //         log_lcb_improv_transf = _log_lcb_improv_transf(x, glog_lcb_improv_transf);
    //         grad                  = -1 * (glog_pf + alpha * glog_ei + (1.0 - alpha) * glog_lcb_improv_transf);
    //         return -1 * (log_pf + alpha * log_ei + (1.0 - alpha) * log_lcb_improv_transf);
    //     };
    //     MVMO::MVMO_Obj mvmvo_f = [&](const VectorXd& x)->double{
    //         double log_pf, log_ei, log_lcb_improv_transf;
    //         log_pf                = _log_pf(x);
    //         log_ei                = _log_ei(x);
    //         log_lcb_improv_transf = _log_lcb_improv_transf(x);
    //         return -1 * (log_pf + alpha * log_ei + (1.0 - alpha) * log_lcb_improv_transf);
    //     };
    //     MatrixXd mvmo_guess(_dim, i + 1);
    //     mvmo_guess.col(0)       = _msp(f, sp, nlopt::LD_LBFGS, 40);
    //     mvmo_guess.rightCols(i) = heuristic_anchors.leftCols(i);
    //     MVMO mvmo_opt(mvmvo_f, lb, ub);
    //     mvmo_opt.set_max_eval(_dim * 50);
    //     mvmo_opt.set_archive_size(25);
    //     mvmo_opt.optimize(mvmo_guess);
    //     heuristic_anchors.col(i) = mvmo_opt.best_x();
    // }
    return heuristic_anchors;
}
MatrixXd MACE::_select_candidate(const MatrixXd& ps, const MatrixXd& pf)
{
    switch(_ss)
    {
        case Random:
            return _select_candidate_random(ps, pf);
        case Greedy:
            return _select_candidate_greedy(ps, pf);
        case Extreme:
            return _select_candidate_extreme(ps, pf);
    }
}
MatrixXd MACE::_select_candidate_extreme(const MatrixXd& ps, const MatrixXd& pf)
{
    MatrixXd candidates = _select_candidate_random(ps, pf);
    const size_t num_extreme = std::min(_acq_pool.size(), std::min(_batch_size, (size_t)(ps.cols())));
    for(size_t i = 0; i < num_extreme; ++i)
    {
        size_t best_idx;
        pf.row(i).minCoeff(&best_idx);
        candidates.col(i) = ps.col(best_idx);
    }
    return candidates;
}
MatrixXd MACE::_select_candidate_random(const MatrixXd& ps, const MatrixXd&)
{
    vector<size_t> eval_idxs = _pick_from_seq(ps.cols(), (size_t)ps.cols() > _batch_size ? _batch_size : ps.cols());
    size_t num_rand = _batch_size > eval_idxs.size() ? _batch_size - eval_idxs.size() : 0;
    MatrixXd candidates(_dim, _batch_size);
    candidates << _slice_matrix(ps, eval_idxs), _set_random(num_rand);
    if(num_rand > 0)
        BOOST_LOG_TRIVIAL(trace) << "NumRand: " << num_rand;
    return candidates;
}
MatrixXd MACE::_select_candidate_greedy(const MatrixXd& ps, const MatrixXd&)
{
    const size_t batch_selection = (size_t)ps.cols() < _batch_size ? ps.cols() : _batch_size;
    const MatrixXd& dbx          = _gp->train_in();
    vector<size_t> selected_idx;
    for(size_t i = 0; i < batch_selection; ++i)
    {
        MatrixXd ref(_dim, dbx.cols() + selected_idx.size());
        ref.leftCols(dbx.cols()) = dbx;
        if(not selected_idx.empty())
            ref.rightCols(selected_idx.size()) = _slice_matrix(ps, selected_idx);
        VectorXd dists(ps.cols());
        long max_idx = 0;
        for(long j  = 0; j < ps.cols(); ++j)
            dists[j] = (ref.colwise() - ps.col(j)).colwise().norm().minCoeff();
        dists.maxCoeff(&max_idx);
        selected_idx.push_back(max_idx);
    }
    size_t num_rand = _batch_size  - selected_idx.size();
    MatrixXd candidates(_dim, _batch_size);
    candidates << _slice_matrix(ps, selected_idx), _set_random(num_rand);
    if(num_rand > 0)
        BOOST_LOG_TRIVIAL(trace) << "NumRand: " << num_rand;
    return candidates;
}
void MACE::_set_kappa()
{
    // Brochu, Eric, Vlad M. Cora, and Nando De Freitas. "A tutorial on Bayesian
    // optimization of expensive cost functions, with application to active user
    // modeling and hierarchical reinforcement learning." arXiv preprint
    // arXiv:1012.2599 (2010).
    const double t = 1.0 + (1.0 * (_eval_counter - _num_init)) / _batch_size;
    _kappa         = sqrt(_upsilon * 2 * log(pow(t, 2.0 + _dim / 2.0) * 3 * pow(M_PI, 2) / (3 * _delta)));
    // const size_t iter = 1.0 + (_eval_counter - _num_init) / _batch_size;
    // _kappa = sqrt(_upsilon * 2 * log((pow(M_PI*iter, 2) / 6.0) * (_dim * 1.0 / _delta)));
}
double MACE::_get_tau(size_t spec_idx) const
{
    // XXX: Only for unconstrained problems
    if(_posterior_ref)
        return _best_posterior_y(spec_idx) - std::max(0.0, _EI_jitter);
    else
        return _best_y(spec_idx) - std::max(0.0, _EI_jitter);
}
bool  MACE::_duplication_checking(const VectorXd& x) const 
{
    return _duplication_checking(x, _gp->train_in());
}
bool  MACE::_duplication_checking(const VectorXd& x, const MatrixXd& ref) const
{
    for(long i = 0; i < ref.cols(); ++i)
    {
        if((x - ref.col(i)).lpNorm<2>() < _eps * (_scaled_ub - _scaled_lb))
            return true;
    }
    return false;
}
MatrixXd MACE::_adjust_x(const MatrixXd& x)
{
    assert((size_t)x.rows() == _dim);
    assert(x.cols() >  0);
    MatrixXd adjusted = x;
    for(long i = 0; i < adjusted.cols(); ++i)
    {
        MatrixXd ref(_dim, _gp->train_in().cols() + x.cols() - i - 1);
        ref << _gp->train_in(), x.rightCols(x.cols() - i - 1);
        while(_duplication_checking(adjusted.col(i), ref))
            adjusted.col(i) = _set_random(1);
        if(adjusted.col(i) != x.col(i))
            BOOST_LOG_TRIVIAL(trace) << "Random sampling to avoid duplication evaluation for eval_x " << i;
    }
    return adjusted;
}
void MACE::_set_best_posterior_mean()
{
    //XXX: If MACE is expanded to constrained problems, this function shoule be reimplemented!
    assert(_gp != nullptr and _gp->trained());
    VectorXd lb = VectorXd::Constant(_dim, 1, _scaled_lb);
    VectorXd ub = VectorXd::Constant(_dim, 1, _scaled_ub);
    auto mvmo_obj = [&](const VectorXd& xs)->double{
        double y, s2;
        _gp->predict(0, xs, y, s2);
        return y;
    };
    auto msp_obj = [&](const VectorXd& xs, VectorXd& grad)->double{
        double y, s2;
        VectorXd gy, gs2;
        _gp->predict_with_grad(0, xs, y, s2, gy, gs2);
        grad = gy;
        return y;
    };
    MVMO mvmo_opt(mvmo_obj, lb, ub);
    mvmo_opt.set_max_eval(_dim * 50);
    mvmo_opt.set_archive_size(10);
    mvmo_opt.optimize(_unscale(_best_x));
    _best_posterior_x = _msp(msp_obj, mvmo_opt.best_x(), nlopt::LD_LBFGS, 40);
    MatrixXd tmp_gpy;
    MatrixXd tmp_gps2;
    _gp->predict(_best_posterior_x, tmp_gpy, tmp_gps2);
    _best_posterior_y = tmp_gpy.row(0).transpose();
}
