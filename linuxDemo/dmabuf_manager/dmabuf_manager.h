#ifndef _DMABUF_MAN_H
#define _DMABUF_MAN_H

#ifdef __cplusplus
extern "C" {
#endif
#include "img_transfer_config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// 启用malloc分配方式
#if (DMABUF_ENABLE_MALLOC)
// 使用malloc分配
#define USE_MALLOC 1
// 如果显式定义了 USE_DMABUF 1
#elif (DMABUF_ENABLE_DMABUF)
// 使用dmabuf分配
#define USE_DMABUF 1
#else
// 一个也没定义默认使用dmabuf
#define USE_DMABUF 1
// 如果一个也没有定义，就报错
// #error "Either USE_MALLOC or USE_DMABUF must be defined"
#endif

// 启用线程安全
#if (DMABUF_ENABLE_THREAD_SAFE)
#define USE_THREAD_SAFE 1
#endif

// 打印已使用资源参数
#if (DMABUF_BUFFER_BLOCK_WIDTH)
// 池状态块宽度，例如 "[2I###]" 缓冲区2号处于空闲状态
#define BUFFER_BLOCK_WIDTH (DMABUF_BUFFER_BLOCK_WIDTH)
#else
#define BUFFER_BLOCK_WIDTH (8)
#endif
// 未分配状态字符
#if (DMABUF_MONITOR_UNALLOCATE_CHAR)
#define MONITOR_UNALLOCATE_CHAR (DMABUF_MONITOR_UNALLOCATE_CHAR)
#else
#define MONITOR_UNALLOCATE_CHAR ('U')
#endif
// 空闲状态字符
#if (DMABUF_MONITOR_IDLE_CHAR)
#define MONITOR_IDLE_CHAR (DMABUF_MONITOR_IDLE_CHAR)
#else
#define MONITOR_IDLE_CHAR ('I')
#endif
// 忙碌状态字符
#if (DMABUF_MONITOR_BUSY_CHAR)
#define MONITOR_BUSY_CHAR (DMABUF_MONITOR_BUSY_CHAR)
#else
#define MONITOR_BUSY_CHAR ('B')
#endif
// 已使用块填充字符（例如进度条中表示已占用的部分）
#if (DMABUF_MONITOR_USED_CHAR)
#define MONITOR_USED_CHAR (DMABUF_MONITOR_USED_CHAR)
#else
#define MONITOR_USED_CHAR ('#')
#endif
// 空闲块填充字符（例如进度条中表示未占用的部分）
#if (DMABUF_MONITOR_FREE_CHAR)
#define MONITOR_FREE_CHAR (DMABUF_MONITOR_FREE_CHAR)
#else
#define MONITOR_FREE_CHAR ('-')
#endif
// 默认每行显示块数（若无法获取终端宽度）
#define DEFAULT_COLS (8)

//=================== 跨平台抽象层（可配置线程安全）===================
#if (USE_THREAD_SAFE)
// 启用线程安全：使用 pthread 互斥锁和原子操作
#include <pthread.h>
// 互斥锁
#define dmabuf_mutex_t pthread_mutex_t
#define dmabuf_mutex_init(m) pthread_mutex_init(m, NULL)
#define dmabuf_mutex_destroy(m) pthread_mutex_destroy(m)
#define dmabuf_mutex_lock(m) pthread_mutex_lock(m)
#define dmabuf_mutex_unlock(m) pthread_mutex_unlock(m)

// 原子操作
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L &&                \
    !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#define dmabuf_atomic_t atomic_uint
#define dmabuf_atomic_load(p) atomic_load(p)
#define dmabuf_atomic_store(p, v) atomic_store(p, v)
#define dmabuf_atomic_fetch_add(p, v) atomic_fetch_add(p, v)
#define dmabuf_atomic_fetch_sub(p, v) atomic_fetch_sub(p, v)
#define dmabuf_atomic_inc(p) atomic_fetch_add(p, 1)
#define dmabuf_atomic_dec(p) atomic_fetch_sub(p, 1)
#elif defined(__GNUC__)
#define dmabuf_atomic_t uint32_t
#define dmabuf_atomic_load(p) __sync_fetch_and_add(p, 0) // 读屏障
#define dmabuf_atomic_store(p, v)                                              \
    do {                                                                       \
        __sync_synchronize();                                                  \
        *(p) = (v);                                                            \
    } while (0)
#define dmabuf_atomic_fetch_add(p, v) __sync_fetch_and_add(p, v)
#define dmabuf_atomic_fetch_sub(p, v) __sync_fetch_and_sub(p, v)
#define dmabuf_atomic_inc(p) __sync_fetch_and_add(p, 1)
#define dmabuf_atomic_dec(p) __sync_fetch_and_sub(p, 1)
#else
#error "No atomic operations support on this platform (need GCC or C11 atomics)"
#endif
#else
// 非线程安全模式：锁操作定义为空，原子操作为普通变量
#define dmabuf_mutex_t int // 占位类型（实际在结构体中条件编译）
#define dmabuf_mutex_init(m) ((void)0)
#define dmabuf_mutex_destroy(m) ((void)0)
#define dmabuf_mutex_lock(m) ((void)0)
#define dmabuf_mutex_unlock(m) ((void)0)

#define dmabuf_atomic_t uint32_t
#define dmabuf_atomic_load(p) (*(p))
#define dmabuf_atomic_store(p, v) (*(p) = (v))
#define dmabuf_atomic_fetch_add(p, v) (*(p) += (v))
#define dmabuf_atomic_fetch_sub(p, v) (*(p) -= (v))
#define dmabuf_atomic_inc(p) (++(*(p)))
#define dmabuf_atomic_dec(p) (--(*(p)))
#endif

/**
 * @brief 缓冲区，构成缓冲池的最小单位。描述单个缓冲区的信息
 */
typedef struct {
    uint32_t id;               // 缓冲区ID
    size_t size;               // 缓冲区大小
    dmabuf_atomic_t ref_count; // 引用计数初始化为0：分配内存+1 队列持有(就绪)+1
                               // 模块持有+1模块处理完毕-1 队列出队-1 释放内存-1
    bool allocated;            // 是否已分配数据
#if (USE_MALLOC)
    void *data; // 缓冲区实际数据指针
#elif (USE_DMABUF)
    int dmabuf_fd;  // dmabuf文件描述符
    void *mmap_ptr; // mmap映射指针，直接访问
#endif
} dmabuf_buffer_t;
/**
 * @brief
 * 缓冲池，由多个缓冲区构成。描述缓冲池内缓冲区状态和宏观的信息，是缓冲区的组织形式
 */
typedef struct {
    dmabuf_buffer_t *buffers; // 缓冲区数组
    uint32_t capacity;        // 池容量
    uint32_t count;           // 当前数量
    uint32_t next_id;         // 下一个可用的缓冲区id
    dmabuf_mutex_t lock;      // 互斥锁
    char *name;               // 池名称
} dmabuf_pool_t;
/**
 * @brief 缓冲队列，管理缓冲区，是缓冲区的对外接口
 */
typedef struct {
    dmabuf_buffer_t **buffers_ptr; // 指针数组，直接指向缓冲区
    uint32_t capacity;             // 队列容量
    uint32_t head, tail, size;     // 队列头，尾索引，队列长度
    dmabuf_mutex_t lock;           // 互斥锁
    char *name;                    // 队列名称
} dmabuf_queue_t;
/**
 * @brief 监视器结构体
 */
typedef struct {
    dmabuf_pool_t *pool;     // 要监控的池
    dmabuf_queue_t **queues; // 要监控的队列，指针数组
    uint32_t num_queues;     // 队列个数
    char **buffer_blocks;    // 存储格式化后的缓冲区信息块，指针数组
    uint32_t pool_caps;      // 池的容量
} dmabuf_monitor_t;
/**
 * @brief 创建缓冲池
 * @param capacity 缓冲池大小
 * @param name 池名称
 * @return 成功返回dmabuf_pool_t类型缓冲池指针，失败返回NULL
 */
dmabuf_pool_t *dmabuf_pool_create(uint32_t capacity, const char *name);
/**
 * @brief 销毁缓冲池
 * @param pool 缓冲池
 */
void dmabuf_pool_destroy(dmabuf_pool_t *pool);
/**
 * @brief 从缓冲池申请缓冲区，默认为调用者添加一次引用计数，此时引用计数=2（池
 * 调用者）
 * @param pool 缓冲池
 * @param buffer_size 缓冲区大小
 * @return 成功返回dmabuf_buffer_t类型缓冲区指针，失败返回NULL
 */
dmabuf_buffer_t *dmabuf_buffer_alloc(dmabuf_pool_t *pool, size_t buffer_size);
/**
 * @brief
 * 释放缓冲区回到缓冲池，如果引用计数为1,直接释放内存；如果大于1,相当于dmabuf_unref；
 * @param pool 缓冲池
 * @param buffer 缓冲区
 */
void dmabuf_buffer_free(dmabuf_pool_t *pool, dmabuf_buffer_t *buffer);
/**
 * @brief 强制释放缓冲区回到缓冲池，无视引用计数
 * @param pool 缓冲池
 * @param buffer 缓冲区
 */
void dmabuf_buffer_force_free(dmabuf_pool_t *pool, dmabuf_buffer_t *buffer);
/**
 * @brief 添加一个缓冲区引用
 * @param buffer 缓冲区
 */
void dmabuf_ref(dmabuf_buffer_t *buffer);
/**
 * @brief 取消一个缓冲区引用
 * @param buffer 缓冲区
 */
void dmabuf_unref(dmabuf_buffer_t *buffer);
/**
 * @brief 创建队列
 * @param capacity 队列容量
 * @param name 队列名称
 * @return 成功返回dmabuf_queue_t类型队列指针，失败返回NULL
 */
dmabuf_queue_t *dmabuf_queue_create(uint32_t capacity, const char *name);
/**
 * @brief 销毁队列
 * @param queue 队列结构体
 */
void dmabuf_queue_destroy(dmabuf_queue_t *queue);
/**
 * @brief 队列入队
 * @param queue 队列
 * @param buffer 缓冲区
 * @return int 成功返回0，失败返回-1
 */
int dmabuf_queue_enqueue(dmabuf_queue_t *queue, dmabuf_buffer_t *buffer);
/**
 * @brief 队列出队，将队列对缓冲区的引用转移给调用者
 * @param queue 队列
 * @return 出队成功返回dmabuf_buffer_t类型缓冲区指针，失败返回NULL
 */
dmabuf_buffer_t *dmabuf_queue_dequeue(dmabuf_queue_t *queue);
/**
 * @brief 获取队列已就绪长度
 * @param queue 队列
 * @return uint32_t 队列长度，queue中的size
 */
uint32_t dmabuf_queue_length(dmabuf_queue_t *queue);
/**
 * @brief 创建监视器
 * @param pool 要监视的池
 * @param queues 要监视的队列
 * @param num_queues 队列个数
 * @return 成功返回dmabuf_monitor_t类型队列指针，失败返回NULL
 */
dmabuf_monitor_t *dmabuf_monitor_create(dmabuf_pool_t *pool,
                                        dmabuf_queue_t **queues,
                                        uint32_t num_queues);
/**
 * @brief 销毁监视器，释放资源
 * @param monitor 监视器
 */
void dmabuf_monitor_destory(dmabuf_monitor_t *monitor);
/**
 * @brief 实时监控多个缓冲池和队列的资源占用情况
 * @param monitor 监视器
 */
void dmabuf_monitor_usage(dmabuf_monitor_t *monitor);
/*********宏定义函数*************/
// 通过结构体成员获取结构体变量
#define dmabuf_get_parent(ptr, type, member)                                   \
    ((type *)((char *)(ptr) - offsetof(type, member)))
/**
 * @brief 顶部刷新 DMA-BUF 监控信息（从屏幕顶部开始显示，覆盖下方所有内容）
 * @param monitor 指向 dmabuf_monitor_t 的指针
 *
 * 使用 ANSI 转义序列：
 *   \033[1;1H - 移动光标到第1行第1列（屏幕左上角）
 *   \033[0J   - 清除从光标到屏幕底部的内容
 *
 * 该宏适用于全屏实时监控场景，每次刷新时信息从顶部开始显示，
 * 并清除屏幕其余部分，确保只有最新的监控信息可见。
 * 注意：此版本不保留屏幕上方的其他输出，如需保留固定标题，请使用原宏。
 *
 * 示例：
 *     while (1) {
 *         DMABUF_MONITOR_REFRESH(monitor); // 每次刷新全屏显示
 *         usleep(1000000);
 *     }
 */
#define dmabuf_monitor_refresh(monitor)                                        \
    do {                                                                       \
        printf("\033[1;1H"); /* 光标移动到左上角 */                            \
        printf("\033[0J");   /* 清除光标到屏幕底部 */                          \
        dmabuf_monitor_usage(monitor);                                         \
        fflush(stdout);                                                        \
    } while (0)

// 获取缓冲区数据指针，消除使用malloc和dmabuf时的差异
#if (USE_MALLOC)
#define dmabuf_get_data_ptr(buffer) ((buffer)->data)
#elif (USE_DMABUF)
#define dmabuf_get_data_ptr(buffer) ((buffer)->mmap_ptr)
#else
#error "Neither USE_MALLOC nor USE_DMABUF defined"
#endif

#ifdef __cplusplus
}
#endif

#endif