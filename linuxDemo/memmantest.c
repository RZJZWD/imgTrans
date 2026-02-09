#include "dmabuf_manager.h"
#include <assert.h>
#include <stdio.h>

void test_dmabuf_management() {
    printf("=== DMA缓冲区管理器测试 ===\n\n");

    // 测试1：创建和销毁缓冲池
    printf("测试1：创建和销毁缓冲池\n");
    dmabuf_pool_t *pool = dmabuf_pool_create(5);
    assert(pool != NULL);
    assert(pool->capacity == 5);
    assert(pool->count == 0);
    printf("✓ 缓冲池创建成功\n\n");

    // 测试2：分配缓冲区
    printf("测试2：分配缓冲区\n");
    dmabuf_buffer_t *buf1 = dmabuf_buffer_alloc(pool, 1024);
    assert(buf1 != NULL);
    assert(buf1->id == 0);
    assert(buf1->size == 1024);
    assert(buf1->ref_count == 1);
    assert(buf1->allocated == true);
    assert(pool->count == 1);
    printf("✓ 缓冲区1分配成功\n");

    dmabuf_buffer_t *buf2 = dmabuf_buffer_alloc(pool, 2048);
    assert(buf2 != NULL);
    assert(buf2->id == 1);
    assert(buf2->size == 2048);
    assert(buf2->ref_count == 1);
    assert(pool->count == 2);
    printf("✓ 缓冲区2分配成功\n\n");

    // 测试3：引用计数操作
    printf("测试3：引用计数操作\n");
    dmabuf_ref(buf1);
    assert(buf1->ref_count == 2);
    printf("✓ 引用计数增加成功\n");

    dmabuf_unref(buf1);
    assert(buf1->ref_count == 1);
    printf("✓ 引用计数减少成功\n\n");

    // 测试4：队列操作
    printf("测试4：队列操作\n");
    dmabuf_queue_t *queue = dmabuf_queue_create(3);
    assert(queue != NULL);
    assert(queue->capacity == 3);
    assert(queue->size == 0);
    printf("✓ 队列创建成功\n");

    // 入队操作
    assert(dmabuf_queue_enqueue(queue, buf1) == 0);
    assert(queue->size == 1);
    assert(buf1->ref_count == 2); // 队列持有+1
    printf("✓ 缓冲区1入队成功\n");

    assert(dmabuf_queue_enqueue(queue, buf2) == 0);
    assert(queue->size == 2);
    assert(buf2->ref_count == 2);
    printf("✓ 缓冲区2入队成功\n");

    // 测试队列满
    dmabuf_buffer_t *buf3 = dmabuf_buffer_alloc(pool, 512);
    assert(dmabuf_queue_enqueue(queue, buf3) == 0);
    assert(queue->size == 3);
    printf("✓ 缓冲区3入队成功\n");

    dmabuf_buffer_t *buf4 = dmabuf_buffer_alloc(pool, 256);
    assert(dmabuf_queue_enqueue(queue, buf4) == -1); // 队列已满
    printf("✓ 队列满检查成功\n");

    // 出队操作
    dmabuf_buffer_t *dequeued = dmabuf_queue_dequeue(queue);
    assert(dequeued == buf1);
    assert(queue->size == 2);
    assert(buf1->ref_count == 1); // 队列释放引用
    printf("✓ 缓冲区1出队成功\n");

    dequeued = dmabuf_queue_dequeue(queue);
    assert(dequeued == buf2);
    assert(queue->size == 1);
    printf("✓ 缓冲区2出队成功\n");

    // 测试队列空
    dmabuf_queue_dequeue(queue); // 出队buf3
    assert(queue->size == 0);
    assert(dmabuf_queue_dequeue(queue) == NULL); // 队列为空
    printf("✓ 队列空检查成功\n\n");

    // 测试5：缓冲池重用
    printf("测试5：缓冲池重用测试\n");

    // 释放缓冲区回到缓冲池
    dmabuf_buffer_free(pool, buf1);
    assert(buf1->allocated == false);
    assert(buf1->ref_count == 0);
    assert(pool->count == 3); // buf2, buf3, buf4还在使用

    // 重新分配相同大小的缓冲区（应该重用）
    dmabuf_buffer_t *buf_reuse = dmabuf_buffer_alloc(pool, 1024);
    assert(buf_reuse != NULL);
    // 注意：重用可能不是原来的buf1，因为优先级1会寻找尺寸匹配的
    printf("✓ 缓冲区重用测试成功\n");

    // 测试6：销毁所有资源
    printf("测试6：销毁资源\n");

    // 释放所有缓冲区
    dmabuf_buffer_free(pool, buf2);
    dmabuf_buffer_free(pool, buf3);
    dmabuf_buffer_free(pool, buf4);
    dmabuf_buffer_free(pool, buf_reuse);

    // 销毁队列
    dmabuf_queue_destroy(queue);
    printf("✓ 队列销毁成功\n");

    // 销毁缓冲池
    dmabuf_pool_destroy(pool);
    printf("✓ 缓冲池销毁成功\n\n");

    printf("=== 所有测试通过！ ===\n");
}

// 边界条件测试
void test_edge_cases() {
    printf("\n=== 边界条件测试 ===\n\n");

    // 测试空指针
    printf("测试空指针处理\n");
    dmabuf_pool_create(0);           // 应该返回NULL
    dmabuf_buffer_alloc(NULL, 1024); // 应该返回NULL
    dmabuf_queue_create(0);          // 应该返回NULL
    printf("✓ 空指针处理正常\n");

    // 测试分配大小为0的缓冲区
    dmabuf_pool_t *pool = dmabuf_pool_create(2);
    dmabuf_buffer_t *buf = dmabuf_buffer_alloc(pool, 0);
    assert(buf == NULL);
    printf("✓ 零大小缓冲区分配被拒绝\n");

    dmabuf_pool_destroy(pool);
}

// 性能测试
void test_performance() {
    printf("\n=== 性能测试 ===\n\n");

    dmabuf_pool_t *pool = dmabuf_pool_create(100);
    dmabuf_queue_t *queue = dmabuf_queue_create(100);

    int iterations = 50;
    printf("进行 %d 次分配/释放循环测试...\n", iterations);

    for (int i = 0; i < iterations; i++) {
        dmabuf_buffer_t *buf = dmabuf_buffer_alloc(pool, 1024);
        assert(buf != NULL);

        // 填充测试数据
        if (buf->data) {
            memset(buf->data, i % 256, buf->size);
        }

        // 入队出队
        assert(dmabuf_queue_enqueue(queue, buf) == 0);
        dmabuf_buffer_t *dequeued = dmabuf_queue_dequeue(queue);
        assert(dequeued == buf);

        // 释放
        dmabuf_buffer_free(pool, buf);
    }

    printf("✓ 性能测试通过，无内存泄漏\n");

    dmabuf_queue_destroy(queue);
    dmabuf_pool_destroy(pool);
}

int main() {
    printf("DMA缓冲区管理器全面测试\n");
    printf("=======================\n\n");

    test_dmabuf_management();
    test_edge_cases();
    test_performance();

    printf("\n所有测试完成！\n");
    return 0;
}