#include "skipvector.h"
#include "rsl_c.h"
#include "skipvector.h"
#include "vector_sfra.h"
#include "../hp/hp_deletable.h"

#ifdef __cplusplus
extern "C" {
#endif

    rsl_t *rsl_create(setup_t *cfg){
        if(cfg->layers == 0) { //calculate layers
            size_t key_range = 65536;
            int chunksize = 32;
            int index_size = 32;
            const double data_node_count_target = key_range * 1.0 / chunksize;
            int layers = static_cast<int>(
                std::ceil(std::log(data_node_count_target) / std::log(index_size)));
            layers = layers < 0 ? 1 : layers;
            cfg->layers = layers;
        }
        rsl_t *r = new rsl(cfg);
        return r;
    };

#ifdef __cplusplus  
}
#endif
