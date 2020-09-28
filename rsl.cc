#include "rsl.h"
#include <stdio.h>

extern "C" {
    typedef std::pair<const slkey_t, val_t> value_type;
    
    rsl_t *rsl_create(){
        rsl_t *r = new rsl();
        return r;
    };
                                            //                  2           3               0,1,2       = 2,8,14,..
    //tid is thread's id                                        0           3               0,1,2,..    = 0,3,6,...
    int rsl_insert(rsl_t *pq, slkey_t k, val_t v, int tid) { //(d->id)+(d->nb_threads)*(d->rpqInserts)
        slkey_t ktrans = (k << 32)+tid; // 10000....0000, 2000...0001, 3000...0002, 10000...0003
        const value_type ins = {ktrans, v};
        return (int) pq->insert(ins); //0 == false, 1 == true in C++ and C
    };

    int rsl_extract_min(rsl_t *pq, slkey_t *k, val_t *v) {
        int res = (int) pq->extract_min(k, v);
        if(res == 1) 
            *k = *k >> 32;
        return res;
    };

    int rsl_size(rsl_t *pq) {
        return pq->get_size();
    }

}
