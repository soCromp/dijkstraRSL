#include <stdio.h>

#include "rsl_inc/hp_manager.h"
#include "rsl_inc/hp_manager_leaky.h"
#include "rsl_inc/vector_sfra.h"
#include "rsl_inc/vector_ufra.h"

#include "rsl.h"

extern "C" {
typedef std::pair<const unsigned long, unsigned long> value_type;

typedef skipvector<unsigned long, unsigned long, vector_ufra, vector_sfra, 4, 3,
                   16, hp_manager_leaky<200>>
    rsl_t;

rsl_t *rsl_create() {
  rsl_t *r = new rsl_t();
  return r;
};
//                       2           3               0,1,2       = 2,8,14,..
// tid is thread's id    0           3               0,1,2,..    = 0,3,6,...
int rsl_insert(rsl_t *pq, unsigned long k, unsigned long v,
               int tid) { //(d->id)+(d->nb_threads)*(d->rpqInserts)
  unsigned long ktrans =
      (k << 32) + tid; // 10000....0000, 2000...0001, 3000...0002, 10000...0003
  const value_type ins = {ktrans, v};
  return (int)pq->insert(ins); // 0 == false, 1 == true in C++ and C
};

int rsl_extract_min(rsl_t *pq, unsigned long *k, unsigned long *v) {
  int res = (int)pq->extract_min(k, v);
  if (res == 1)
    *k = *k >> 32;
  return res;
};

int rsl_size(rsl_t *pq) { return pq->get_size(); }
}
