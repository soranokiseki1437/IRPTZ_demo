#ifndef THREADSAFEQUEUE_H
#define THREADSAFEQUEUE_H

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>
#include <QByteArray>

template<typename T>
class ThreadSafeQueue
{
public:
    ThreadSafeQueue() : m_maxSize(100), m_stopped(false) {}

    void setMaxSize(int maxSize) { QMutexLocker locker(&m_mutex); m_maxSize = maxSize; }

    void push(const T &item)
    {
        QMutexLocker locker(&m_mutex);
        if (m_stopped) return;

        if (m_queue.size() >= m_maxSize) {
            m_queue.dequeue();
            ++m_dropped;        // 队列满：丢最旧帧并计数，供上层评估输入质量
        }
        m_queue.enqueue(item);
        m_cond.wakeOne();
    }

    bool waitAndPop(T &item, unsigned long timeoutMs = ULONG_MAX)
    {
        QMutexLocker locker(&m_mutex);
        while (m_queue.isEmpty() && !m_stopped) {
            if (timeoutMs != ULONG_MAX) {
                if (!m_cond.wait(&m_mutex, timeoutMs)) {
                    return false;
                }
            } else {
                m_cond.wait(&m_mutex);
            }
        }
        if (m_stopped && m_queue.isEmpty()) return false;
        item = m_queue.dequeue();
        return true;
    }

    void stop()
    {
        QMutexLocker locker(&m_mutex);
        m_stopped = true;
        m_cond.wakeAll();
    }

    // C6：复位 stopped 标志并清空残留数据，用于线程复活（DecodeThread::restart）
    void reset()
    {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
        m_stopped = false;
        m_dropped = 0;
    }

    bool isStopped() const { QMutexLocker locker(&m_mutex); return m_stopped; }
    void clear() { QMutexLocker locker(&m_mutex); m_queue.clear(); }
    int size() const { QMutexLocker locker(&m_mutex); return m_queue.size(); }
    bool isEmpty() const { QMutexLocker locker(&m_mutex); return m_queue.isEmpty(); }

    // 丢帧统计（队列满时丢最旧帧的次数）
    int droppedCount() const { QMutexLocker locker(&m_mutex); return m_dropped; }
    void resetDropped() { QMutexLocker locker(&m_mutex); m_dropped = 0; }

private:
    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    QQueue<T> m_queue;
    int m_maxSize;
    bool m_stopped;
    int m_dropped = 0;
};

#endif // THREADSAFEQUEUE_H
