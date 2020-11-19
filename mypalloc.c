#include "mypalloc.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

static inline void *js_palloc_small(js_pool_t *pool, js_uint_t size,js_uint_t align);
static void *js_palloc_block(js_pool_t *pool, js_uint_t size);
static void *js_palloc_large(js_pool_t *pool, js_uint_t size);
/**
 * 封装了malloc函数，并且添加了日志
 */
 void *js_alloc(js_uint_t size/*, js_log_t *log*/) {
    void  *p;
    //分配一块内存
    p = malloc(size);
    if (p == NULL) {
        printf("malloc(%s) failed\n", size);
    }

    printf("malloc: %p:%uz\n", p, size);
    return p;
 }

  /**
 * 调用js_alloc方法，如果分配成，将内存块设置为0
 */
void *js_calloc(js_uint_t size/*, js_log_t *log*/) {
    void  *p;

    //调用内存分配函数
    p = js_alloc(size);
 
    if (p) {
    	//将内存块全部设置为0
        memset(p, 0, size);
    }

    return p;
}

js_pool_t *js_create_pool(js_uint_t size/*, js_log_t *log*/) {
    js_pool_t  *p;

    p = js_alloc(size);
    if (p == NULL) {
        return NULL;
    }

    /**
	 * Nginx会分配一块大内存，其中内存头部存放js_pool_t本身内存池的数据结构
	 * js_pool_data_t	p->d 存放内存池的数据部分（适合小于p->max的内存块存储）
	 * p->large 存放大内存块列表
	 * p->cleanup 存放可以被回调函数清理的内存块（该内存块不一定会在内存池上面分配）
	 */
    p->d.last = (u_char)p + sizeof(js_pool_t); // 内存开始地址，指向js_pool_t结构体之后数据取起始位置
    p->d.end = (u_char)p + size; // 内存结束地址
    p->d.next = NULL;            // 下一个js_pool_t内存池地址
    p->d.failed = 0;             // 失败次数
    size = size - sizeof(js_pool_t);
    p->max = (size < 4095) ? size : 4095;

    /* 只有缓存池的父节点，才会用到下面的这些  ，子节点只挂载在p->d.next,并且只负责p->d的数据内容*/
    p->current = p;
    //p->chain = NULL;
    p->large = NULL;
    p->cleanup = NULL;
    //p->log = log;

    return p;
}

void js_destroy_pool(js_pool_t *pool) {
    js_pool_t          *p, *n;
    js_pool_large_t    *l;
    js_pool_cleanup_t  *c;

    /* 首先清理pool->cleanup链表 */
    for (c = pool->cleanup; c; c = c->next) {
        if (c->handler) {
            printf("run cleanup: %p", c);
            c->handler(c->data);
        }
    }

    /* 清理pool->large链表（pool->large为单独的大数据内存块）  */
    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            free(l->alloc);
        }
    }

    /* 对内存池的data数据区域进行释放 */
    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        free(p);

        if (n == NULL) {
            break;
        }
    }
}

void js_reset_pool(js_pool_t *pool) {
    js_pool_t       *p;
    js_pool_large_t *l;

    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            free(l->alloc);
        }
    }

    for (p = pool; p; p = p->d.next) {
        p->d.last = (u_char *) p + sizeof(js_pool_t);
        p->d.failed = 0;
    }

    pool->current = pool;
    //pool->chain = NULL;
    pool->large = NULL;
}

/**
 * 内存池分配一块内存，返回void类型指针
 */
void *
js_palloc(js_pool_t *pool, js_uint_t size)
{
//#if !(js_DEBUG_PALLOC)
    /* 判断每次分配的内存大小 */
    if (size <= pool->max) {    
        return js_palloc_small(pool, size, 1);
    }
//#endif

    /* 走大数据分配策略 ，在pool->large链表上分配 */
    return js_palloc_large(pool, size);
}


void *
js_pnalloc(js_pool_t *pool, js_uint_t size)
{
//#if !(js_DEBUG_PALLOC)
    if (size <= pool->max) {
        return js_palloc_small(pool, size, 0);
    }
//#endif

    return js_palloc_large(pool, size);
}

void *
js_pcalloc(js_pool_t *pool, js_uint_t size)
{
    void *p;

    p = js_palloc(pool, size);
    if (p) {
        memset(p, 0, size);
    }

    return p;
}

static inline void*
js_palloc_small(js_pool_t *pool, js_uint_t size, js_uint_t align)
{
    u_char      *m;
    js_pool_t  *p;

    p = pool->current;

    /*
	* 循环读取缓存池链p->d.next的各个的js_pool_t节点，
	* 如果剩余的空间可以容纳size，则返回指针地址
	*
	* 这边的循环，实际上最多只有4次，具体可以看js_palloc_block函数
	* */
    do {
        m = p->d.last;

        if (align) {
            /* 对齐操作,会损失内存，但是提高内存使用速度 */
            m = js_align_ptr(m, sizeof(unsigned long));
        }

        if ((js_uint_t) (p->d.end - m) >= size) {
            p->d.last = m + size;

            return m;
        }

        p = p->d.next;

    } while (p);
    /* 如果缓存池空间没有可以容纳大小为size的内存块，则需要重新申请一个缓存池pool节点 */
    return js_palloc_block(pool, size);
}

/**
 * 申请一个新的缓存池 js_pool_t
 * 新的缓存池会挂载在主缓存池的 数据区域 （pool->d->next）
 */
static void *
js_palloc_block(js_pool_t *pool, js_uint_t size)
{
    u_char      *m;
    js_uint_t       psize;
    js_pool_t  *p, *new;

    psize = (js_uint_t) (pool->d.end - (u_char *) pool);
    /* 申请新的块 */ 
    m = js_alloc(psize);
    if (m == NULL) {
        return NULL;
    }

    new = (js_pool_t *) m;

    new->d.end = m + psize;
    new->d.next = NULL;
    new->d.failed = 0;

    m += sizeof(js_pool_data_t);
    m = js_align_ptr(m, sizeof(unsigned long));
    new->d.last = m + size;

    for (p = pool->current; p->d.next; p = p->d.next) {
        if (p->d.failed++ > 4) {
            pool->current = p->d.next;
        }
    }

    p->d.next = new;

    return m;
}


static void *
js_palloc_large(js_pool_t *pool, js_uint_t size)
{
    void              *p;
    js_uint_t         n;
    js_pool_large_t  *large;

    p = js_alloc(size);
    if (p == NULL) {
        return NULL;
    }

    n = 0;

    for (large = pool->large; large; large = large->next) {
        if (large->alloc == NULL) {
            large->alloc = p;
            return p;
        }

        if (n++ > 3) {
            break;
        }
    }

    large = js_palloc_small(pool, sizeof(js_pool_large_t), 1);
    if (large == NULL) {
        free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}


void *
js_pmemalign(js_pool_t *pool, js_uint_t size)
{
    void              *p;
    js_pool_large_t  *large;

    p = js_alloc(size);
    if (p == NULL) {
        return NULL;
    }

    large = js_palloc_small(pool, sizeof(js_pool_large_t), 1);
    if (large == NULL) {
        free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large; 

    return p;
}

/**
 * 大内存块释放  pool->large
 */
js_int_t
js_pfreeLarge(js_pool_t *pool, void *p)
{
    js_pool_large_t  *l;

    /* 在pool->large链上循环搜索，并且只释放内容区域，不释放js_pool_large_t数据结构*/
    for (l = pool->large; l; l = l->next) {
        if (p == l->alloc) {
            printf("free: %p\r\n", l->alloc);
            free(l->alloc);
            l->alloc = NULL;

            return 0;
        }
    }

    return -5;
}

int main() {
    js_pool_t *pool =  js_create_pool(1024);
    js_destroy_pool(pool);
}