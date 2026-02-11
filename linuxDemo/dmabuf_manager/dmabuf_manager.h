#ifndef _DMABUF_MAN_H
#define _DMABUF_MAN_H

#ifdef __cplusplus
extern "C" {
#endif

// 如果显式定义了 USE_MALLOC 1
#if (USE_MALLOC)
// 使用malloc分配
#define USE_MALLOC 1
// 如果显式定义了 USE_DMABUF 1
#elif (USE_DMABUF)
// 使用dmabuf分配
#define USE_DMABUF 1
#else
// 一个也没定义默认使用dmabuf
#define USE_DMABUF 1
// 如果一个也没有定义，就报错
// #error "Either USE_MALLOC or USE_DMABUF must be defined"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
/**
 * @brief 缓冲区，构成缓冲池的最小单位。描述单个缓冲区的信息
 */
typedef struct {
    uint32_t id;        // 缓冲区ID
    size_t size;        // 缓冲区大小
    uint32_t ref_count; // 引用计数初始化为0：分配内存+1 队列持有(就绪)+1
                        // 模块持有+1模块处理完毕-1 队列出队-1 释放内存-1
    bool allocated;     // 是否已分配数据
#if (USE_MALLOC)
    void *data; // 缓冲区实际数据指针
#elif (USE_DMABUF)
    int dmabuf_fd;  // dmabuf文件描述符
    void *mmap_ptr; // mmap映射指针，直接访问
#endif

#if (USE_MALLOC)
#elif (USE_DMABUF)
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
} dmabuf_pool_t;
/**
 * @brief 缓冲队列，管理缓冲区，是缓冲区的对外接口
 */
typedef struct {
    dmabuf_buffer_t **buffers_ptr; // 指针数组，直接指向缓冲区
    uint32_t capacity;             // 队列容量
    uint32_t head, tail, size;     // 队列头，尾索引，队列长度
} dmabuf_queue_t;
/**
 * @brief 创建缓冲池
 * @param capacity 缓冲池大小
 * @return 成功返回dmabuf_pool_t类型缓冲池指针，失败返回NULL
 */
dmabuf_pool_t *dmabuf_pool_create(uint32_t capacity);
/**
 * @brief 销毁缓冲池
 * @param pool 缓冲池
 */
void dmabuf_pool_destroy(dmabuf_pool_t *pool);
/**
 * @brief 从缓冲池申请缓冲区
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
 * @return 成功返回dmabuf_queue_t类型队列指针，失败返回NULL
 */
dmabuf_queue_t *dmabuf_queue_create(uint32_t capacity);
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
 * @brief 队列出队
 * @param queue 队列
 * @return 出队成功返回dmabuf_buffer_t类型缓冲区指针，失败返回NULL
 */
dmabuf_buffer_t *dmabuf_queue_dequeue(dmabuf_queue_t *queue);

// 通过结构体成员获取结构体变量
#define GET_PARENT(ptr, type, member)                                          \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#ifdef __cplusplus
}
#endif

#endif