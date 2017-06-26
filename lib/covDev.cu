#include <transfer.h>
#include <gauge_field_order.h>
#include <color_spinor_field_order.h>
#include <index_helper.cuh>
#include <stencil.h>
#include <color_spinor.h>

/**
   This is the covariant derivative based on the basic gauged Laplace operator
*/

namespace quda {

  /**
     @brief Parameter structure for driving the covariant derivative
   */
  template <typename Float, int nColor, QudaReconstructType reconstruct>
  struct CovDevArg {
    typedef typename colorspinor_order_mapper<Float,QUDA_FLOAT2_FIELD_ORDER,1,nColor>::type F;
    typedef typename gauge_mapper<Float,reconstruct>::type G;

    F out;                // output vector field
    const F in;           // input vector field
    const G U;            // the gauge field
    const int parity;     // only use this for single parity fields
    const int nParity;    // number of parities we're working on
    const int nFace;      // hard code to 1 for now
    const int dim[4];     // full lattice dimensions
    const int commDim[4]; // whether a given dimension is partitioned or not
    const int volumeCB;   // checkerboarded volume
    const int mu;         // direction of the covariant derivative

    CovDevArg(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U, const int parity, const int mu)
      : out(out), in(in), U(U), parity(parity), mu(mu), nParity(in.SiteSubset()), nFace(1),
	dim{ (3-nParity) * in.X(0), in.X(1), in.X(2), in.X(3) },
      commDim{comm_dim_partitioned(0), comm_dim_partitioned(1), comm_dim_partitioned(2), comm_dim_partitioned(3)},
      volumeCB(in.VolumeCB())
    {
      if (in.FieldOrder() != QUDA_FLOAT2_FIELD_ORDER || !U.isNative())
      errorQuda("Unsupported field order colorspinor=%d gauge=%d combination\n", in.FieldOrder(), U.FieldOrder());
    }
  };

  /**
     Applies the off-diagonal part of the Laplace operator

     @param[out] out The out result field
     @param[in] U The gauge field
     @param[in] kappa Kappa value
     @param[in] in The input field
     @param[in] parity The site parity
     @param[in] x_cb The checkerboarded site index
   */
  extern __shared__ float s[];
  template <typename Float, int nDim, int nColor, typename Vector, typename Arg>
  __device__ __host__ inline void applyCovDev(Vector &out, Arg &arg, int x_cb, int parity) {
    typedef Matrix<complex<Float>,nColor> Link;
    const int their_spinor_parity = (arg.nParity == 2) ? 1-parity : 0;

    int coord[4];
    getCoords(coord, x_cb, arg.dim, parity);

    const int d = arg.mu%4;

    if (arg.mu >= 4) {

      //Forward gather - compute fwd offset for vector fetch
      const int fwd_idx = linkIndexP1(coord, arg.dim, d);

      if ( arg.commDim[d] && (coord[d] + arg.nFace >= arg.dim[d]) ) {
	const int ghost_idx = ghostFaceIndex<1>(coord, arg.dim, d, arg.nFace);

	const Link U = arg.U(d, x_cb, parity);
#if 0 // FIXME - why is this slow?
	const Vector in = arg.in.Ghost(d, 1, ghost_idx, their_spinor_parity);
#else
	const Vector in;
	arg.in.loadGhost((Float*)in.data, ghost_idx, d, 1, their_spinor_parity);
#endif
	out += U * in;
	} else {

	const Link U = arg.U(d, x_cb, parity);
	const Vector in = arg.in(fwd_idx, their_spinor_parity);

	out += U * in;
      }
    } else {
      //Backward gather - compute back offset for spinor and gauge fetch
      const int back_idx = linkIndexM1(coord, arg.dim, d);
      const int gauge_idx = back_idx;

      if ( arg.commDim[d] && (coord[d] - arg.nFace < 0) ) {
	const int ghost_idx = ghostFaceIndex<0>(coord, arg.dim, d, arg.nFace);

	const Link U = arg.U.Ghost(d, ghost_idx, 1-parity);
#if 0 // FIXME - why is this slow?
	const Vector in = arg.in.Ghost(d, 0, ghost_idx, their_spinor_parity);
#else
	const Vector in;
	arg.in.loadGhost((Float*)in.data, ghost_idx, d, 0, their_spinor_parity);
#endif
	out += conj(U) * in;
      } else {
	
	const Link U = arg.U(d, gauge_idx, 1-parity);
	const Vector in = arg.in(back_idx, their_spinor_parity);

	out += conj(U) * in;
      }
    } // Forward/backward derivative

  }


  //out(x) = M*in
  template <typename Float, int nDim, int nColor, typename Arg>
  __device__ __host__ inline void covDev(Arg &arg, int x_cb, int parity)
  {
    typedef ColorSpinor<Float,nColor,1> Vector;
    Vector out;

    applyCovDev<Float,nDim,nColor>(out, arg, x_cb, parity);
    arg.out(x_cb, parity) = out;
  }

  // CPU kernel for applying the Laplace operator to a vector
  template <typename Float, int nDim, int nColor, typename Arg>
  void covDevCPU(Arg arg)
  {

    for (int parity= 0; parity < arg.nParity; parity++) {
      // for full fields then set parity from loop else use arg setting
      parity = (arg.nParity == 2) ? parity : arg.parity;

      for (int x_cb = 0; x_cb < arg.volumeCB; x_cb++) { // 4-d volume
	covDev<Float,nDim,nColor>(arg, x_cb, parity);
      } // 4-d volumeCB
    } // parity

  }

  // GPU Kernel for applying the Laplace operator to a vector
  template <typename Float, int nDim, int nColor, typename Arg>
  __global__ void covDevGPU(Arg arg)
  {
    int x_cb = blockIdx.x*blockDim.x + threadIdx.x;

    // for full fields set parity from y thread index else use arg setting
    int parity = blockDim.y*blockIdx.y + threadIdx.y;

    if (x_cb >= arg.volumeCB) return;
    if (parity >= arg.nParity) return;

    covDev<Float,nDim,nColor>(arg, x_cb, parity);
  }

  template <typename Float, int nDim, int nColor, typename Arg>
  class CovDev : public TunableVectorY {

  protected:
    Arg &arg;
    const ColorSpinorField &meta;

    long long flops() const
    {
      return 8*nColor*nColor*arg.nParity*(long long)meta.VolumeCB();
    }
    long long bytes() const
    {
      return arg.out.Bytes() + arg.in.Bytes() + arg.nParity*arg.U.Bytes()*meta.VolumeCB();
    }
    bool tuneGridDim() const { return false; }
    unsigned int minThreads() const { return arg.volumeCB; }
    unsigned int maxBlockSize() const { return deviceProp.maxThreadsPerBlock / arg.nParity; }

  public:
    CovDev(Arg &arg, const ColorSpinorField &meta) : TunableVectorY(arg.nParity), arg(arg), meta(meta)
    {
      strcpy(aux, meta.AuxString());
#ifdef MULTI_GPU
      char comm[5];
      comm[0] = (arg.commDim[0] ? '1' : '0');
      comm[1] = (arg.commDim[1] ? '1' : '0');
      comm[2] = (arg.commDim[2] ? '1' : '0');
      comm[3] = (arg.commDim[3] ? '1' : '0');
      comm[4] = '\0';
      strcat(aux,",comm=");
      strcat(aux,comm);
#endif
    }
    virtual ~CovDev() { }

    void apply(const cudaStream_t &stream) {
      if (meta.Location() == QUDA_CPU_FIELD_LOCATION) {
	covDevCPU<Float,nDim,nColor>(arg);
      } else {
        TuneParam tp = tuneLaunch(*this, getTuning(), getVerbosity());
	covDevGPU<Float,nDim,nColor> <<<tp.grid,tp.block,tp.shared_bytes,stream>>>(arg);
      }
    }

    TuneKey tuneKey() const { return TuneKey(meta.VolString(), typeid(*this).name(), aux); }
  };


  template <typename Float, int nColor, QudaReconstructType recon>
    void ApplyCovDev(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U, int parity, int mu)
  {
    constexpr int nDim = 4;
      CovDevArg<Float,nColor,recon> arg(out, in, U, parity, mu);
      CovDev<Float,nDim,nColor,CovDevArg<Float,nColor,recon> > myCovDev(arg, in);
      myCovDev.apply(0);
  }

  // template on the gauge reconstruction
  template <typename Float, int nColor>
    void ApplyCovDev(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U, int parity, int mu)
  {
    if (U.Reconstruct()== QUDA_RECONSTRUCT_NO) {
      ApplyCovDev<Float,nColor,QUDA_RECONSTRUCT_NO>(out, in, U, parity, mu);
    } else if (U.Reconstruct()== QUDA_RECONSTRUCT_12) {
      ApplyCovDev<Float,nColor,QUDA_RECONSTRUCT_12>(out, in, U, parity, mu);
    } else if (U.Reconstruct()== QUDA_RECONSTRUCT_8) {
      ApplyCovDev<Float,nColor,QUDA_RECONSTRUCT_8> (out, in, U, parity, mu);
    } else {
      errorQuda("Unsupported reconstruct type %d\n", U.Reconstruct());
    }
  }

  // template on the number of colors
  template <typename Float>
    void ApplyCovDev(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U, int parity, int mu)
  {
    if (in.Ncolor() == 3) {
      ApplyCovDev<Float,3>(out, in, U, parity, mu);
    } else {
      errorQuda("Unsupported number of colors %d\n", U.Ncolor());
    }
  }

  // this is the Worker pointer that may have issue additional work
  // while we're waiting on communication to finish
  namespace dslash {
    extern Worker* aux_worker;
  }

  //Apply the covariant derivative operator
  //out(x) = U_{\mu}(x)in(x+mu) for mu = 0...3
  //out(x) = U^\dagger_mu'(x-mu')in(x-mu') for mu = 4...7 and we set mu' = mu-4
  void ApplyCovDev(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U, int parity, int mu)		    
  {
    if (in.V() == out.V()) errorQuda("Aliasing pointers");
    if (out.Precision() != in.Precision() || U.Precision() != in.Precision())
      errorQuda("Precision mismatch out=%d in=%d U=%d", out.Precision(), in.Precision(), U.Precision());
    if (in.FieldOrder() != out.FieldOrder())
      errorQuda("Field order mismatch in = %d, out = %d", in.FieldOrder(), out.FieldOrder());
    
    // check all locations match
    Location(out, in, U);

    const int nFace = 1;
    in.exchangeGhost((QudaParity)(1-parity), nFace, 0); // last parameter is dummy

    if (dslash::aux_worker) dslash::aux_worker->apply(0);

    if (U.Precision() == QUDA_DOUBLE_PRECISION) {
      ApplyCovDev<double>(out, in, U, parity, mu);
    } else if (U.Precision() == QUDA_SINGLE_PRECISION) {
      ApplyCovDev<float>(out, in, U, parity, mu);
    } else {
      errorQuda("Unsupported precision %d\n", U.Precision());
    }
  }


} // namespace quda
