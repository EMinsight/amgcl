#ifndef AMGCL_PRECONDITIONER_SIMPLE_HPP
#define AMGCL_PRECONDITIONER_SIMPLE_HPP

/*
The MIT License

Copyright (c) 2012-2017 Denis Demidov <dennis.demidov@gmail.com>
Copyright (c) 2016, Riccardo Rossi, CIMNE (International Center for Numerical Methods in Engineering)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   amgcl/preconditioner/schur_pressure_correction.hpp
 * \author Denis Demidov <dennis.demidov@gmail.com>
 * \brief  SIMPLE preconditioning scheme.
 */

#include <vector>

#include <boost/static_assert.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>

#include <amgcl/backend/builtin.hpp>
#include <amgcl/util.hpp>
#include <amgcl/preconditioner/detail/compatible_backends.hpp>

namespace amgcl {
namespace preconditioner {

/// Schur-complement pressure correction preconditioner
template <class USolver, class PSolver>
class simple {
    BOOST_STATIC_ASSERT_MSG(
            (
             detail::compatible_backends<
                 typename USolver::backend_type,
                 typename PSolver::backend_type
                 >::value
            ),
            "Backends for pressure and flow preconditioners should coincide!"
            );
    public:
        typedef
            typename detail::common_backend<
                typename USolver::backend_type,
                typename PSolver::backend_type
                >::type
            backend_type;

        typedef typename backend_type::value_type value_type;
        typedef typename backend_type::matrix     matrix;
        typedef typename backend_type::vector     vector;
        typedef typename backend_type::params     backend_params;

        typedef typename backend::builtin<value_type>::matrix build_matrix;

        struct params {
            typedef typename USolver::params usolver_params;
            typedef typename PSolver::params psolver_params;

            usolver_params usolver;
            psolver_params psolver;

            std::vector<char> pmask;

            params() {}

            params(const boost::property_tree::ptree &p)
                : AMGCL_PARAMS_IMPORT_CHILD(p, usolver),
                  AMGCL_PARAMS_IMPORT_CHILD(p, psolver)
            {
                void *pm = 0;
                size_t n = 0;

                pm = p.get("pmask",     pm);
                n  = p.get("pmask_size", n);

                precondition(pm,
                        "Error in schur_complement parameters: "
                        "pmask is not set");

                precondition(n > 0,
                        "Error in schur_complement parameters: "
                        "pmask is set, but pmask_size is not"
                        );

                pmask.assign(static_cast<char*>(pm), static_cast<char*>(pm) + n);

                AMGCL_PARAMS_CHECK(p, (usolver)(psolver)(pmask)(pmask_size));
            }

            void get(boost::property_tree::ptree &p, const std::string &path = "") const
            {
                AMGCL_PARAMS_EXPORT_CHILD(p, path, usolver);
                AMGCL_PARAMS_EXPORT_CHILD(p, path, psolver);
            }
        } prm;

        template <class Matrix>
        simple(
                const Matrix &K,
                const params &prm = params(),
                const backend_params &bprm = backend_params()
                )
            : prm(prm), n(backend::rows(K)), np(0), nu(0)
        {
            init(boost::make_shared<build_matrix>(K), bprm);
        }

        simple(
                boost::shared_ptr<build_matrix> K,
                const params &prm = params(),
                const backend_params &bprm = backend_params()
                )
            : prm(prm), n(backend::rows(*K)), np(0), nu(0)
        {
            init(K, bprm);
        }

        template <class Vec1, class Vec2>
        void apply(
                const Vec1 &rhs,
#ifdef BOOST_NO_CXX11_RVALUE_REFERENCES
                Vec2       &x
#else
                Vec2       &&x
#endif
                ) const
        {
            backend::spmv(1, *x2u, rhs, 0, *rhs_u);
            backend::spmv(1, *x2p, rhs, 0, *rhs_p);

            // Ai u = rhs_u
            backend::clear(*u);
            report("U1", (*U)(*rhs_u, *u));

            // rhs_p -= Kpu u
            backend::spmv(-1, *Kpu, *u, 1, *rhs_p);

            // S p = rhs_p
            backend::clear(*p);
            report("P1", (*P)(*S, *rhs_p, *p));

            // rhs_u -= Kup p
            backend::spmv(-1, *Kup, *p, 1, *rhs_u);

            // Ai u = rhs_u
            backend::clear(*u);
            report("U2", (*U)(*rhs_u, *u));

            backend::clear(x);
            backend::spmv(1, *u2x, *u, 1, x);
            backend::spmv(1, *p2x, *p, 1, x);
        }

        const matrix& system_matrix() const {
            return *K;
        }
    private:
        size_t n, np, nu;

        boost::shared_ptr<matrix> K, S, Kup, Kpu, x2u, x2p, u2x, p2x;
        boost::shared_ptr<vector> rhs_u, rhs_p, u, p;

        boost::shared_ptr<USolver> U;
        boost::shared_ptr<PSolver> P;

        void init(const boost::shared_ptr<build_matrix> &K, const backend_params &bprm)
        {
            typedef typename backend::row_iterator<build_matrix>::type row_iterator;

            this->K = backend_type::copy_matrix(K, bprm);

            // Extract matrix subblocks.
            boost::shared_ptr<build_matrix> Kuu = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> Kpu = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> Kup = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> Kpp = boost::make_shared<build_matrix>();

            std::vector<ptrdiff_t> idx(n);

            for(size_t i = 0; i < n; ++i)
                idx[i] = (prm.pmask[i] ? np++ : nu++);

            Kuu->set_size(nu, nu, true);
            Kup->set_size(nu, np, true);
            Kpu->set_size(np, nu, true);
            Kpp->set_size(np, np, true);

#pragma omp parallel for
            for(ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); ++i) {
                ptrdiff_t ci = idx[i];
                char      pi = prm.pmask[i];
                for(row_iterator k = backend::row_begin(*K, i); k; ++k) {
                    char pj = prm.pmask[k.col()];

                    if (pi) {
                        if (pj) {
                            ++Kpp->ptr[ci+1];
                        } else {
                            ++Kpu->ptr[ci+1];
                        }
                    } else {
                        if (pj) {
                            ++Kup->ptr[ci+1];
                        } else {
                            ++Kuu->ptr[ci+1];
                        }
                    }
                }
            }

            std::partial_sum(Kuu->ptr, Kuu->ptr + nu + 1, Kuu->ptr);
            std::partial_sum(Kup->ptr, Kup->ptr + nu + 1, Kup->ptr);
            std::partial_sum(Kpu->ptr, Kpu->ptr + np + 1, Kpu->ptr);
            std::partial_sum(Kpp->ptr, Kpp->ptr + np + 1, Kpp->ptr);

            Kuu->set_nonzeros(Kuu->ptr[nu]);
            Kup->set_nonzeros(Kup->ptr[nu]);
            Kpu->set_nonzeros(Kpu->ptr[np]);
            Kpp->set_nonzeros(Kpp->ptr[np]);

#pragma omp parallel for
            for(ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); ++i) {
                ptrdiff_t ci = idx[i];
                char      pi = prm.pmask[i];

                ptrdiff_t uu_head = 0, up_head = 0, pu_head = 0, pp_head = 0;

                if(pi) {
                    pu_head = Kpu->ptr[ci];
                    pp_head = Kpp->ptr[ci];
                } else {
                    uu_head = Kuu->ptr[ci];
                    up_head = Kup->ptr[ci];
                }

                for(row_iterator k = backend::row_begin(*K, i); k; ++k) {
                    ptrdiff_t  j = k.col();
                    value_type v = k.value();
                    ptrdiff_t cj = idx[j];
                    char      pj = prm.pmask[j];

                    if (pi) {
                        if (pj) {
                            Kpp->col[pp_head] = cj;
                            Kpp->val[pp_head] = v;
                            ++pp_head;
                        } else {
                            Kpu->col[pu_head] = cj;
                            Kpu->val[pu_head] = v;
                            ++pu_head;
                        }
                    } else {
                        if (pj) {
                            Kup->col[up_head] = cj;
                            Kup->val[up_head] = v;
                            ++up_head;
                        } else {
                            Kuu->col[uu_head] = cj;
                            Kuu->val[uu_head] = v;
                            ++uu_head;
                        }
                    }
                }
            }

            // Explicitly form the matrix
            //   S = Kpp - Kpu M Kup,
            // where
            //   M = approx(inv(dia(Kuu)))
            std::vector<value_type> M(nu);
#pragma omp parallel for
            for(ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(nu); ++i) {
                for(ptrdiff_t j = Kuu->ptr[i], e = Kuu->ptr[i+1]; j < e; ++j) {
                    if (Kuu->col[j] == i) {
                        M[i] = math::inverse(Kuu->val[j]);
                        break;
                    }
                }
            }

            boost::shared_ptr<build_matrix> S = boost::make_shared<build_matrix>();
            S->set_size(np, np);
            S->ptr[0] = 0;

#pragma omp parallel
            {
                std::vector<ptrdiff_t> marker(np, -1);

#pragma omp for
                for(ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(np); ++i) {
                    ptrdiff_t row_width = 0;

                    for(ptrdiff_t j = Kpp->ptr[i], e = Kpp->ptr[i+1]; j < e; ++j) {
                        marker[Kpp->col[j]] = i;
                        ++row_width;
                    }

                    for(ptrdiff_t jp = Kpu->ptr[i], ep = Kpu->ptr[i+1]; jp < ep; ++jp) {
                        ptrdiff_t cp = Kpu->col[jp];

                        for(ptrdiff_t ju = Kup->ptr[cp], eu = Kup->ptr[cp+1]; ju < eu; ++ju) {
                            ptrdiff_t cu = Kup->col[ju];
                            if (marker[cu] != i) {
                                marker[cu]  = i;
                                ++row_width;
                            }
                        }
                    }

                    S->ptr[i+1] = row_width;
                }
            }

            std::partial_sum(S->ptr, S->ptr + np + 1, S->ptr);
            S->set_nonzeros(S->ptr[np]);

#pragma omp parallel
            {
                std::vector<ptrdiff_t> marker(np, -1);

#pragma omp for
                for(ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(np); ++i) {
                    ptrdiff_t row_beg = S->ptr[i];
                    ptrdiff_t row_end = row_beg;

                    for(ptrdiff_t j = Kpp->ptr[i], e = Kpp->ptr[i+1]; j < e; ++j) {
                        ptrdiff_t  c = Kpp->col[j];
                        value_type v = Kpp->val[j];

                        marker[c] = row_end;

                        S->col[row_end] = c;
                        S->val[row_end] = v;


                        ++row_end;
                    }

                    for(ptrdiff_t jp = Kpu->ptr[i], ep = Kpu->ptr[i+1]; jp < ep; ++jp) {
                        ptrdiff_t  cp = Kpu->col[jp];
                        value_type vp = Kpu->val[jp] * M[cp];

                        for(ptrdiff_t ju = Kup->ptr[cp], eu = Kup->ptr[cp+1]; ju < eu; ++ju) {
                            ptrdiff_t  cu = Kup->col[ju];
                            value_type vu = Kup->val[ju];

                            if (marker[cu] < row_beg) {
                                marker[cu] = row_end;

                                S->col[row_end] = cu;
                                S->val[row_end] = -(vp * vu);

                                ++row_end;
                            } else {
                                S->val[marker[cu]] -= vp * vu;
                            }
                        }
                    }

                    amgcl::detail::sort_row(
                            S->col + row_beg, S->val + row_beg, row_end - row_beg);
                }
            }

            U = boost::make_shared<USolver>(*Kuu, prm.usolver, bprm);
            P = boost::make_shared<PSolver>(*Kpp, prm.psolver, bprm);

            this->Kup = backend_type::copy_matrix(Kup, bprm);
            this->Kpu = backend_type::copy_matrix(Kpu, bprm);
            this->S   = backend_type::copy_matrix(S,   bprm);

            rhs_u = backend_type::create_vector(nu, bprm);
            rhs_p = backend_type::create_vector(np, bprm);

            u = backend_type::create_vector(nu, bprm);
            p = backend_type::create_vector(np, bprm);

            // Scatter/Gather matrices
            boost::shared_ptr<build_matrix> x2u = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> x2p = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> u2x = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> p2x = boost::make_shared<build_matrix>();

            x2u->set_size(nu, n, true);
            x2p->set_size(np, n, true);
            u2x->set_size(n, nu, true);
            p2x->set_size(n, np, true);

            {
                ptrdiff_t x2u_head = 0, x2u_idx = 0;
                ptrdiff_t x2p_head = 0, x2p_idx = 0;
                ptrdiff_t u2x_head = 0, u2x_idx = 0;
                ptrdiff_t p2x_head = 0, p2x_idx = 0;

                for(size_t i = 0; i < n; ++i) {
                    if (prm.pmask[i]) {
                        x2p->ptr[++x2p_idx] = ++x2p_head;
                        ++p2x_head;
                    } else {
                        x2u->ptr[++x2u_idx] = ++x2u_head;
                        ++u2x_head;
                    }

                    p2x->ptr[++p2x_idx] = p2x_head;
                    u2x->ptr[++u2x_idx] = u2x_head;
                }
            }

            x2u->set_nonzeros();
            x2p->set_nonzeros();
            u2x->set_nonzeros();
            p2x->set_nonzeros();

            {
                ptrdiff_t x2u_head = 0;
                ptrdiff_t x2p_head = 0;
                ptrdiff_t u2x_head = 0;
                ptrdiff_t p2x_head = 0;

                for(size_t i = 0; i < n; ++i) {
                    ptrdiff_t j = idx[i];

                    if (prm.pmask[i]) {
                        x2p->col[x2p_head] = i;
                        x2p->val[x2p_head] = math::identity<value_type>();
                        ++x2p_head;

                        p2x->col[p2x_head] = j;
                        p2x->val[p2x_head] = math::identity<value_type>();
                        ++p2x_head;
                    } else {
                        x2u->col[x2u_head] = i;
                        x2u->val[x2u_head] = math::identity<value_type>();
                        ++x2u_head;

                        u2x->col[u2x_head] = j;
                        u2x->val[u2x_head] = math::identity<value_type>();
                        ++u2x_head;
                    }
                }
            }

            this->x2u = backend_type::copy_matrix(x2u, bprm);
            this->x2p = backend_type::copy_matrix(x2p, bprm);
            this->u2x = backend_type::copy_matrix(u2x, bprm);
            this->p2x = backend_type::copy_matrix(p2x, bprm);
        }

        friend std::ostream& operator<<(std::ostream &os, const simple &p) {
            os << "SIMPLE (two-stage preconditioner)" << std::endl;
            os << "  unknowns: " << p.n << "(" << p.np << ")" << std::endl;
            os << "  nonzeros: " << backend::nonzeros(p.system_matrix()) << std::endl;

            return os;
        }

#if defined(AMGCL_DEBUG) || !defined(NDEBUG)
        template <typename I, typename E>
        static void report(const std::string &name, const boost::tuple<I, E> &c) {
            std::cout << name << " (" << boost::get<0>(c) << ", " << boost::get<1>(c) << ")\n";
        }
#else
        template <typename I, typename E>
        static void report(const std::string&, const boost::tuple<I, E>&) {
        }
#endif
};

} // namespace preconditioner
} // namespace amgcl

#endif