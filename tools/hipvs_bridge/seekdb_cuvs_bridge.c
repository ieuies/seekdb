/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * This is an independent implementation of the contest submission for the AMD
 * hipVS task. It keeps the interface of the frozen V5 bridge unchanged and
 * changes three things:
 *
 *  1. The query workspace (device buffers for the query vectors, the neighbour
 *     ids and the neighbour distances) and the cuVS search parameters live in
 *     the index handle and are reused by every query. The frozen bridge
 *     allocates and frees three device buffers and creates and destroys the
 *     search parameters for every single call. cudaFree synchronises the
 *     device, so that repeated alloc/free pair also stalls the pipeline
 *     between the queries of one SQL statement.
 *
 *  2. The OpenMP team size used by the graph refinement is capped to the
 *     number of CPUs the process is actually allowed to run on. The OpenMP
 *     runtime of the evaluation image reports the thread count of the host,
 *     which is far above the core quota of the container, so an oversized
 *     team only adds scheduling overhead to every refinement round.
 *
 *  3. The intermediate graph degree of the NN-Descent refinement is lowered
 *     from 256 to 192. The final graph degree stays at 128 because it decides
 *     the steady state recall, and the recall gates of the task are tight.
 *     The intermediate degree only decides how many candidates every
 *     refinement round looks at, and 192 is the lower bound of the 1.5x-2x
 *     band that cuVS documents for it, so the per round candidate work drops
 *     by a quarter while the recommended band is respected. The iteration cap
 *     is left alone: NN-Descent already stops early once the graph converges,
 *     so a lower cap would buy nothing on a converged graph and would only
 *     risk truncating a graph that has not converged yet.
 */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif
#if defined(_OPENMP)
#include <omp.h>
#endif

/* The frontier of one search is four times the requested neighbour count, with
 * a floor that keeps the recall of the small top-k requests reasonable. */
#define SK_CUVS_ITOPK_FACTOR 4
#define SK_CUVS_ITOPK_FLOOR 512

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;

  /* Persistent query workspace, reused by every search on this index. */
  float*                     q_buf;     /* device: nq * dim query vectors      */
  unsigned*                  id_buf;    /* device: nq * topk neighbour ids      */
  float*                     dist_buf;  /* device: nq * topk neighbour distances */
  size_t                     q_cap;     /* elements reserved in q_buf          */
  size_t                     id_cap;    /* elements reserved in id_buf         */
  size_t                     dist_cap;  /* elements reserved in dist_buf       */
  cuvsCagraSearchParams_t    sp;        /* search parameters, kept across calls */
  /* The workspace belongs to the index, so the searches that share an index
   * share it as well. The frozen bridge allocated its buffers per call, which
   * made it safe by accident; the lock keeps it safe on purpose. All the
   * searches of one index already run on the same device stream. */
  pthread_mutex_t            lock;
} sk_cuvs_index;

/* The number of CPUs this process may run on. The evaluation container has a
 * much smaller core quota than the machine the OpenMP runtime sees, so the
 * affinity mask is the only trustworthy source. */
static long sk_cuvs_allowed_cpus(void)
{
#if defined(__linux__) && defined(CPU_COUNT)
  cpu_set_t set;
  if (0 == sched_getaffinity(0, sizeof(set), &set)) {
    int n = CPU_COUNT(&set);
    if (n > 0) {
      return (long)n;
    }
  }
#endif
  return 0;
}

/* Keep the refinement team inside the core quota of the process. An explicit
 * setting of the user or of the environment is never overridden. */
static void sk_cuvs_limit_openmp_team(void)
{
  long cpus = sk_cuvs_allowed_cpus();
#if defined(_OPENMP)
  if (cpus > 0) {
    int current = omp_get_max_threads();
    if (current > (int)cpus) {
      omp_set_num_threads((int)cpus);
    }
  }
#else
  if (cpus > 0) {
    /* The runtime may already be initialised, in which case the environment
     * is only a hint, but it is still the best effort available here. */
    const char* current = getenv("OMP_NUM_THREADS");
    if (NULL == current || current[0] == '\0') {
      char buf[32];
      snprintf(buf, sizeof(buf), "%ld", cpus);
      setenv("OMP_NUM_THREADS", buf, 0);
    }
  }
#endif
}

static int sk_cuvs_reserve(sk_cuvs_index* h, long nq, long topk)
{
  int rc = 0;
  size_t want_q = (size_t)nq * (size_t)h->dim;
  size_t want_out = (size_t)nq * (size_t)topk;
  if (h->q_cap < want_q) {
    if (NULL != h->q_buf) {
      if (cuvsRMMFree(h->res, h->q_buf, h->q_cap * sizeof(float)) != CUVS_SUCCESS) { rc = 20; }
      h->q_buf = NULL;
      h->q_cap = 0;
    }
    if (0 == rc && cuvsRMMAlloc(h->res, (void**)&h->q_buf, want_q * sizeof(float)) != CUVS_SUCCESS) {
      rc = 10;
    }
    if (0 == rc) {
      h->q_cap = want_q;
    }
  }
  if (0 == rc && h->id_cap < want_out) {
    if (NULL != h->id_buf) {
      if (cuvsRMMFree(h->res, h->id_buf, h->id_cap * sizeof(unsigned)) != CUVS_SUCCESS) { rc = 21; }
      h->id_buf = NULL;
      h->id_cap = 0;
    }
    if (0 == rc && cuvsRMMAlloc(h->res, (void**)&h->id_buf, want_out * sizeof(unsigned)) != CUVS_SUCCESS) {
      rc = 11;
    }
    if (0 == rc) {
      h->id_cap = want_out;
    }
  }
  if (0 == rc && h->dist_cap < want_out) {
    if (NULL != h->dist_buf) {
      if (cuvsRMMFree(h->res, h->dist_buf, h->dist_cap * sizeof(float)) != CUVS_SUCCESS) { rc = 22; }
      h->dist_buf = NULL;
      h->dist_cap = 0;
    }
    if (0 == rc && cuvsRMMAlloc(h->res, (void**)&h->dist_buf, want_out * sizeof(float)) != CUVS_SUCCESS) {
      rc = 12;
    }
    if (0 == rc) {
      h->dist_cap = want_out;
    }
  }
  return rc;
}

static void sk_cuvs_release_workspace(sk_cuvs_index* h)
{
  if (NULL == h) {
    return;
  }
  if (NULL != h->q_buf && h->q_cap > 0) {
    cuvsRMMFree(h->res, h->q_buf, h->q_cap * sizeof(float));
  }
  if (NULL != h->id_buf && h->id_cap > 0) {
    cuvsRMMFree(h->res, h->id_buf, h->id_cap * sizeof(unsigned));
  }
  if (NULL != h->dist_buf && h->dist_cap > 0) {
    cuvsRMMFree(h->res, h->dist_buf, h->dist_cap * sizeof(float));
  }
  h->q_buf = NULL;
  h->id_buf = NULL;
  h->dist_buf = NULL;
  h->q_cap = 0;
  h->id_cap = 0;
  h->dist_cap = 0;
  if (NULL != h->sp) {
    cuvsCagraSearchParamsDestroy(h->sp);
    h->sp = NULL;
  }
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (0 != pthread_mutex_init(&h->lock, NULL)) { free(h); return NULL; }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { pthread_mutex_destroy(&h->lock); free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.bits = 32; dset.dl_tensor.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  /* The final degree keeps the steady state recall. */
  h->ip->graph_degree = 128;
  /* The intermediate degree only sizes the candidate list of one refinement
   * round; 192 is the lower bound of the band cuVS documents for it. */
  h->ip->intermediate_graph_degree = 192;
  /* The iteration cap stays at the frozen value, the refinement stops early on
   * a converged graph on its own. */
  h->ip->nn_descent_niter = 100;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  sk_cuvs_limit_openmp_team();
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); pthread_mutex_destroy(&h->lock); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  if (nq <= 0 || topk <= 0) return 1;
  pthread_mutex_lock(&h->lock);
  if (0 != (rc = sk_cuvs_reserve(h, nq, topk))) goto done;
  if (cudaMemcpy(h->q_buf, query, sizeof(float) * (size_t)nq * (size_t)h->dim,
                 cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->q_buf; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.bits=32; qt.dl_tensor.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->id_buf; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dist_buf; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (NULL == h->sp) {
      if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc = 14; goto done; }
    }
    {
      size_t required_itopk = (size_t)topk * SK_CUVS_ITOPK_FACTOR;
      if (required_itopk < SK_CUVS_ITOPK_FLOOR) required_itopk = SK_CUVS_ITOPK_FLOOR;
      if (h->sp->itopk_size < required_itopk) h->sp->itopk_size = required_itopk;
      h->sp->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc = 15; goto done; }
    if (cudaMemcpy(out_ids, h->id_buf, sizeof(unsigned) * (size_t)nq * (size_t)topk,
                   cudaMemcpyDefault) != cudaSuccess) { rc = 16; goto done; }
    if (out_dist) {
      if (cudaMemcpy(out_dist, h->dist_buf, sizeof(float) * (size_t)nq * (size_t)topk,
                     cudaMemcpyDefault) != cudaSuccess) { rc = 17; goto done; }
    }
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  pthread_mutex_unlock(&h->lock);
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  pthread_mutex_lock(&h->lock);
  sk_cuvs_release_workspace(h);
  pthread_mutex_unlock(&h->lock);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->lock);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (0 != pthread_mutex_init(&index->lock, NULL)) { free(index); return NULL; }
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx: cuvsCagraIndexDestroy(index->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(index->ip);
fail_res: cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  pthread_mutex_destroy(&index->lock);
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
