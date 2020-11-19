#ifndef __MY_PALLOC_H__
#define __MY_PALLOC_H__

typedef long long js_int_t;
typedef unsigned long long js_uint_t;
typedef unsigned char	u_char;
typedef unsigned short	u_short;
typedef unsigned int	u_int;

#define js_align(d, a)     (((d) + (a - 1)) & ~(a - 1))
#define js_align_ptr(p, a)                                                   \
    (u_char *) (((uintptr_t) (p) + ((uintptr_t) a - 1)) & ~((uintptr_t) a - 1))

typedef void (*js_pool_cleanup_pt)(void *data);
typedef struct js_pool_cleanup_s  js_pool_cleanup_t;
struct js_pool_cleanup_s {
    js_pool_cleanup_pt   handler;  /* 清理的回调函数 */
    void                 *data; 	/* 指向存储的数据地址 */
    js_pool_cleanup_t    *next; 	/* 下一个js_pool_cleanup_t */
} ;

typedef struct js_pool_large_s js_pool_large_t;
struct js_pool_large_s {
    js_pool_large_t      *next;   /* 指向下一个存储地址 通过这个地址可以知道当前块长度 */
    void                 *alloc;  /* 数据块指针地址 */
};

typedef struct js_pool_s  js_pool_t;

typedef struct {
    u_char               *last;     /* 内存池中未使用内存的开始节点地址 */
    u_char               *end;      /* 内存池的结束地址 */
    js_pool_t            *next;     /* 指向下一个内存池 */
    js_uint_t            failed;    /* 失败次数 */
} js_pool_data_t;


struct js_pool_s {
    js_pool_data_t        d;        /* 内存池的数据区域*/
    js_uint_t                max;      /* 最大每次可分配内存 */
    js_pool_t            *current;  /* 指向当前的内存池指针地址。js_pool_t链表上最后一个缓存池结构*/
    //js_chain_t          *chain;    /* 缓冲区链表 */
    js_pool_large_t      *large;    /* 存储大数据的链表 */
    js_pool_cleanup_t    *cleanup;  /* 可自定义回调函数，清除内存块分配的内存 */
    //js_log_t            *log;      /* 日志 */
};

/**
 * 封装了malloc函数，并且添加了日志
 */
 void *js_alloc(js_uint_t size/*, js_log_t *log*/);
 /**
 * 调用js_alloc方法，如果分配成，将内存块设置为0
 */
void *js_calloc(js_uint_t size/*, js_log_t *log*/);

// 创建内存池
js_pool_t *js_create_pool(js_uint_t size/*, js_log_t *log*/);
// 释放内存池
void js_destroy_pool(js_pool_t *pool);
// 重置内存池
void js_reset_pool(js_pool_t *pool);
// 从内存池中分配大小为size的内存
void *js_palloc(js_pool_t *pool, js_uint_t size);
// 从内存池中分配连续为size的内存
void *js_pnalloc(js_pool_t *pool, js_uint_t size);
void *js_pcalloc(js_pool_t *pool, js_uint_t size);

void *js_pmemalign(js_pool_t *pool, js_uint_t size);
// 大内存块释放  pool->large
js_int_t js_pfreeLarge(js_pool_t *pool, void *p);

#endif