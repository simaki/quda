#ifndef _UTIL_QUDA_H
#define _UTIL_QUDA_H

#include <stdio.h>
#include <stdlib.h>
#include <enum_quda.h>
#include <comm_quda.h>
#include <tune_key.h>

/**
   @brief Query whether autotuning is enabled or not.  Default is enabled but can be overridden by setting QUDA_ENABLE_TUNING=0.
   @return If autotuning is enabled
 */
QudaTune getTuning();

QudaVerbosity getVerbosity();
char *getOutputPrefix();
FILE *getOutputFile();

void setVerbosity(const QudaVerbosity verbosity);
void setOutputPrefix(const char *prefix);
void setOutputFile(FILE *outfile);

void pushVerbosity(QudaVerbosity verbosity);
void popVerbosity();

/**
   @brief This function returns true if the calling rank is enabled
   for verbosity (e.g., whether printQuda and warningQuda will print
   out from this rank).
   @return Return whether this rank will print
 */
bool getRankVerbosity();

char *getPrintBuffer();


#define zeroThread (threadIdx.x + blockDim.x*blockIdx.x==0 &&		\
		    threadIdx.y + blockDim.y*blockIdx.y==0 &&		\
		    threadIdx.z + blockDim.z*blockIdx.z==0)

#define printfZero(...)	do {						\
    if (zeroThread) printf(__VA_ARGS__);				\
  } while (true)


#ifdef MULTI_GPU

#define printfQuda(...) do {                           \
  sprintf(getPrintBuffer(), __VA_ARGS__);	       \
  if (getRankVerbosity()) {			       \
    fprintf(getOutputFile(), "%s", getOutputPrefix()); \
    fprintf(getOutputFile(), "%s", getPrintBuffer());  \
    fflush(getOutputFile());                           \
  }                                                    \
} while (true)

#define errorQuda(...) do {                                                  \
  fprintf(getOutputFile(), "%sERROR: ", getOutputPrefix());                  \
  fprintf(getOutputFile(), __VA_ARGS__);                                     \
  fprintf(getOutputFile(), " (rank %d, host %s, " __FILE__ ":%d in %s())\n", \
          comm_rank(), comm_hostname(), __LINE__, __func__);                 \
  fprintf(getOutputFile(), "%s       last kernel called was (name=%s,volume=%s,aux=%s)\n", \
	  getOutputPrefix(), getLastTuneKey().name,			     \
	  getLastTuneKey().volume, getLastTuneKey().aux);	             \
  fflush(getOutputFile());                                                   \
  comm_abort(1);                                                             \
} while (true)

#define warningQuda(...) do {                                   \
  if (getVerbosity() > QUDA_SILENT) {				\
    sprintf(getPrintBuffer(), __VA_ARGS__);			\
    if (getRankVerbosity()) {						\
      fprintf(getOutputFile(), "%sWARNING: ", getOutputPrefix());	\
      fprintf(getOutputFile(), "%s", getPrintBuffer());			\
      fprintf(getOutputFile(), "\n");					\
      fflush(getOutputFile());						\
    }									\
  }									\
} while (true)

#else

#define printfQuda(...) do {                         \
  fprintf(getOutputFile(), "%s", getOutputPrefix()); \
  fprintf(getOutputFile(), __VA_ARGS__);             \
  fflush(getOutputFile());                           \
} while (true)

#define errorQuda(...) do {						     \
  fprintf(getOutputFile(), "%sERROR: ", getOutputPrefix());		     \
  fprintf(getOutputFile(), __VA_ARGS__);				     \
  fprintf(getOutputFile(), " (" __FILE__ ":%d in %s())\n",		     \
	  __LINE__, __func__);						     \
  fprintf(getOutputFile(), "%s       last kernel called was (name=%s,volume=%s,aux=%s)\n", \
	  getOutputPrefix(), getLastTuneKey().name,			     \
	  getLastTuneKey().volume, getLastTuneKey().aux);		     \
  comm_abort(1);								     \
} while (true)

#define warningQuda(...) do {                                 \
  if (getVerbosity() > QUDA_SILENT) {			      \
    fprintf(getOutputFile(), "%sWARNING: ", getOutputPrefix()); \
    fprintf(getOutputFile(), __VA_ARGS__);                      \
    fprintf(getOutputFile(), "\n");                             \
    fflush(getOutputFile());                                    \
  }								\
} while (true)

#endif // MULTI_GPU


#define checkCudaErrorNoSync() do {                    \
  cudaError_t error = cudaGetLastError();              \
  if (error != cudaSuccess)                            \
    errorQuda("(CUDA) %s", cudaGetErrorString(error)); \
} while (true)


#ifdef HOST_DEBUG

#define checkCudaError() do {  \
  cudaDeviceSynchronize();     \
  checkCudaErrorNoSync();      \
} while (true)

#else

#define checkCudaError() checkCudaErrorNoSync()

#endif // HOST_DEBUG


#endif // _UTIL_QUDA_H
