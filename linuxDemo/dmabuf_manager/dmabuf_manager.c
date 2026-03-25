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
        DEBUG_LOG("减少引用计数: id=%u, old=%u, new=%u", buffer->id, old_val,
                  old_val - 1);
    } else {
        DEBUG_LOG("引用计数已为0,无法减少: id=%u", buffer->id);
    }
}

//===============队列==================
// static inline bool queue_is_empty(dmabuf_queue_t *queue) {
//     return queue ? (queue->size == 0) : true;
// }
// static inline bool queue_is_full(dmabuf_queue_t *queue) {
//     return queue ? (queue->size == queue->capacity) : true;
// }
// static inline uint32_t queue_current_size(dmabuf_queue_t *queue) {
//     return queue ? (queue->size) : 0;
// }

/**
 * @brief 创建缓冲池
 * @param capacity 缓冲池大小
 * @param name 池名称
 * @return 成功返回dmabuf_pool_t类型缓冲池指针，失败返回NULL
 */
dmabuf_pool_t *dmabuf_pool_create(uint32_t capacity, const char *name) {
    if (!capacity) {
        return NULL;
    }
    // 分配缓冲池结构体
    dmabuf_pool_t *pool = (dmabuf_pool_t *)malloc(sizeof(dmabuf_pool_t));
    if (!pool) {
        ERROR_LOG("缓冲池结构体分配失败");
        return NULL;
    }
    if (name) {
        pool->name = strdup(name);
    } else {
        pool->name = NULL;
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
    // 释放缓冲池结构体内存/缓冲区结构体内存
    if (pool->name) {
        free(pool->name);
    }
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
 * @param name 队列名称
 * @return 成功返回dmabuf_queue_t类型队列指针，失败返回NULL
 */
dmabuf_queue_t *dmabuf_queue_create(uint32_t capacity, const char *name) {
    if (capacity == 0) {
        ERROR_LOG("队列容量不能为0");
        return NULL;
    }
    uint32_t real_capacity = capacity + 1;

    // 分配队列结构体
    dmabuf_queue_t *queue = (dmabuf_queue_t *)malloc(sizeof(dmabuf_queue_t));
    if (!queue) {
        ERROR_LOG("队列结构体分配失败");
        return NULL;
    }
    // 将队列命名拷贝进队列结构体
    if (name) {
        queue->name = strdup(name);
    } else {
        queue->name = NULL;
    }
    // 分配队列指针数组（存储指向缓冲区的指针）
    queue->buffers_ptr =
        (dmabuf_buffer_t **)malloc(sizeof(dmabuf_buffer_t *) * capacity);
    if (!queue->buffers_ptr) {
        ERROR_LOG("队列指针数组分配失败");
        if (queue->name)
            free(queue->name);
        free(queue);
        return NULL;
    }
    // 初始化信号量
    if (dmabuf_sem_init(&queue->sem, 0, 0) != 0) {
        ERROR_LOG("信号量初始化失败");
        // 释放已分配资源
        free(queue->buffers_ptr);
        if (queue->name)
            free(queue->name);
        free(queue->name);
        free(queue);
        return NULL;
    }
    // 初始化队列参数
    queue->capacity = capacity;
    queue->real_capacity = real_capacity;
    dmabuf_atomic_store(&queue->head, 0);
    dmabuf_atomic_store(&queue->tail, 0);

    /// 清空指针数组
    for (uint32_t i = 0; i < real_capacity; i++) {
        queue->buffers_ptr[i] = NULL;
    }

    DEBUG_LOG("创建无锁队列成功: capacity=%u, real_capacity=%u", capacity,
              real_capacity);

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
    // 销毁信号量
    dmabuf_sem_destroy(&queue->sem);
    // 只释放队列指针数组，不释放缓冲区
    if (queue->buffers_ptr) {
        free(queue->buffers_ptr);
    }
    if (queue->name) {
        free(queue->name);
    }
    free(queue);
    DEBUG_LOG("销毁队列成功");
}
int dmabuf_queue_wait(dmabuf_queue_t *queue) {
    if (!queue)
        return -1;
    dmabuf_sem_wait_interruptible(&queue->sem); // 自动处理 EINTR
    return 0;
}
/**
 * @brief 队列入队，队列将增加对缓冲区的一次引用
 * @param queue 队列
 * @param buffer 缓冲区
 * @return int 成功返回0，失败返回-1
 */
int dmabuf_queue_enqueue(dmabuf_queue_t *queue, dmabuf_buffer_t *buffer) {
    if (!queue || !buffer) {
        ERROR_LOG("参数错误");
        return -1;
    }

    uint32_t head = dmabuf_atomic_load(&queue->head);
    uint32_t tail = dmabuf_atomic_load(&queue->tail);
    uint32_t next_tail = (tail + 1) % queue->real_capacity;

    // 检查队列是否已满
    if (next_tail == head) {
        DEBUG_LOG("队列已满，无法入队: capacity=%u", queue->capacity);
        return -1;
    }
    // 检查缓冲区是否分配数据
    if (!buffer->allocated) {
        ERROR_LOG("缓冲区未分配，无法入队: id=%u", buffer->id);
        return -1;
    }
    // 入队
    queue->buffers_ptr[queue->tail] = buffer;
    // 更新队列尾和大小
    dmabuf_atomic_store(&queue->tail, next_tail);

    // 增加缓冲区的引用计数（队列持有）
    buffer_inc_ref(buffer);
    uint32_t cur_ref = dmabuf_atomic_load(&buffer->ref_count);
    DEBUG_LOG("入队成功: buffer_id=%u, ref_count=%u", buffer->id, cur_ref);

    dmabuf_sem_post(&queue->sem); // 唤醒一个等待的消费者

    return 0;
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
    uint32_t head = dmabuf_atomic_load(&queue->head);
    uint32_t tail = dmabuf_atomic_load(&queue->tail);

    // 检查队列是否为空
    if (head == tail) {
        DEBUG_LOG("队列为空，无法出队: capacity=%u, size=%u", queue->capacity,
                  queue->size);
        return NULL;
    }

    // 出队
    buffer = queue->buffers_ptr[head];
    if (!buffer) {
        ERROR_LOG("队列指针异常: head=%u 处为 NULL", head);
        return NULL;
    }

    // 清除头索引位置的指针
    queue->buffers_ptr[head] = NULL;
    // 更新头索引和大小
    uint32_t next_head = (head + 1) % queue->real_capacity;
    dmabuf_atomic_store(&queue->head, next_head);

    // 这里给调用者添加一次引用计数，便于在分配但没有使用的间隔被其他线程申请走
    buffer_inc_ref(buffer);
    // 减少引用计数，释放队列引用
    buffer_dec_ref(buffer);

    DEBUG_LOG("出队成功: buffer_id=%u, ref_count=%u", buffer->id,
              dmabuf_atomic_load(&buffer->ref_count));

    return buffer;
}
/**
 * @brief 获取队列已就绪长度
 * @param queue 队列
 * @return uint32_t 队列长度，queue中的size
 */
uint32_t dmabuf_queue_length(dmabuf_queue_t *queue) {
    if (!queue) {
        ERROR_LOG("参数queue不存在");
        return 0;
    }
    uint32_t head = dmabuf_atomic_load(&queue->head);
    uint32_t tail = dmabuf_atomic_load(&queue->tail);
    if (head <= tail) {
        return tail - head;
    } else {
        return queue->real_capacity - head + tail;
    }
}
/**
 * @brief 创建监视器
 * @param pool 要监视的池
 * @param queues 要监视的队列
 * @param num_queues 队列个数
 * @return 成功返回dmabuf_monitor_t类型队列指针，失败返回NULL
 */
dmabuf_monitor_t *dmabuf_monitor_create(dmabuf_pool_t *pool,
                                        dmabuf_queue_t **queues,
                                        uint32_t num_queues) {
    dmabuf_monitor_t *monitor = NULL;
    uint32_t malloced_buffer_block = 0; // 记录已成功分配的缓冲区个数

    // 参数检查
    if (!pool) {
        ERROR_LOG("参数pool不存在\n");
        goto error;
    }
    if (num_queues == 0) {
        ERROR_LOG("队列为空\n");
        goto error;
    }
    // 检查二级指针本身
    if (!queues) {
        ERROR_LOG("指针数组queues不存在\n");
        goto error;
    } else {
        for (uint32_t i = 0; i < num_queues; i++) {
            if (!queues[i]) {
                ERROR_LOG("指针数组queues中的 %u 项不存在\n", i);
                goto error;
            }
        }
    }

    // 创建监视器
    monitor = malloc(sizeof(dmabuf_monitor_t));
    if (!monitor) {
        ERROR_LOG("monitor内存分配失败\n");
        goto error;
    }

    // 写入池信息
    monitor->pool = pool;
    monitor->pool_caps = pool->capacity;
    monitor->queues = queues;
    monitor->num_queues = num_queues;

    // 分配 buffer_blocks 指针数组（大小为 pool_caps）
    monitor->buffer_blocks = malloc(monitor->pool_caps * sizeof(char *));
    if (!monitor->buffer_blocks) {
        goto error;
    }
    // 分配每个字符串缓冲区
    for (uint32_t i = 0; i < monitor->pool_caps; i++) {
        monitor->buffer_blocks[i] = malloc(DMABUF_BUFFER_BLOCK_WIDTH + 1);
        if (!monitor->buffer_blocks[i]) {
            goto error;
        }
        malloced_buffer_block++;
        monitor->buffer_blocks[i][0] = '\0'; // 初始化为空字符串（可选）
    }

    return monitor;
error:
    // 统一错误处理：释放已分配的资源
    if (monitor) {
        if (monitor->buffer_blocks) {
            for (uint32_t i = 0; i < malloced_buffer_block; i++) {
                free(monitor->buffer_blocks[i]);
            }
            free(monitor->buffer_blocks);
        }
        free(monitor);
    }
    return NULL;
}
/**
 * @brief 销毁监视器，释放资源
 * @param monitor 监视器
 */
void dmabuf_monitor_destory(dmabuf_monitor_t *monitor) {
    // 参数检查
    if (!monitor) {
        ERROR_LOG("参数monitor不存在\n");
        return;
    }
    if (monitor->buffer_blocks) {
        for (uint32_t i = 0; i < monitor->pool_caps; i++) {
            if (monitor->buffer_blocks[i])
                free(monitor->buffer_blocks[i]);
        }
        free(monitor->buffer_blocks);
    }
    free(monitor);
}
#include <sys/ioctl.h>
#include <unistd.h>
static int get_terminal_cols(void) {
    int cols = DMABUF_MONITOR_DEFAULT_COLS;
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) {
        cols = ws.ws_col;
    }
#endif
    return cols;
}
/**
 * @brief 获取 DMA-BUF 缓冲区的状态字符
 * @param allocated 是否已分配（布尔值，非0表示已分配）
 * @param ref       引用计数
 * @return 状态字符：
 *         - MONITOR_UNALLOCATE（未分配）
 *         - MONITOR_IDLE（已分配且引用计数为1，空闲）
 *         - MONITOR_BUSY（已分配且引用计数>1，忙碌）
 */
#define DMABUF_BUFFER_STATUS(allocated, ref)                                   \
    (!(allocated)                                                              \
         ? DMABUF_MONITOR_UNALLOCATE_CHAR                                      \
         : ((ref) == 1 ? DMABUF_MONITOR_IDLE_CHAR : DMABUF_MONITOR_BUSY_CHAR))
/**
 * @brief 格式化缓冲区状态块（使用快照数据）
 * @param out 输出缓冲区，长度至少为 BUFFER_BLOCK_WIDTH + 1
 * @param id 缓冲区ID
 * @param allocated 是否已分配
 * @param ref 引用计数（快照值）
 * @param size 缓冲区大小（快照值）
 * @param max_buf_size 当前池中最大已分配缓冲区大小（用于比例计算）
 */
static inline void dmabuf_format_buffer_block(char *out, uint32_t id,
                                              bool allocated, uint32_t ref,
                                              size_t size,
                                              size_t max_buf_size) {
    // 参数检查
    if (!out) {
        ERROR_LOG("输出字符串为空");
        return;
    }
    if (max_buf_size == 0) {
        ERROR_LOG("最大缓冲区大小不能为0");
        return;
    }

    // 状态字符：U=未分配，I=空闲(ref=1)，B=使用中(ref>1)
    char buffer_status = DMABUF_BUFFER_STATUS(allocated, ref);

    // 固定部分长度：'[' + 2位ID + 状态 + ']' = 5
    const int fixed_len = 5;
    const int mem_len = DMABUF_BUFFER_BLOCK_WIDTH - fixed_len;
    if (mem_len < 0) {
        // 宏定义异常，输出简化格式
        snprintf(out, DMABUF_BUFFER_BLOCK_WIDTH + 1, "[%2u%c]", id,
                 buffer_status);
        return;
    }

    // 构造表示内存占用的部分（mem_len 个字符）
    char mem_part[mem_len + 1];
    if (allocated) {
        // 计算 # 个数，按比例映射并钳位
        uint32_t num_hashes =
            (uint32_t)((uint64_t)size * mem_len / max_buf_size);
        if (num_hashes > mem_len)
            num_hashes = mem_len;
        memset(mem_part, DMABUF_MONITOR_USED_CHAR, num_hashes);
        memset(mem_part + num_hashes, DMABUF_MONITOR_FREE_CHAR,
               mem_len - num_hashes);
    } else {
        // 未分配，全部填充 DMABUF_MONITOR_FREE_CHAR
        memset(mem_part, DMABUF_MONITOR_FREE_CHAR, mem_len);
    }
    mem_part[mem_len] = '\0'; // 添加字符串终止符

    // 最终格式化输出
    snprintf(out, DMABUF_BUFFER_BLOCK_WIDTH + 1, "[%2u%c%s]", id, buffer_status,
             mem_part);
}

/**
 * @brief 实时监控缓冲池和队列的资源占用情况（按标志位控制输出内容）
 * @param monitor 监视器
 * @param flags   控制位：bit0=标题，bit1=缓冲区，bit2=队列
 */
void dmabuf_monitor_usage(dmabuf_monitor_t *monitor, uint8_t flags) {
    if (!monitor || !monitor->pool || !monitor->queues ||
        monitor->num_queues == 0) {
        ERROR_LOG("monitor参数错误");
        return;
    }

    // 提取控制位
    bool print_title = flags & 0x01;
    bool print_buffers = flags & 0x02;
    bool print_queues = flags & 0x04;

    // 如果没有任何内容需要打印，直接返回
    if (!print_title && !print_buffers && !print_queues)
        return;

    dmabuf_pool_t *pool = monitor->pool;
    uint32_t cap = pool->capacity;
    int block_width = DMABUF_BUFFER_BLOCK_WIDTH;
    // 获取列数，计算出每列可以放下多少个缓冲区状态块
    int cols = get_terminal_cols();
    int blocks_per_row = cols / block_width;
    if (blocks_per_row < 1)
        blocks_per_row = 1;

    // 快照结构（用于保存缓冲区状态）
    typedef struct {
        uint32_t id;
        bool allocated;
        uint32_t ref_count;
        size_t size;
    } buf_snapshot_t;
    buf_snapshot_t snapshots[cap]; // C99 VLA

    // 统计变量
    uint32_t allocated_count = 0;
    uint32_t active_count = 0;
    size_t max_size = 1; // 避免除零

    // 加锁复制所有缓冲区状态（只有需要标题或缓冲区或队列时才执行）
    dmabuf_mutex_lock(&pool->lock);
    for (uint32_t i = 0; i < cap; i++) {
        dmabuf_buffer_t *buf = &pool->buffers[i];
        snapshots[i].id = buf->id;
        snapshots[i].allocated = buf->allocated;
        snapshots[i].ref_count = dmabuf_atomic_load(&buf->ref_count);
        snapshots[i].size = buf->size;
        if (buf->allocated) {
            allocated_count++;
            if (buf->size > max_size)
                max_size = buf->size;
            if (snapshots[i].ref_count > 1)
                active_count++;
        }
    }
    dmabuf_mutex_unlock(&pool->lock);

    // 如果需要打印缓冲区或队列，则预先填充所有缓冲区状态块
    if (print_buffers || print_queues) {
        for (uint32_t i = 0; i < cap; i++) {
            char *block = monitor->buffer_blocks[i];
            dmabuf_format_buffer_block(
                block, snapshots[i].id, snapshots[i].allocated,
                snapshots[i].ref_count, snapshots[i].size, max_size);
        }
    }

    // 打印标题
    if (print_title) {
        printf("=== DMA-BUF Monitor === %s\n",
               pool->name ? pool->name : "Unnamed");
        uint32_t active_percent = (cap > 0) ? (active_count * 100 / cap) : 0;
        printf("Pool capacity: %u, allocated: %u, active: %u (%u%% used), max "
               "size: %zu bytes\n",
               cap, allocated_count, active_count, active_percent, max_size);
    }

    // 打印缓冲区状态块
    if (print_buffers) {
        for (uint32_t i = 0; i < cap; i++) {
            if (i % blocks_per_row == 0 && i != 0)
                printf("\n");
            printf("%-*s", block_width, monitor->buffer_blocks[i]);
        }
        printf("\n\n");
    }

    // 打印队列信息
    if (print_queues) {
        for (uint32_t q = 0; q < monitor->num_queues; q++) {
            dmabuf_queue_t *queue = monitor->queues[q];
            if (!queue)
                continue;

            // 原子读取 head 和 tail
            uint32_t head = dmabuf_atomic_load(&queue->head);
            uint32_t tail = dmabuf_atomic_load(&queue->tail);
            uint32_t size;
            if (head <= tail)
                size = tail - head;
            else
                size = queue->real_capacity - head + tail;

            printf("Queue %s: %u/%u [", queue->name ? queue->name : "unnamed",
                   size, queue->capacity);

            uint32_t show = size < 10 ? size : 10;
            for (uint32_t j = 0; j < show; j++) {
                uint32_t idx = (head + j) % queue->real_capacity;
                dmabuf_buffer_t *buf = queue->buffers_ptr[idx];
                if (buf) {
                    // 假设 buffer_blocks 已按 id 索引好
                    printf("%s ", monitor->buffer_blocks[buf->id]);
                } else {
                    printf("? ");
                }
            }
            if (size > 10)
                printf(", ...");
            printf("]\n");
        }
    }

    fflush(stdout);
}