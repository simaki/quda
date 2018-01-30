/* C. Kallidonis: Header file for the qlua-interface
 * lib/interface_qlua.cpp
 */

#ifndef INTERFACE_QLUA_H__
#define INTERFACE_QLUA_H__

#ifndef LONG_T
#define LONG_T long long
#endif
typedef double QUDA_REAL;

#ifdef __cplusplus

#define XTRN_CPLX std::complex<QUDA_REAL>

#define EXTRN_C extern "C"

#define delete_not_null(p) do { if (NULL != (p)) delete (p) ; } while(0)

#else /*!defined(__cplusplus)*/
#include <complex.h>
#define EXTRN_C
#define XTRN_CPLX double complex
#endif/*__cplusplus*/

#define QUDA_Nc 3
#define QUDA_Ns 4
#define QUDA_DIM 4
#define QUDA_MAX_RANK 6
#define QUDA_PROP_NVEC 12
#define QUDA_TIME_AXIS 3
#define QUDA_LEN_G (QUDA_Ns*QUDA_Ns)

#define PI 2*asin(1.0)
#define THREADS_PER_BLOCK 64


//#include <complex_quda.h>


typedef enum qudaAPI_ContractId_s{
  cntr12 = 12,
  cntr13 = 13,
  cntr14 = 14,
  cntr23 = 23,
  cntr24 = 24,
  cntr34 = 34,
  cntr_INVALID = 0
} qudaAPI_ContractId;

struct qudaLattice_s { 
  int node;
  int rank;
  int net[QUDA_MAX_RANK];
  int net_coord[QUDA_MAX_RANK];
  /* local volume : lo[mu] <= x[mu] < hi[mu] */
  int site_coord_lo[QUDA_MAX_RANK];
  int site_coord_hi[QUDA_MAX_RANK];
  LONG_T locvol;
  LONG_T *ind_qdp2quda;
};
typedef struct qudaLattice_s qudaLattice;

typedef struct {
  QUDA_REAL alpha[QUDA_MAX_RANK];
  QUDA_REAL beta;
  int Nstep;
  QudaVerbosity verbosity;
} wuppertalParam;

typedef struct {
  int nVec;
  qudaAPI_ContractId cntrID;
} contractParam;

typedef struct {
  int QsqMax;
  int Nmoms;
  int Ndata;
  int expSgn;
  int GPU_phaseMatrix;
  int momDim;
  int V3;
  int tAxis;
  int Tdim;
  double bc_t;
  int csrc[QUDA_DIM];
  LONG_T locvol;
  int push_res;
} momProjParam;

typedef struct {
  QudaVerbosity verbosity;
  wuppertalParam wParam;
  contractParam cParam;
  momProjParam mpParam;
} qudaAPI_Param;

typedef struct{
  QUDA_REAL re;
  QUDA_REAL im;
} QLUA_COMPLEX;

typedef enum RotateType_s {
  QLUA_qdp2quda = 1,
  QLUA_quda2qdp = 2
} RotateType;


EXTRN_C
QudaVerbosity parseVerbosity(const char *v);

EXTRN_C
qudaAPI_ContractId parseContractIdx(const char *v);

EXTRN_C int
doQQ_contract_Quda(
		   QUDA_REAL *hprop_out,
		   QUDA_REAL *hprop_in1,
		   QUDA_REAL *hprop_in2,
		   const qudaLattice *qS,
		   int nColor, int nSpin,
		   const qudaAPI_Param qAparam);

EXTRN_C int
momentumProjectionPropagator_Quda(
				  QUDA_REAL *corrOut,
				  QUDA_REAL *corrIn,
				  const qudaLattice *qS,
				  qudaAPI_Param paramAPI);

EXTRN_C int
laplacianQuda(
	      QUDA_REAL *hv_out,
	      QUDA_REAL *hv_in,
	      QUDA_REAL *h_gauge[],
	      const qudaLattice *qS,
	      int nColor, int nSpin,
	      const qudaAPI_Param qAparam);


EXTRN_C int
Qlua_invertQuda(
		QUDA_REAL *hv_out,
		QUDA_REAL *hv_in,
		QUDA_REAL *h_gauge[],
		const qudaLattice *qS,
		int nColor, int nSpin,
		qudaAPI_Param qAparam);

EXTRN_C int
baryon_sigma_twopt_asymsrc_gvec_momProj_Quda(XTRN_CPLX *momproj_buf, XTRN_CPLX *corrPosSpc, const qudaLattice *qS, const int *momlist,
                                             QUDA_REAL *hprop1, QUDA_REAL *hprop2, QUDA_REAL *hprop3,
                                             XTRN_CPLX *S2, XTRN_CPLX *S1,
                                             int Nc, int Ns, qudaAPI_Param paramAPI);

#endif/*INTERFACE_QLUA_H__*/
