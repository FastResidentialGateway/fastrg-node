#ifndef _KAFKA_RETRY_POLICY_H_
#define _KAFKA_RETRY_POLICY_H_

#include <cstddef>
#include <cstdint>

/*
 * Retry bookkeeping for the Kafka WAL. Kept behind a small interface so the
 * decisions can be exercised without a broker, a WAL file or the producer's
 * globals. C++ only: the C side declared in kafka_producer.h never sees this.
 */
class KafkaRetryPolicy {
public:
    /*
     * Collect the indices of the entries whose delivery failed, oldest first
     * and stopping at max_out. Returns how many indices were written; 0 when
     * either buffer is missing.
     */
    static std::size_t select(const unsigned char *failed_flags,
                              std::size_t count,
                              std::size_t *out_indices,
                              std::size_t max_out);

    /*
     * Whether a retry sweep is due. A non-positive interval makes every pass
     * due.
     */
    static bool is_due(std::int64_t now_sec, std::int64_t last_sec,
                       std::int64_t interval_sec);
};

#endif /* _KAFKA_RETRY_POLICY_H_ */
