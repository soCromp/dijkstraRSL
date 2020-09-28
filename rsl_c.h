#ifdef __cplusplus
extern "C" {
#endif

        typedef unsigned long slkey_t;
        typedef unsigned long val_t;

        //typedef struct setup {
        //        int layers;
        //} setup_t;

        typedef struct rsl rsl_t;

        rsl_t *rsl_create();
        int rsl_insert(rsl_t *pq, slkey_t k, val_t v, int tid);
        int rsl_extract_min(rsl_t *pq, slkey_t *k, val_t *v);
        int rsl_size(rsl_t *pq);

#ifdef __cplusplus
}
#endif