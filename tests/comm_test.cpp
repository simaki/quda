#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>

// QUDA headers
#include <quda.h>
#include <color_spinor_field.h> // convenient quark field container

// External headers
#include <misc.h>
#include <host_utils.h>
#include <command_line_params.h>
#include <dslash_reference.h>

#include <communicator_quda.h>

#include <gauge_field.h>

#include <timer.h>

#include <gauge_tools.h>

#include <gtest/gtest.h>
#include <blas_quda.h>

static quda::TimeProfile profileExtendedGauge("createExtendedGaugeFieldCommTest");

extern quda::cudaGaugeField *gaugePrecise;

#define MAX(a, b) ((a) > (b) ? (a) : (b))

void display_test_info()
{
  printfQuda("running the following test:\n");
  printfQuda("prec    prec_sloppy   multishift  matpc_type  recon  recon_sloppy solve_type S_dimension T_dimension "
             "Ls_dimension   dslash_type  normalization\n");
  printfQuda(
    "%6s   %6s          %d     %12s     %2s     %2s         %10s %3d/%3d/%3d     %3d         %2d       %14s  %8s\n",
    get_prec_str(prec), get_prec_str(prec_sloppy), multishift, get_matpc_str(matpc_type), get_recon_str(link_recon),
    get_recon_str(link_recon_sloppy), get_solve_str(solve_type), xdim, ydim, zdim, tdim, Lsdim,
    get_dslash_str(dslash_type), get_mass_normalization_str(normalization));

  if (inv_multigrid) {
    printfQuda("MG parameters\n");
    printfQuda(" - number of levels %d\n", mg_levels);
    for (int i = 0; i < mg_levels - 1; i++) {
      printfQuda(" - level %d number of null-space vectors %d\n", i + 1, nvec[i]);
      printfQuda(" - level %d number of pre-smoother applications %d\n", i + 1, nu_pre[i]);
      printfQuda(" - level %d number of post-smoother applications %d\n", i + 1, nu_post[i]);
    }

    printfQuda("MG Eigensolver parameters\n");
    for (int i = 0; i < mg_levels; i++) {
      if (low_mode_check || mg_eig[i]) {
        printfQuda(" - level %d solver mode %s\n", i + 1, get_eig_type_str(mg_eig_type[i]));
        printfQuda(" - level %d spectrum requested %s\n", i + 1, get_eig_spectrum_str(mg_eig_spectrum[i]));
        if (mg_eig_type[i] == QUDA_EIG_BLK_TR_LANCZOS)
          printfQuda(" - eigenvector block size %d\n", mg_eig_block_size[i]);
        printfQuda(" - level %d number of eigenvectors requested nConv %d\n", i + 1, nvec[i]);
        printfQuda(" - level %d size of eigenvector search space %d\n", i + 1, mg_eig_nEv[i]);
        printfQuda(" - level %d size of Krylov space %d\n", i + 1, mg_eig_nKr[i]);
        printfQuda(" - level %d solver tolerance %e\n", i + 1, mg_eig_tol[i]);
        printfQuda(" - level %d convergence required (%s)\n", i + 1, mg_eig_require_convergence[i] ? "true" : "false");
        printfQuda(" - level %d Operator: daggered (%s) , norm-op (%s)\n", i + 1,
                   mg_eig_use_dagger[i] ? "true" : "false", mg_eig_use_normop[i] ? "true" : "false");
        if (mg_eig_use_poly_acc[i]) {
          printfQuda(" - level %d Chebyshev polynomial degree %d\n", i + 1, mg_eig_poly_deg[i]);
          printfQuda(" - level %d Chebyshev polynomial minumum %e\n", i + 1, mg_eig_amin[i]);
          if (mg_eig_amax[i] <= 0)
            printfQuda(" - level %d Chebyshev polynomial maximum will be computed\n", i + 1);
          else
            printfQuda(" - level %d Chebyshev polynomial maximum %e\n", i + 1, mg_eig_amax[i]);
        }
        printfQuda("\n");
      }
    }
  }

  if (inv_deflate) {
    printfQuda("\n   Eigensolver parameters\n");
    printfQuda(" - solver mode %s\n", get_eig_type_str(eig_type));
    printfQuda(" - spectrum requested %s\n", get_eig_spectrum_str(eig_spectrum));
    if (eig_type == QUDA_EIG_BLK_TR_LANCZOS) printfQuda(" - eigenvector block size %d\n", eig_block_size);
    printfQuda(" - number of eigenvectors requested %d\n", eig_nConv);
    printfQuda(" - size of eigenvector search space %d\n", eig_nEv);
    printfQuda(" - size of Krylov space %d\n", eig_nKr);
    printfQuda(" - solver tolerance %e\n", eig_tol);
    printfQuda(" - convergence required (%s)\n", eig_require_convergence ? "true" : "false");
    if (eig_compute_svd) {
      printfQuda(" - Operator: MdagM. Will compute SVD of M\n");
      printfQuda(" - ***********************************************************\n");
      printfQuda(" - **** Overriding any previous choices of operator type. ****\n");
      printfQuda(" - ****    SVD demands normal operator, will use MdagM    ****\n");
      printfQuda(" - ***********************************************************\n");
    } else {
      printfQuda(" - Operator: daggered (%s) , norm-op (%s)\n", eig_use_dagger ? "true" : "false",
                 eig_use_normop ? "true" : "false");
    }
    if (eig_use_poly_acc) {
      printfQuda(" - Chebyshev polynomial degree %d\n", eig_poly_deg);
      printfQuda(" - Chebyshev polynomial minumum %e\n", eig_amin);
      if (eig_amax <= 0)
        printfQuda(" - Chebyshev polynomial maximum will be computed\n");
      else
        printfQuda(" - Chebyshev polynomial maximum %e\n\n", eig_amax);
    }
  }

  printfQuda("Grid partition info:     X  Y  Z  T\n");
  printfQuda("                         %d  %d  %d  %d\n", dimPartitioned(0), dimPartitioned(1), dimPartitioned(2),
             dimPartitioned(3));
}

auto plaquette_gauge(quda::GaugeField &g)
{
  int R[4] = {2 * comm_dim_partitioned(0), 2 * comm_dim_partitioned(1), 2 * comm_dim_partitioned(2),
              2 * comm_dim_partitioned(3)};

  quda::cudaGaugeField *data
    = quda::createExtendedGauge(*reinterpret_cast<quda::cudaGaugeField *>(&g), R, profileExtendedGauge);
  double3 plaq3 = quda::plaquette(*data);

  delete data;

  return plaq3;
}

int comm_rank_from_coords(const int *coords);

namespace quda
{
  void copyOffsetGauge(GaugeField &out, const GaugeField &in, const int offset[4]);

  void copy_color_spinor_offset(ColorSpinorField &out, const ColorSpinorField &in, const int offset[4]);

  auto product(const CommKey &input) { return input[0] * input[1] * input[2] * input[3]; }

  CommKey operator+(const CommKey &lhs, const CommKey &rhs)
  {
    CommKey sum;
    for (int d = 0; d < nDim; d++) { sum[d] = lhs[d] + rhs[d]; }
    return sum;
  }

  CommKey operator*(const CommKey &lhs, const CommKey &rhs)
  {
    CommKey product;
    for (int d = 0; d < nDim; d++) { product[d] = lhs[d] * rhs[d]; }
    return product;
  }

  CommKey operator/(const CommKey &lhs, const CommKey &rhs)
  {
    CommKey quotient;
    for (int d = 0; d < nDim; d++) { quotient[d] = lhs[d] / rhs[d]; }
    return quotient;
  }

  CommKey operator%(const CommKey &lhs, const CommKey &rhs)
  {
    CommKey mod;
    for (int d = 0; d < nDim; d++) { mod[d] = lhs[d] % rhs[d]; }
    return mod;
  }

  CommKey coordinate_from_index(int index, CommKey dim)
  {
    CommKey coord;
    for (int d = 0; d < nDim; d++) {
      coord[d] = index % dim[d];
      index /= dim[d];
    }
    return coord;
  }

  int index_from_coordinate(CommKey coord, CommKey dim)
  {
    return ((coord[3] * dim[2] + coord[2]) * dim[1] + coord[1]) * dim[0] + coord[0];
  }

  void print(const CommKey &comm_key)
  {
    printf("(%3d,%3d,%3d,%3d)", comm_key[0], comm_key[1], comm_key[2], comm_key[3]);
  }

  void split_gauge_field(GaugeField &collect_field, GaugeField &base_field, const CommKey &comm_key)
  {
    CommKey full_dim = {comm_dim(0), comm_dim(1), comm_dim(2), comm_dim(3)};
    CommKey full_idx = {comm_coord(0), comm_coord(1), comm_coord(2), comm_coord(3)};

    int rank = comm_rank();
    int total_rank = product(full_dim);

    auto grid_dim = full_dim / comm_key;  // Communicator grid.
    auto block_dim = full_dim / grid_dim; // The full field needs to be partitioned according to the communicator grid.

    int n_replicates = product(comm_key);
    std::vector<void *> v_send_buffer_h(n_replicates, nullptr);
    std::vector<MsgHandle *> v_mh_send(n_replicates, nullptr);

    // Send cycles
    for (int i = 0; i < n_replicates; i++) {
      auto grid_idx = coordinate_from_index(i, comm_key);
      auto block_idx = full_idx / block_dim;
      // auto thread_idx = full_idx % block_dim;

      auto dst_idx = grid_idx * grid_dim + block_idx;

      int dst_rank = comm_rank_from_coords(dst_idx.data());
      int tag = rank * total_rank + dst_rank; // tag = src_rank * total_rank + dst_rank

      // TODO: For now only the cpu-gpu communication is implemented.
      printf("rank %4d -> rank %4d: tag = %4d\n", comm_rank(), dst_rank, tag);

      size_t bytes = base_field.Bytes();

      v_send_buffer_h[i] = pinned_malloc(bytes);
      cudaMemcpy(v_send_buffer_h[i], base_field.Gauge_p(), bytes, cudaMemcpyDeviceToHost);

      v_mh_send[i] = comm_declare_send_rank(v_send_buffer_h[i], dst_rank, tag, bytes);
      comm_start(v_mh_send[i]);
    }

    quda::GaugeFieldParam param(base_field);
    quda::GaugeField *buffer_field = quda::GaugeField::Create(param);

    CommKey thread_dim = {base_field.X()[0], base_field.X()[1], base_field.X()[2], base_field.X()[3]};

    // Receive cycles
    for (int i = 0; i < n_replicates; i++) {
      auto thread_idx = coordinate_from_index(i, comm_key);
      auto src_idx = (full_idx % grid_dim) * block_dim + thread_idx;

      int src_rank = comm_rank_from_coords(src_idx.data());
      int tag = src_rank * total_rank + rank;

      // TODO: For now only the cpu-gpu communication is implemented.
      printf("rank %4d <- rank %4d: tag = %4d\n", comm_rank(), src_rank, tag);

      size_t bytes = buffer_field->Bytes();

      void *recv_buffer_h = pinned_malloc(bytes);

      auto mh_recv = comm_declare_recv_rank(recv_buffer_h, src_rank, tag, bytes);

      comm_start(mh_recv);
      comm_wait(mh_recv);

      cudaMemcpy(buffer_field->Gauge_p(), recv_buffer_h, bytes, cudaMemcpyHostToDevice);

      comm_free(mh_recv);
      host_free(recv_buffer_h);

      auto offset = thread_idx * thread_dim;

      quda::copyOffsetGauge(collect_field, *buffer_field, offset.data());
    }

    delete buffer_field;

    comm_barrier();

    for (auto &p : v_send_buffer_h) { host_free(p); };
    for (auto &p : v_mh_send) { comm_free(p); };
  }

} // namespace quda

double plaq_ref;
double plaq_split;

TEST(split, verify)
{
  double tol = getTolerance(cuda_prec);
  double deviation = abs(plaq_split - plaq_ref);

  ASSERT_LE(deviation, tol) << "CPU and CUDA implementations do not agree";
}

int main(int argc, char **argv)
{
  // initalize google test, includes command line options
  ::testing::InitGoogleTest(&argc, argv);

  // Parse command line options
  auto app = make_app();
  try {
    app->parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app->exit(e);
  }

  // Set values for precisions via the command line.
  setQudaPrecisions();

  // initialize QMP/MPI, QUDA comms grid and RNG (host_utils.cpp)
  initComms(argc, argv, gridsize_from_cmdline);

  // All parameters have been set. Display the parameters via stdout
  display_test_info();

  // Initialize the QUDA library
  initQuda(device);

  QudaGaugeParam gauge_param = newQudaGaugeParam();
  setWilsonGaugeParam(gauge_param);
  QudaInvertParam inv_param = newQudaInvertParam();

  setInvertParam(inv_param);

  // set parameters for the reference Dslash, and prepare fields to be loaded
  if (dslash_type == QUDA_DOMAIN_WALL_DSLASH || dslash_type == QUDA_DOMAIN_WALL_4D_DSLASH
      || dslash_type == QUDA_MOBIUS_DWF_DSLASH || dslash_type == QUDA_MOBIUS_DWF_EOFA_DSLASH) {
    dw_setDims(gauge_param.X, inv_param.Ls);
  } else {
    setDims(gauge_param.X);
  }
  setSpinorSiteSize(24);

  // Allocate host side memory for the gauge field.
  //----------------------------------------------------------------------------
  void *gauge[4];
  // Allocate space on the host (always best to allocate and free in the same scope)
  for (int dir = 0; dir < 4; dir++) gauge[dir] = malloc(V * gauge_site_size * host_gauge_data_type_size);
  constructHostGaugeField(gauge, gauge_param, argc, argv);
  // Load the gauge field to the device
  loadGaugeQuda((void *)gauge, &gauge_param);

  // Compute plaquette as a sanity check
  double plaq[3];
  plaqQuda(plaq);
  printfQuda("Computed plaquette is %12.8e: (spatial = %12.8e, temporal = %12.8e)\n", plaq[0], plaq[1], plaq[2]);

  plaq_ref = plaq[0];

  CommKey split_key = {1, 2, 2, 2};

  quda::GaugeFieldParam param(*gaugePrecise);

  for (int d = 0; d < nDim; d++) {
    if (comm_dim(d) % split_key[d] != 0) {
      errorQuda("Split not possible: (%d,%d,%d,%d) / (%d,%d,%d,%d).", comm_dim(0), comm_dim(1), comm_dim(2),
                comm_dim(3), split_key[0], split_key[1], split_key[2], split_key[3]);
    }

    param.x[d] *= split_key[d];
    param.pad *= split_key[d];
  }
  quda::GaugeField *collect_gauge = quda::GaugeField::Create(param);

  quda::split_gauge_field(*collect_gauge, *gaugePrecise, split_key);

  comm_barrier();

  // Create a communicator with the split {1, 1, 1, 2} and push to the top.
  if (gridsize_from_cmdline[3] % 2 == 0) push_to_current(split_key);

  comm_barrier();

  auto plaq_split3 = plaquette_gauge(*collect_gauge);
  plaq_split = plaq_split3.x;

  if (gridsize_from_cmdline[3] % 2 == 0) push_to_current({1, 1, 1, 1});

  delete collect_gauge;

  printf("Computed plaquette is %12.8e: (%12.8e %12.8e, splitted on rank %4d)\n", plaq_split3.x, plaq_split3.y,
         plaq_split3.z, comm_rank());

  comm_barrier();

  /**
    quda::ColorSpinorParam cpu_cs_param;
    constructWilsonTestSpinorParam(&cpu_cs_param, &inv_param, &gauge_param);
    quda::cpuColorSpinorField *in_h = new quda::cpuColorSpinorField(cpu_cs_param);

    auto *rng = new quda::RNG(quda::LatticeFieldParam(gauge_param), 1234 * comm_rank());
    rng->Init();

    // Populate the host spinor with random numbers.
    constructRandomSpinorSource(in_h->V(), 4, 3, inv_param.cpu_prec, gauge_param.X, *rng);

    quda::ColorSpinorParam cuda_cs_param(cpu_cs_param, inv_param);
    cuda_cs_param.create = QUDA_NULL_FIELD_CREATE;
    quda::cudaColorSpinorField *in_d = new quda::cudaColorSpinorField(cuda_cs_param);

    *in_d = *in_h;


    cuda_cs_param.x[3] *= 2;
    cuda_cs_param.pad *= 2;
    cuda_cs_param.create = QUDA_ZERO_FIELD_CREATE;
    quda::cudaColorSpinorField *collect_d = new quda::cudaColorSpinorField(cuda_cs_param);

    for (int i = 0; i < 2; i++) {
      int offset_cs[4] = {0, 0, 0, i * cuda_cs_param.x[3] / 2};
      copy_color_spinor_offset(*collect_d, *in_d, offset_cs);
      copy_color_spinor_offset(*in_d, *collect_d, offset_cs);
    }

    printf("in_h norm = %12.8e, in_d norm = %12.8e, collect_d = %12.8e.\n",
      quda::blas::norm2(*in_h), quda::blas::norm2(*in_d), quda::blas::norm2(*collect_d));

    delete in_h;
    delete in_d;
    delete collect_d;

    rng->Release();
    delete rng;
  */

  if (verify_results) {
    int test_rc = RUN_ALL_TESTS();
    if (test_rc != 0) warningQuda("Tests failed on rank %4d", comm_rank());
  }

  // finalize the QUDA library
  endQuda();
  finalizeComms();

  return 0;
}