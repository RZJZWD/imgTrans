#include "dmabuf_manager.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#if (USE_DMABUF)
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
// 定义 DMA-Heap 路径
#define DMA_HEAP_SYSTEM_PATH "/dev/dma_heap/system"
#define DMA_HEAP_CMA_PATH "/dev/dma_heap/linux,cma"
#endif
// #define DEBUG
#define ERROR_LOG(fmt, ...)                                                    \
    fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FUNCTION__, __LINE__,        \
            ##__VA_ARGS__)

#ifdef DEBUG
#define __FILENAME__ (strrchr("/" __FILE__, '/') + 1)
#define DEBUG_LOG(user_msg, ...)                                               \
    printf("[%s:%d]" user_msg "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
#define DEBUG_LOG(user_msg, ...)
#endif

//==============内存分配===================
#if (USE_DMABUF)
/**
 * @brief 创建dmabuf缓冲区
 * @param size 大小
 * @return 成功返回dmabuf文件描述符 失败返回-1
 */
static int create_dmabuf(size_t size) {
    int heap_fd = open(DMA_HEAP_SYSTEM_PATH, O_RDWR);
    int dma_fd = -1;
    if (heap_fd < 0) {
        // 尝试CMA
        heap_fd = open(DMA_HEAP_CMA_PATH, O_RDWR);
        if (heap_fd < 0) {
            ERROR_LOG("打开dmabuf失败: %s", strerror(errno));
            return dma_fd;
        }
    }

    struct dma_heap_allocation_data alloc_data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
        ERROR_LOG("DMA-Heap分配失败: %s", strerror(errno));
        close(heap_fd);
        return dma_fd;
    }
    close(heap_fd);
    dma_fd = alloc_data.fd;
    return dma_fd;
}
/**
 * @brief 从dmabuf文件描述符创建内存映射
 * @param dmabuf_fd 文件描述符
 * @param size 内存大小，这里最好和dmabuf保持一致
 * @return 成功返回mmap指针，失败返回NULL
 */
static void *mmap_dmabuf(int dmabuf_fd, size_t size) {
    void *mmap_addr = NULL;
    mmap_addr =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (mmap_addr == MAP_FAILED) {
        ERROR_LOG("映射DMA-BUF失败: %s", strerror(errno));
        return NULL;
    }
    return mmap_addr;
}
#endif
// 对于实际的缓冲区数据分配，使用以下两个函数；结构体不使用
/**
 * @brief 给缓冲区分配数据
 * @param buffer 缓冲区结构体
 * @param size 数据大小
 * @return true 申请成功
 * @return false 申请失败
 */
static bool alloc_mem(dmabuf_buffer_t *buffer, size_t size) {
    if (!buffer) {
        ERROR_LOG("参数buffer不存在");
        return false;
    }
    if (size == 0) {
        ERROR_LOG("参数buffer_size不得为0");
        return false;
    }
#if (USE_MALLOC)
    buffer->data = malloc(size);
    if (buffer->data) {
        buffer->allocated = true;
        buffer->ref_count = 1; // 成功分配内存+1
        buffer->size = size;
        DEBUG_LOG("分配内存成功: id=%u, size=%zu", buffer->id, size);
        return true;
    } else {
        buffer->allocated = false;
        buffer->ref_count = 0;
        buffer->size = 0;
        ERROR_LOG("分配内存失败: id=%u, size=%zu", buffer->id, size);
        return false;
    }
#elif (USE_DMABUF)
    int fd = create_dmabuf(size);
    if (fd < 0) {
        ERROR_LOG("创建dmabuf失败: id=%u, size=%zu", buffer->id, size);
        return false;
    }

    void *mmap_ptr = mmap_dmabuf(fd, size);
    if (mmap_ptr == NULL) {
        ERROR_LOG("映射dmabuf失败: id=%u, fd=%d, size=%zu", buffer->id, fd,
                  size);
        close(fd); // 映射失败，需要关闭文件描述符
        return false;
    }

    buffer->dmabuf_fd = fd;
    buffer->mmap_ptr = mmap_ptr;
    buffer->allocated = true;
    buffer->ref_count = 1; // 成功分配内存+1
    buffer->size = size;
    DEBUG_LOG("分配内存成功: id=%u, size=%zu, fd=%d", buffer->id, size, fd);
    return true;
#endif
}
// 释放buffer内存，但不释放结构体本身
/**
 * @brief 释放缓冲区数据，但不释放结构体
 * @param buffer 缓冲区结构体
 */
static void free_mem(dmabuf_buffer_t *buffer) {
    if (!buffer) {
        ERROR_LOG("free_mem():参数buffer不存在");
        return;
    }

#if (USE_MALLOC)
    if (buffer->data) {
        free(buffer->data);
        buffer->data = NULL;
    }
    buffer->allocated = false;
    buffer->ref_count = 0;
    buffer->size = 0;
    DEBUG_LOG("释放内存: id=%u", buffer->id);
#elif (USE_DMABUF)
    if (buffer->mmap_ptr != NULL) {
        munmap(buffer->mmap_ptr, buffer->size);
        buffer->mmap_ptr = NULL;
    }
    if (buffer->dmabuf_fd >= 0) {
        close(buffer->dmabuf_fd);
        buffer->dmabuf_fd = -1;
    }
    buffer->allocated = false;
    buffer->ref_count = 0;
    buffer->size = 0;
    DEBUG_LOG("释放内存: id=%u", buffer->id);
#endif
}
/**
 * @brief 重分配函数，内部先free再alloc
 * @param buffer 缓冲区结构体
 * @param buffer_size 缓冲区大小
 * @return true 申请成功
 * @return false 申请失败
 */
static inline bool realloc_mem(dmabuf_buffer_t *buffer, size_t buffer_size) {
    free_mem(buffer);
    return alloc_mem(buffer, buffer_size);
}
static inline void buffer_inc_ref(dmabuf_buffer_t *buffer) {
    if (!buffer) {
        DEBUG_LOG("buffer_inc_ref():参数buffer不存在");
        return;
    }
    uint32_t old_val = dmabuf_atomic_inc(&buffer->ref_count);
    DEBUG_LOG("增加引用计数: id=%u, old=%u, new=%u", buffer->id, old_val,
              old_val + 1);
}
static inline void buffer_dec_ref(dmabuf_buffer_t *buffer) {
    if (!buffer) {
        DEBUG_LOG("buffer_dec_ref():参数buffer不存在");
        return;
    }
    uint32_t old_val = dmabuf_atomic_dec(&buffer->ref_count);
    if (old_val > 0) {
        DEBUG_LOG("减少引用计数: id=%u, old=%u, new=%u", buffer->id, old_vald,
                  old - 1);
    } else {
        DEBUG_LOG("引用计数已为0,无法减少: id=%u", buffer->id);
    }
}

//===============队列==================
static inline bool queue_is_empty(dmabuf_queue_t *queue) {
    return queue ? (queue->size == 0) : true;
}
static inline bool queue_is_full(dmabuf_queue_t *queue) {
    return queue ? (queue->size == queue->capacity) : true;
}
static inline uint32_t queue_current_size(dmabuf_queue_t *queue) {
    return queue ? (queue->size) : 0;
}

/**
 * @brief 创建缓冲池
 * @param capacity 缓冲池大小
 * @return 成功返回dmabuf_pool_t类型缓冲池指针，失败返回NULL
 */
dmabuf_pool_t *dmabuf_pool_create(uint32_t capacity) {
    if (!capacity) {
        return NULL;
    }
    // 分配缓冲池结构体
    dmabuf_pool_t *pool = (dmabuf_pool_t *)malloc(sizeof(dmabuf_pool_t));
    if (!pool) {
        ERROR_LOG("缓冲池结构体分配失败");
        return NULL;
    }
    // 分配缓冲池中缓冲区结构体
    pool->buffers =
        (dmabuf_buffer_t *)malloc(sizeof(dmabuf_buffer_t) * capacity);
    if (!pool->buffers) {
        ERROR_LOG("缓冲区结构体分配失败");
        free(pool);
        return NULL;
    }

    // 初始化所有缓冲区结构体
    memset(pool->buffers, 0, sizeof(dmabuf_buffer_t) * capacity);
    for (uint32_t i = 0; i < capacity; i++) {
        pool->buffers[i].id = i;
        pool->buffers[i].allocated = false;
        pool->buffers[i].ref_count = 0;
        pool->buffers[i].size = 0;
#if (USE_MALLOC)
        pool->buffers[i].data = NULL;
#elif (USE_DMABUF)
        pool->buffers[i].dmabuf_fd = -1;
        pool->buffers[i].mmap_ptr = NULL;
#endif
    }
    pool->capacity = capacity;
    pool->count = 0;
    pool->next_id = 0;
    DEBUG_LOG("创建缓冲池成功: capacity=%u", capacity);
    // 初始化互斥锁
    dmabuf_mutex_init(&pool->lock);
    return pool;
}
/**
 * @brief 销毁缓冲池
 * @param pool 缓冲池
 */
void dmabuf_pool_destroy(dmabuf_pool_t *pool) {
    if (!pool) {
        ERROR_LOG("参数pool不存在");
        return;
    }
    // 释放所以已分配的缓冲区数据
    for (uint32_t i = 0; i < pool->capacity; i++) {
        if (pool->buffers[i].allocated) {
            free_mem(&pool->buffers[i]);
        }
    }
    dmabuf_mutex_destroy(&pool->lock);
    // 释放缓冲池/缓冲区结构体内存
    free(pool->buffers);
    free(pool);
    DEBUG_LOG("销毁缓冲池成功");
}
/**
 * @brief 从缓冲池申请缓冲区，默认为调用者添加一次引用计数，此时引用计数=2（池
 * 调用者）
 * @param pool 缓冲池
 * @param buffer_size 缓冲区大小
 * @return 成功返回dmabuf_buffer_t类型缓冲区指针，失败返回NULL
 */
dmabuf_buffer_t *dmabuf_buffer_alloc(dmabuf_pool_t *pool, size_t buffer_size) {
    // 参数检查
    if (!pool) {
        ERROR_LOG("参数pool不存在");
        return NULL;
    }

    if (buffer_size == 0) {
        ERROR_LOG("参数buffer_size不得为0");
        return NULL;
    }
    // 上锁
    dmabuf_mutex_lock(&pool->lock);
    // 初始化函数内参数
    dmabuf_buffer_t *buffer = NULL;         // 缓冲区结构体
    uint32_t mismatched_index = UINT32_MAX; // 尺寸不匹配的空闲缓冲区索引
    bool found_mismatched = false;

    // 优先级1：分配新缓冲区（未分配的缓冲区）
    for (uint32_t i = 0; i < pool->capacity; i++) {
        buffer = &pool->buffers[i];
        if (!buffer->allocated) {
            // 找到未分配的缓冲区，分配内存
            if (alloc_mem(buffer, buffer_size)) {
                pool->count++;
                DEBUG_LOG(
                    "优先级2:分配新缓冲区: id=%u, size=%zu, pool_count=%u",
                    buffer->id, buffer_size, pool->count);
                goto return_buffer;
            } else {
                // 分配失败，继续尝试
                DEBUG_LOG("分配新缓冲区失败: id=%u...继续尝试", buffer->id);
                continue;
            }
        }
    }
    // 优先级2：重新利用空闲缓冲区（已分配，引用计数为1，尺寸匹配）
    for (uint32_t i = 0; i < pool->capacity; i++) {
        buffer = &pool->buffers[i];
        if (buffer->allocated && dmabuf_atomic_load(&buffer->ref_count) == 1) {
            if (buffer->size == buffer_size) {
                // 尺寸匹配，直接重用
                DEBUG_LOG("优先级1:重用空闲缓冲区: id=%u, size=%zu", buffer->id,
                          buffer->size);
                goto return_buffer;
            } else {
                // 记录第一次尺寸不匹配的缓冲区索引
                if (!found_mismatched) {
                    mismatched_index = i;
                    found_mismatched = true;
                }
                DEBUG_LOG(
                    "记录第一个尺寸不匹配的空闲缓冲区: id=%u, old_size=%zu, "
                    "new_size=%zu",
                    buffer->id, buffer->size, buffer_size);
            }
        }
    }

    // 优先级3：重新分配尺寸不符合的空闲缓冲区（已分配，引用计数为1，尺寸不匹配）
    if (found_mismatched) {
        // 尝试已记录的索引分配
        buffer = &pool->buffers[mismatched_index];
        DEBUG_LOG("优先级3:重新分配尺寸不符合的空闲缓冲区: id=%u, "
                  "old_size=%zu, new_size=%zu",
                  buffer->id, buffer->size, buffer_size);
        if (realloc_mem(buffer, buffer_size)) {
            goto return_buffer;
        }
        DEBUG_LOG("重新分配新缓冲区失败: id=%u", buffer->id);
    }

    // 所有缓冲区都在被外部使用（引用计数>1），无法分配
    DEBUG_LOG("缓冲池已满，所有缓冲区都在被外部使用: capacity=%u",
              pool->capacity);
    // 释放锁
    dmabuf_mutex_unlock(&pool->lock);
    return NULL;

return_buffer:
    // 这里给调用者添加一次引用计数，便于再分配但没有使用的间隔被其他线程申请走
    buffer_inc_ref(buffer);
    // 释放锁
    dmabuf_mutex_unlock(&pool->lock);
    return buffer;
}
/**
 * @brief
 * 释放缓冲区回到缓冲池，如果引用计数为1,直接释放内存；如果大于1,相当于dmabuf_unref；
 * @param pool 缓冲池
 * @param buffer 缓冲区
 */
void dmabuf_buffer_free(dmabuf_pool_t *pool, dmabuf_buffer_t *buffer) {
    if (!pool || !buffer) {
        ERROR_LOG("参数错误"); // 可选
        return;
    }

    dmabuf_mutex_lock(&pool->lock);

    if (!buffer->allocated) {
        ERROR_LOG("缓冲区未分配: id=%u", buffer->id);
        goto unlock;
    }
    // 检查引用计数
    uint32_t cur_ref = dmabuf_atomic_load(&buffer->ref_count);
    if (cur_ref != 1) {
        // 当前除了缓冲池还有其他模块持有该缓冲区
        // 手动减一
        buffer_dec_ref(buffer);
        DEBUG_LOG("缓冲区仍在被使用: id=%u, ref_count=%u", buffer->id, cur_ref);
    } else if (cur_ref == 1) {
        // 只有缓冲池持有该缓冲区
        pool->count--;
        free_mem(buffer);
        DEBUG_LOG("释放缓冲区内存: id=%u", buffer->id);
    } else {
        // 引用计数为0，不应该发生，但做防御性处理
        ERROR_LOG("警告:缓冲区引用计数为0但已分配: id=%u ... \n自动释放",
                  buffer->id);
        free_mem(buffer);
    }

unlock:
    dmabuf_mutex_unlock(&pool->lock);
}
/**
 * @brief 强制释放缓冲区回到缓冲池，无视引用计数
 * @param pool 缓冲池
 * @param buffer 缓冲区
 */
void dmabuf_buffer_force_free(dmabuf_pool_t *pool, dmabuf_buffer_t *buffer) {
    if (!pool || !buffer) {
        ERROR_LOG("参数错误"); // 可选
        return;
    }

    dmabuf_mutex_lock(&pool->lock);

    if (!buffer->allocated) {
        ERROR_LOG("缓冲区未分配: id=%u", buffer->id);
        goto unlock;
    }
    DEBUG_LOG("强制释放缓冲区: id=%u ... \n", buffer->id);
    free_mem(buffer);
    pool->count--;

unlock:
    dmabuf_mutex_unlock(&pool->lock);
}
/**
 * @brief 添加一个缓冲区引用
 * @param buffer 缓冲区
 */
void dmabuf_ref(dmabuf_buffer_t *buffer) { buffer_inc_ref(buffer); }
/**
 * @brief 取消一个缓冲区引用
 * @param buffer 缓冲区
 */
void dmabuf_unref(dmabuf_buffer_t *buffer) { buffer_dec_ref(buffer); }
/**
 * @brief 创建队列
 * @param capacity 队列容量
 * @return 成功返回dmabuf_queue_t类型队列指针，失败返回NULL
 */
dmabuf_queue_t *dmabuf_queue_create(uint32_t capacity) {
    if (capacity == 0) {
        ERROR_LOG("队列容量不能为0");
        return NULL;
    }
    // 分配队列结构体
    dmabuf_queue_t *queue = (dmabuf_queue_t *)malloc(sizeof(dmabuf_queue_t));
    if (!queue) {
        ERROR_LOG("队列结构体分配失败");
        return NULL;
    }
    // 分配队列指针数组（存储指向缓冲区的指针）
    queue->buffers_ptr =
        (dmabuf_buffer_t **)malloc(sizeof(dmabuf_buffer_t *) * capacity);
    if (!queue->buffers_ptr) {
        ERROR_LOG("队列指针数组分配失败");
        free(queue);
        return NULL;
    }
    // 初始化队列参数
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;

    // 初始化指针数组为NULL
    memset(queue->buffers_ptr, 0, sizeof(dmabuf_buffer_t *) * capacity);

    DEBUG_LOG("创建队列成功: capacity=%u", capacity);
    dmabuf_mutex_init(&queue->lock);
    return queue;
}
/**
 * @brief 销毁队列
 * @param queue 队列结构体
 */
void dmabuf_queue_destroy(dmabuf_queue_t *queue) {
    if (!queue) {
        ERROR_LOG("参数queue不存在");
        return;
    }
    // 只释放队列指针数组，不释放缓冲区
    if (queue->buffers_ptr) {
        free(queue->buffers_ptr);
    }
    dmabuf_mutex_destroy(&queue->lock);
    free(queue);
    DEBUG_LOG("销毁队列成功");
}
/**
 * @brief 队列入队
 * @param queue 队列
 * @param buffer 缓冲区
 * @return int 成功返回0，失败返回-1
 */
int dmabuf_queue_enqueue(dmabuf_queue_t *queue, dmabuf_buffer_t *buffer) {
    if (!queue || !buffer) {
        ERROR_LOG("参数错误");
        return -1;
    }
    int ret = 0;

    dmabuf_mutex_lock(&queue->lock);

    // 检查队列是否已满
    if (queue_is_full(queue)) {
        DEBUG_LOG("队列已满，无法入队: capacity=%u, size=%u", queue->capacity,
                  queue->size);
        ret = -1;
        goto unlock;
    }
    // 检查缓冲区是否分配数据
    if (!buffer->allocated) {
        ERROR_LOG("缓冲区未分配，无法入队: id=%u", buffer->id);
        ret = -1;
        goto unlock;
    }
    // 入队
    queue->buffers_ptr[queue->tail] = buffer;
    // 增加缓冲区的引用计数（队列持有）
    buffer_inc_ref(buffer);
    uint32_t cur_ref = dmabuf_atomic_load(&buffer->ref_count);
    DEBUG_LOG("入队成功: buffer_id=%u, ref_count=%u", buffer->id, cur_ref);

    // 更新队列尾和大小
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;
    DEBUG_LOG("入队完成: 队列size=%u, head=%u, tail=%u", queue->size,
              queue->head, queue->tail);
    ret = 0;

unlock:
    dmabuf_mutex_unlock(&queue->lock);
    return ret;
}
/**
 * @brief 队列出队，将队列对缓冲区的引用转移给调用者
 * @param queue 队列
 * @return 出队成功返回dmabuf_buffer_t类型缓冲区指针，失败返回NULL
 */
dmabuf_buffer_t *dmabuf_queue_dequeue(dmabuf_queue_t *queue) {
    if (!queue) {
        ERROR_LOG("参数queue不存在");
        return NULL;
    }
    dmabuf_buffer_t *buffer = NULL;

    dmabuf_mutex_lock(&queue->lock);
    // 检查队列是否为空
    if (queue_is_empty(queue)) {
        DEBUG_LOG("队列为空，无法出队: capacity=%u, size=%u", queue->capacity,
                  queue->size);
        goto unlock;
    }

    // 出队
    buffer = queue->buffers_ptr[queue->head];
    // 清除头索引位置的指针
    queue->buffers_ptr[queue->head] = NULL;
    uint32_t cur_ref = dmabuf_atomic_load(&buffer->ref_count);
    DEBUG_LOG("出队: buffer_id=%u, ref_count=%u", buffer->id, cur_ref);

    // 这里给调用者添加一次引用计数，便于在分配但没有使用的间隔被其他线程申请走
    buffer_inc_ref(buffer);
    // 减少引用计数，释放队列引用
    buffer_dec_ref(buffer);

    // 更新头索引和大小
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    DEBUG_LOG("出队完成: 队列size=%u, head=%u, tail=%u", queue->size,
              queue->head, queue->tail);

unlock:
    dmabuf_mutex_unlock(&queue->lock);
    return buffer;
}