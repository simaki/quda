#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <vector>
#include <algorithm>

#include <invert_quda.h>

#include <quda_internal.h>
#include <eigensolve_quda.h>
#include <qio_field.h>
#include <color_spinor_field.h>
#include <blas_quda.h>
#include <util_quda.h>

#include <Eigen/Eigenvalues>
#include <Eigen/Dense>

namespace quda
{

  using namespace Eigen;

  // -------------------------------
  // Davidson-type eigensolver class

  // Jacobi-Davidson Method constructor
  JD::JD(const DiracMatrix &mat, const DiracMatrix &matSloppy, const DiracMatrix &matPrecon, QudaEigParam *eig_param,
         TimeProfile &profile) :
    EigenSolver(mat, matSloppy, matPrecon, eig_param, profile)
  {
    if (eig_param->spectrum != QUDA_SPECTRUM_SR_EIG)
      errorQuda("Only smallest real spectrum type (SR) can be passed to the JD solver");

    bool profile_running = profile.isRunning(QUDA_PROFILE_INIT);
    if (!profile_running) profile.TPSTART(QUDA_PROFILE_INIT);

    corr_eq_tol = eig_param->corr_eq_tol;
    corr_eq_maxiter = eig_param->corr_eq_maxiter;

    // Additional auxiliar profilers used in the solvers for the correction equation
    profile_corr_eq_invs = new TimeProfile("profile_corr_eq_invs");
    profile_mat_corr_eq_invs = new TimeProfile("profile_mat_corr_eq_invs");

    QudaInvertParam refineparam = *eig_param->invert_param;
    refineparam.cuda_prec_sloppy = eig_param->invert_param->cuda_prec_refinement_sloppy;
    solverParam = new SolverParam(refineparam);
    solverParam->tol = corr_eq_tol;
    solverParam->maxiter = corr_eq_maxiter;
    solverParam->use_init_guess = QUDA_USE_INIT_GUESS_YES;
    solverParam->delta = eig_param->invert_param->reliable_delta_refinement;
    // disable preconditioning on solvers used in the correction equation
    solverParam->inv_type_precondition = QUDA_INVALID_INVERTER;

    solverParamPrec = new SolverParam(*solverParam);
    solverParamPrec->maxiter = 5;
    solverParamPrec->tol = 1e-2;

    d = nullptr;
    // dSloppy = nullptr;
    // Create the dirac operator
    // IMPORTANT: cannot call createDirac(...) as in interface_quda.cpp
    {
      bool pc_solve = ((eig_param->invert_param->solve_type == QUDA_DIRECT_PC_SOLVE)
                       || (eig_param->invert_param->solve_type == QUDA_NORMOP_PC_SOLVE)
                       || (eig_param->invert_param->solve_type == QUDA_NORMERR_PC_SOLVE));

      DiracParam diracParam;
      // DiracParam diracSloppyParam;

      quda::setDiracParam(diracParam, eig_param->invert_param, pc_solve);
      // quda::setDiracSloppyParam(diracSloppyParam, eig_param->invert_param, pc_solve);
      d = Dirac::create(diracParam);
      // dSloppy = Dirac::create(diracSloppyParam);
    }

    Dirac &dirac = *d;
    // Dirac &diracSloppy = *dSloppy;

    mmPP = new DiracPrecProjCorr(dirac);
    // alternative: the following two (commented) lines will depend on whether getStencilSteps() is virtual or not
    //Dirac *dirac_alt = (Dirac*)((void*) (&mat));
    //mmPP = new DiracPrecProjCorr(dirac_alt);
    mmPP = new DiracPrecProjCorr(dirac);

    // mmPPSloppy = new DiracPrecProjCorr(diracSloppy);

    // Solvers used in the correction equation
    cg = new CG(const_cast<DiracMatrix &>(mat), const_cast<DiracMatrix &>(mat), const_cast<DiracMatrix &>(mat),
                *solverParam, *profile_mat_corr_eq_invs);
    gcrPrec = new GCR(*mmPP, *mmPP, *mmPP, *solverParamPrec, *profile_corr_eq_invs);

    if (!profile_running) profile.TPSTOP(QUDA_PROFILE_INIT);
  }

  void JD::operator()(std::vector<ColorSpinorField *> &eigSpace, std::vector<Complex> &evals)
  {

    // TODO: general pendings:

    //		1. fix profiling !
    //		2. extend whole code to address any <target> (i.e. include LR, not only SR)

    //		3. avoid calls to <new> within JD::eigsolveInSubspace(...)
    //		4. optimize the eigendecomposition of the subspace, and in particular the use of Eigen
    //		   for this (this within JD::eigsolveInSubspace(...))
    //		5. avoid the mess being done for sorting Ritz eigenpairs after the eigendecomposition
    //		   (this within JD::eigsolveInSubspace(...))

    //		6. any more changes from camelCase ---> underscore_case needed ?
    //		7. is TRLM::precChangeKrylov(...) of any use in the context of low tol in the correction equation?

    // Check to see if we are loading eigenvectors
    if (strcmp(eig_param->vec_infile, "") != 0) {
      printfQuda("Loading evecs from file name %s\n", eig_param->vec_infile);
      loadFromFile(mat, eigSpace, evals);
      return;
    }

    // Setting some initial parameters of the eigensolver

    k = 0;
    k_max = eig_param->nConv;
    m = 0;
    m_max = eig_param->mmax;
    m_min = eig_param->mmin;
    theta = 0.0;

    // Test for an initial guess
    testInitGuess(eigSpace[0]);

    bool profile_running = profile.isRunning(QUDA_PROFILE_INIT);
    if (!profile_running) profile.TPSTART(QUDA_PROFILE_INIT);

    // Clone eigSpace's CSF params
    ColorSpinorParam csParam(*eigSpace[0]);

    // Some more memory pre-allocations
    moreInits(csParam, *eigSpace[0]);

    Complex *ort_dot_prod = (Complex *)safe_malloc(std::max(m_max, k_max) * 1 * sizeof(Complex));

    if (!profile_running) profile.TPSTOP(QUDA_PROFILE_INIT);

    // double epsilon = DBL_EPSILON;
    QudaPrecision prec = eigSpace[0]->Precision();
    switch (prec) {
    case QUDA_DOUBLE_PRECISION:
      // epsilon = DBL_EPSILON;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in double precision\n");
      break;
    case QUDA_SINGLE_PRECISION:
      // epsilon = FLT_EPSILON;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in single precision\n");
      break;
    case QUDA_HALF_PRECISION:
      // epsilon = 2e-3;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in half precision\n");
      break;
    case QUDA_QUARTER_PRECISION:
      // epsilon = 5e-2;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in quarter precision\n");
      break;
    default: errorQuda("Invalid precision %d", prec);
    }

    // Begin JD Eigensolver computation
    //---------------------------------------------------------------------------
    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("*****************************\n");
      printfQuda("**** START JD SOLUTION ******\n");
      printfQuda("*****************************\n");
    }

    // Print Eigensolver params
    if (getVerbosity() >= QUDA_VERBOSE) {
      printfQuda("spectrum %s\n", spectrum);
      printfQuda("tol %.4e\n", tol);
      printfQuda("nConv %d\n", nConv);
      printfQuda("mmin %d\n", m_min);
      printfQuda("mmax %d\n", m_max);
      printfQuda("corr-eq-maxiter %d\n", corr_eq_maxiter);
      printfQuda("corr-eq-tol %f\n", corr_eq_tol);
    }

    profile.TPSTART(QUDA_PROFILE_COMPUTE);

    // Matrix with the compressed sub-space information to extract the eigenpairs
    MatrixXcd H;
    SelfAdjointEigenSolver<MatrixXcd> eigensolver;

    loopr = 0;

    // Main loop
    while (restart_iter < max_restarts) {

      // Locking
      orth(ort_dot_prod, t, eigSpace, k);

      // Project t orthogonal to V: t = t - ( v_i^* . t ) . v_i
      orth(ort_dot_prod, t, V, m);

      m++;

      // Push: V[m-1] = t, and then normalize it
      blas::copy(*V[m - 1], *t[0]);
      norm = sqrt(blas::norm2(*V[m - 1]));
      blas::ax(1.0 / norm, *V[m - 1]);
      // and then apply A to that newly added vector
      matVec(mat, *V_A[m - 1], *V[m - 1]);

      // Perform the eigendecomposition in the acceleration subspace
      std::vector<std::pair<double, std::vector<Complex> *>> eigenpairs;
      eigsolveInSubspace(eigenpairs, &eigensolver, &H);

      // Computing the residual
      // u = V * s_1 -- lifting the first Ritz vector through V
      blas::zero(*u[0]);
      for (int i = 0; i < m; i++) {
        // use this due to frequent-shrinking-avoiding
        blas::caxpy((*(eigenpairs[loopr].second))[i], *V[i], *u[0]);
      }
      // FIXME :  change this matmul for the superposition ?
      matVec(mat, *u_A[0], *u[0]);
      // and then compute the residual
      theta = eigenpairs[loopr].first;
      blas::copy(*r[0], *u_A[0]);
      blas::caxpy(-theta, *u[0], *r[0]);

      norm = sqrt(blas::norm2(*r[0]));
      // Print the residual
      if (getVerbosity() >= QUDA_SUMMARIZE) {
        printfQuda("Iteration = %5d, m = %3d, loopr = %d, residual = %.12f, converged eigenpairs = %4d\n", iter, m, loopr,
                   norm, k);
      }

      // Check for convergence
      checkIfConverged(eigenpairs, eigSpace, csParam, evals);

      // Check if end has been reached
      if (k >= k_max) { break; }

      // Restart: shrink the acceleration subspace
      if (m == m_max) { shrinkSubspace(eigenpairs, csParam, &H); }

      // Solving the correction equation
      blas::copy(*t[0], *r[0]);
      {
        invertProjMat(matPrecon, *t[0], *r[0], QUDA_SILENT, 1, u);
      }

      // Some local clean-up
      for (auto p : eigenpairs) { delete p.second; }

      iter++;
    }

    //--------------------

    profile.TPSTOP(QUDA_PROFILE_COMPUTE);

    if (getVerbosity() >= QUDA_DEBUG_VERBOSE)
      printfQuda("eigSpace size at convergence/max restarts = %d\n", (int)eigSpace.size());

    // Post computation report
    //---------------------------------------------------------------------------
    if (!converged) {
      if (eig_param->require_convergence) {
        errorQuda("JD failed to compute the requested %d vectors with a search space of size between %d and %d in %d "
                  "restart steps. Exiting.",
                  nConv, m_min, m_max, max_restarts);
      } else {
        warningQuda("JD failed to compute the requested %d vectors with a search space of size between %d and %d in %d "
                    "restart steps.",
                    nConv, m_min, m_max, max_restarts);
      }
    } else {
      if (getVerbosity() >= QUDA_SUMMARIZE) {
        printfQuda("JD computed the requested %d vectors in %d restart steps and %d iterations.\n", nConv, restart_iter,
                   iter);
      }

      // Compute eigenvalues
      computeEvals(mat, eigSpace, evals);
    }

    // Local clean-up
    delete r[0];
    delete t[0];
    delete r_tilde[0];
    delete mmPP->y_hat[0];
    for (auto p : u) { delete p; }
    for (auto p : u_A) { delete p; }
    for (auto p : V) { delete p; }
    for (auto p : V_A) { delete p; }
    for (auto p : tmpV) { delete p; }
    for (auto p : tmpAV) { delete p; }
    for (auto p : Qhat) { delete p; }
    host_free(ort_dot_prod);

    // Only save if outfile is defined
    if (strcmp(eig_param->vec_outfile, "") != 0) {
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("saving eigenvectors\n");
      // Make an array of size nConv
      std::vector<ColorSpinorField *> vecs_ptr;
      vecs_ptr.reserve(nConv);
      const QudaParity mat_parity = impliedParityFromMatPC(mat.getMatPCType());
      for (int i = 0; i < nConv; i++) {
        eigSpace[i]->setSuggestedParity(mat_parity);
        vecs_ptr.push_back(eigSpace[i]);
      }
      saveVectors(vecs_ptr, eig_param->vec_outfile);
    }

    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("*****************************\n");
      printfQuda("***** END JD SOLUTION *******\n");
      printfQuda("*****************************\n");
    }

    mat.flops();
  }

  // Jacobi-Davidson destructor
  JD::~JD()
  {
    delete profile_corr_eq_invs;
    delete profile_mat_corr_eq_invs;

    delete solverParam;
    delete solverParamPrec;
    delete gcrPrec;
    delete cg;

    delete d;
    delete mmPP;
  }

  // JD Member functions
  //---------------------------------------------------------------------------

  void JD::invertProjMat(const DiracMatrix &matPrecon, ColorSpinorField &x, ColorSpinorField &b, QudaVerbosity verb,
                         const int kp, std::vector<ColorSpinorField *> &projSpace)
  {
    std::vector<ColorSpinorField *> &qSpace = projSpace;

    // Clone x's CSF params
    ColorSpinorParam csParam(x);

    // Buffers for the shifts of the matrix operators
    double bare_shift;

    // 1. solve for Qhat in K * Qhat = qSpace, with K a good (but 'cheap') preconditioner

    int size_ps = kp;

    // Casting away contractual constness
    DiracMatrix &mat_unconst = const_cast<DiracMatrix &>(matPrecon);

    csParam.create = QUDA_COPY_FIELD_CREATE;
    //---------------------------------------------
    // Switching to the appropriate shift for JD
    bare_shift = mat_unconst.shift;
    mat_unconst.shift = bare_shift - theta;

    if (size_ps > 1) {
      for (int i = 0; i < size_ps; i++) {
        blas::copy(*Qhat[i], *qSpace[i]);
        K(*cg, corr_eq_tol, corr_eq_maxiter, QUDA_SILENT, *solverParam, *Qhat[i], *qSpace[i]);
      }
    } else {
      blas::copy(*Qhat[0], *qSpace[0]);
      K(*cg, corr_eq_tol, corr_eq_maxiter, QUDA_SILENT, *solverParam, *Qhat[0], *qSpace[0]);
    }

    // and, switching back the shift parameters
    mat_unconst.shift = bare_shift;
    //---------------------------------------------

    // 2. M = qSpacedag * Qhat

    MatrixXcd M = MatrixXcd::Zero(size_ps, size_ps);
    if (size_ps > 1) {
      std::vector<ColorSpinorField *> tmpQhat(Qhat.begin(), Qhat.begin() + size_ps);
      std::vector<ColorSpinorField *> tmpQSpace(qSpace.begin(), qSpace.begin() + size_ps);

      Complex *result_dot_prod = (Complex *)safe_malloc(size_ps * size_ps * sizeof(Complex));
      blas::cDotProduct(result_dot_prod, const_cast<std::vector<ColorSpinorField *> &>(tmpQSpace),
                        const_cast<std::vector<ColorSpinorField *> &>(tmpQhat));

      // 3. LU decomposition of M -- TODO: move the application of .fullPivLu() to this sub-section ?

      // Init eigen object
      for (int i = 0; i < size_ps; i++) {
        for (int j = 0; j < size_ps; j++) { M(i, j) = result_dot_prod[i * size_ps + j]; }
      }

      // Local clean-up
      host_free(result_dot_prod);
    } else {
      Complex result_dot_prod = blas::cDotProduct(*qSpace[0], *Qhat[0]);
      M(0, 0) = result_dot_prod;
    }

    // 4. r_tilde = Ktilde^-1 * r

    //---------------------------------------------
    // Switching to the appropriate shift for JD
    bare_shift = mat_unconst.shift;
    mat_unconst.shift = bare_shift - theta;

    // <r_tilde> is "r-hat" for a few of the upcoming lines

    blas::copy(*r_tilde[0], b);
    K(*cg, corr_eq_tol, corr_eq_maxiter, QUDA_SILENT, *solverParam, *r_tilde[0], b);

    // and, switching back the shift parameters
    mat_unconst.shift = bare_shift;
    //---------------------------------------------

    if (size_ps > 1) {
      MatrixXcd gamma(size_ps, 1);
      for (int i = 0; i < size_ps; i++) { gamma(i, 0) = blas::cDotProduct(*qSpace[i], *r_tilde[0]); }
      MatrixXcd alpha = M.fullPivLu().solve(gamma);
      for (int i = 0; i < size_ps; i++) { blas::caxpy(-alpha(i), *Qhat[i], *r_tilde[0]); }
    } else {
      Complex gamma = blas::cDotProduct(*qSpace[0], *r_tilde[0]);
      Complex alpha = gamma / M(0, 0);
      blas::caxpy(-alpha, *Qhat[0], *r_tilde[0]);
    }

    blas::ax(-1.0, *r_tilde[0]);

    // 5. solve: ( Ktilde^-1 * (I - QQdag)(A - \theta I)(I - QQdag) ) x = r_tilde

    // TODO: move some of the following assignments to the beginning of JD::operator()
    mmPP->projSpace = qSpace;
    mmPP->theta = theta;
    mmPP->Mproj = M;
    mmPP->Qhat = Qhat;
    mmPP->solverParam_ = solverParam;
    mmPP->matUnconst_ = &mat_unconst;
    mmPP->cg_ = cg;
    mmPP->eigSlvr = this;
    mmPP->k = size_ps;
    mmPP->tol = corr_eq_tol;
    mmPP->maxiter = corr_eq_maxiter;

    blas::zero(x);
    QudaVerbosity verbTmp = getVerbosity();
    setVerbosity(QUDA_SILENT);
    (*gcrPrec)(x, *r_tilde[0]);
    setVerbosity(verbTmp);
  }

  void JD::K(CG &cgw, double tol, int maxiter, QudaVerbosity verb, SolverParam &slvrPrm, ColorSpinorField &x,
             ColorSpinorField &b)
  {
    QudaVerbosity verbTmp = getVerbosity();
    setVerbosity(verb);

    double tol_buff = slvrPrm.tol;
    slvrPrm.tol = tol;
    int maxiter_buff = slvrPrm.maxiter;
    slvrPrm.maxiter = maxiter;

    cgw(x, b);

    slvrPrm.tol = tol_buff;
    slvrPrm.maxiter = maxiter_buff;

    setVerbosity(verbTmp);
  }

  void JD::moreInits(ColorSpinorParam &csParam, ColorSpinorField &initVec)
  {
    // Init a zero residual
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    r.push_back(ColorSpinorField::Create(csParam));

    r_tilde.push_back(ColorSpinorField::Create(csParam));
    mmPP->y_hat.push_back(ColorSpinorField::Create(csParam));

    // We're only using one vector to project in the correction equation
    // Qhat.reserve(k_max+1);
    // for (int i=0; i<(k_max+1); i++) {Qhat.push_back(ColorSpinorField::Create(csParam));}
    Qhat.push_back(ColorSpinorField::Create(csParam));

    csParam.create = QUDA_COPY_FIELD_CREATE;
    t.push_back(ColorSpinorField::Create(initVec, csParam));

    // Create the vector subspaces used for the search of eigenpairs
    // buffer spinors
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    u.push_back(ColorSpinorField::Create(csParam));
    u_A.push_back(ColorSpinorField::Create(csParam));

    for (int i = 0; i < m_max; i++) {
      V.push_back(ColorSpinorField::Create(csParam));
      V_A.push_back(ColorSpinorField::Create(csParam));
    }

    for (int i = 0; i < m_max; i++) {
      tmpV.push_back(ColorSpinorField::Create(csParam));
      tmpAV.push_back(ColorSpinorField::Create(csParam));
    }
  }

  void JD::orth(Complex *ort_dot_prod, std::vector<ColorSpinorField *> &vectr, std::vector<ColorSpinorField *> &ort_space,
                const int size_os)
  {
    int tmp_size = size_os;

    if (tmp_size == 0) { return; }

    std::vector<ColorSpinorField *> tmp_ort_space(ort_space.begin(), ort_space.begin() + tmp_size);

    // double orthogonalization
    for (int j = 0; j < 2; j++) {
      // normalize before the orthogonalization
      norm = sqrt(blas::norm2(*vectr[0]));
      blas::ax(1.0 / norm, *vectr[0]);

      blas::cDotProduct(ort_dot_prod, tmp_ort_space, vectr);
      for (int i = 0; i < tmp_size; i++) { ort_dot_prod[i] *= -1.0; }
      blas::caxpy(ort_dot_prod, tmp_ort_space, vectr);
    }
  }

  void JD::checkIfConverged(std::vector<std::pair<double, std::vector<Complex> *>> &eigenpairs,
                            std::vector<ColorSpinorField *> &X_tilde, ColorSpinorParam &csParam,
                            std::vector<Complex> &evals)
  {
    csParam.create = QUDA_COPY_FIELD_CREATE;

    while (norm < eig_param->tol) {
      // evals.push_back(eigenpairs[loopr].first);
      evals[k] = eigenpairs[loopr].first;
      blas::copy(*X_tilde[k], *u[0]);
      k++;

      // Check for convergence
      if (k >= k_max) {
        converged = true;
        break;
      }

      blas::zero(*u[0]);
      // project-up with a subspace still of size m
      for (int j = 0; j < m; j++) { blas::caxpy((*(eigenpairs[loopr + 1].second))[j], *V[j], *u[0]); }
      // FIXME :  change this matmul for the superposition ?
      matVec(mat, *u_A[0], *u[0]);

      // and then compute the residual
      theta = eigenpairs[loopr + 1].first;
      blas::copy(*r[0], *u_A[0]);
      blas::caxpy(-theta, *u[0], *r[0]);

      norm = sqrt(blas::norm2(*r[0]));

      loopr++;
    }
  }

  void JD::shrinkSubspace(std::vector<std::pair<double, std::vector<Complex> *>> &eigenpairs, ColorSpinorParam &csParam,
                          void *H_)
  {
    // TODO:
    //		1. add exceptions for case: (loopr+m_min) > m_max
    //		2. is it possible to avoid resizing H?

    MatrixXcd &H = *((MatrixXcd *)H_);

    H.resize(m_min, m_min);
    H.setZero();

    // 0-th element of the new (smaller) subspace
    csParam.create = QUDA_COPY_FIELD_CREATE;
    blas::copy(*tmpV[0], *u[0]);
    blas::copy(*tmpAV[0], *u_A[0]);
    H(0, 0) = eigenpairs[loopr].first;

    csParam.create = QUDA_ZERO_FIELD_CREATE;
    for (int i = 1; i < m_min; i++) {
      blas::zero(*tmpV[i]);
      for (int j = 0; j < m; j++) {
        blas::caxpy((*(eigenpairs[loopr + i].second))[j], *V[j], *tmpV[i]);
        // H(i,i) = eigenpairs[loopr+i].first;
      }
      H(i, i) = eigenpairs[loopr + i].first;
      // FIXME :  change this matmul for the superposition ?
      matVec(mat, *tmpAV[i], *tmpV[i]);
    }

    m = m_min;

    // Assign new values of V and W
    for (int i = 0; i < m; i++) {
      ColorSpinorField *tmpf = V[i];
      V[i] = tmpV[i];
      tmpV[i] = tmpf;
      tmpf = V_A[i];
      V_A[i] = tmpAV[i];
      tmpAV[i] = tmpf;
    }

    restart_iter++;

    if (getVerbosity() >= QUDA_SUMMARIZE) { printfQuda("RESTART (#%d)\n", restart_iter); }

    loopr = 0;

    theta = H(0, 0).real();
  }

  void JD::eigsolveInSubspace(std::vector<std::pair<double, std::vector<Complex> *>> &eigenpairs, void *eigensolver_,
                              void *H_)
  {
    SelfAdjointEigenSolver<MatrixXcd> &eigensolver = *((SelfAdjointEigenSolver<MatrixXcd> *)(eigensolver_));
    MatrixXcd &H = *((MatrixXcd *)H_);

    // Construction of H = Vdag . V_A
    H.conservativeResize(m, m);
    for (int i = 0; i < (m - 1); i++) {
      H(i, m - 1) = blas::cDotProduct(*V[i], *V_A[m - 1]);
      // is the next line necessary ?
      H(m - 1, i) = conj(H(i, m - 1));
    }
    // this next line ensures the case m=1
    H(m - 1, m - 1) = blas::cDotProduct(*V[m - 1], *V_A[m - 1]);

    // ith eigenvalue: eigensolver.eigenvalues()[i], ith eigenvector: eigensolver.eigenvectors().col(i)
    eigensolver.compute(H);

    // Moving the eigenpairs to a vector of std::pair to sort by eigenvalue
    std::vector<Complex> *buffVec = 0;
    for (int i = 0; i < m; i++) {
      buffVec
        = new std::vector<Complex>(eigensolver.eigenvectors().col(i).data(),
                                   eigensolver.eigenvectors().col(i).data() + eigensolver.eigenvectors().col(i).size());
      eigenpairs.push_back(std::make_pair(eigensolver.eigenvalues()[i], buffVec));
    }

    // Order the eigeninformation extracted from H in descending order of eigenvalues
    // TODO: switch to using a sort-function, due to general values of <tau>. Descending
    //       order is only applicable to the case tau=0 i.e. smallest eigenvalues
    std::sort(eigenpairs.begin(), eigenpairs.end());
  }

  // Projection matrix used in the solution of the correction equation
  //---------------------------------------------------------------------------

  void matCorrEq(ColorSpinorField &out, const ColorSpinorField &in, const DiracPrecProjCorr &mmPP)
  {
    double norm_bf = sqrt(blas::norm2(in));

    if (norm_bf == 0) {
      if (getVerbosity() >= QUDA_SUMMARIZE)
        warningQuda("Received a zero spinor in DiracPrecProjCorr::operator() within JD");
      blas::zero(out);
      return;
    }

    // unpacking some attributes
    SolverParam &solverParam = *(mmPP.solverParam_);
    DiracMatrix &matUnconst = *(mmPP.matUnconst_);
    CG &cgx = *(mmPP.cg_);

    // 1. y = (A - \theta I)v

    matUnconst(out, in);
    blas::caxpy(-mmPP.theta, const_cast<ColorSpinorField &>(in), out);

    // 2. y_hat = K^-1 * y

    blas::copy(*(mmPP.y_hat[0]), out);

    //---------------------------------------------
    // Switching to the appropriate shift for JD
    double bare_shift = matUnconst.shift;
    matUnconst.shift = bare_shift - mmPP.theta;

    (mmPP.eigSlvr)->K(cgx, mmPP.tol, mmPP.maxiter, QUDA_SILENT, solverParam, *(mmPP.y_hat[0]), out);

    // Switching back the shift parameters
    matUnconst.shift = bare_shift;
    //---------------------------------------------

    int size_ps = mmPP.k;

    if (size_ps > 1) {
      Eigen::MatrixXcd gamma(size_ps, 1);
      for (int i = 0; i < size_ps; i++) { gamma(i, 0) = blas::cDotProduct(*(mmPP.projSpace[i]), *(mmPP.y_hat[0])); }
      Eigen::MatrixXcd alpha = (mmPP.Mproj).fullPivLu().solve(gamma);
      for (int i = 0; i < size_ps; i++) { blas::caxpy(-alpha(i), *(mmPP.Qhat[i]), *(mmPP.y_hat[0])); }
    } else {
      Complex gamma = blas::cDotProduct(*(mmPP.projSpace[0]), *(mmPP.y_hat[0]));
      Complex alpha = gamma / (mmPP.Mproj(0, 0));
      blas::caxpy(-alpha, *(mmPP.Qhat[0]), *(mmPP.y_hat[0]));
    }

    // out = *y_hat[0];
    blas::copy(out, *(mmPP.y_hat[0]));
  }

  void DiracPrecProjCorr::operator()(ColorSpinorField &out, const ColorSpinorField &in) const
  {
    matCorrEq(out, in, *this);
  }

  void DiracPrecProjCorr::operator()(ColorSpinorField &out, const ColorSpinorField &in, ColorSpinorField &tmp) const
  {
    dirac->tmp1 = &tmp;
    matCorrEq(out, in, *this);
    dirac->tmp1 = NULL;
  }

  void DiracPrecProjCorr::operator()(ColorSpinorField &out, const ColorSpinorField &in, ColorSpinorField &Tmp1,
                                     ColorSpinorField &Tmp2) const
  {
    dirac->tmp1 = &Tmp1;
    dirac->tmp2 = &Tmp2;
    matCorrEq(out, in, *this);
    dirac->tmp2 = NULL;
    dirac->tmp1 = NULL;
  }
} // namespace quda