/*
 * Copyright 2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */
#ifndef OSSL_QUIC_CC_H
# define OSSL_QUIC_CC_H

#include "openssl/params.h"
#include "internal/time.h"

# ifndef OPENSSL_NO_QUIC

typedef struct ossl_cc_data_st OSSL_CC_DATA;

typedef struct ossl_cc_ack_info_st {
    /* The time the packet being acknowledged was originally sent. */
    OSSL_TIME   tx_time;

    /* The size in bytes of the packet being acknowledged. */
    size_t      tx_size;
} OSSL_CC_ACK_INFO;

typedef struct ossl_cc_loss_info_st {
    /* The time the packet being lost was originally sent. */
    OSSL_TIME   tx_time;

    /* The size in bytes of the packet which has been determined lost. */
    size_t      tx_size;
} OSSL_CC_LOSS_INFO;

typedef struct ossl_cc_ecn_info_st {
    /*
     * The time at which the largest acked PN (in the incoming ACK frame) was
     * sent.
     */
    OSSL_TIME   largest_acked_time;
} OSSL_CC_ECN_INFO;

/* Parameter (read-write): Maximum datagram payload length in bytes. */
#define OSSL_CC_OPTION_MAX_DGRAM_PAYLOAD_LEN        1

/* Diagnostic (read-only): current congestion window size in bytes. */
#define OSSL_CC_OPTION_CUR_CWND_SIZE                2

/* Diagnostic (read-only): minimum congestion window size in bytes. */
#define OSSL_CC_OPTION_MIN_CWND_SIZE                3

/* Diagnostic (read-only): current net bytes in flight. */
#define OSSL_CC_OPTION_CUR_BYTES_IN_FLIGHT          4

/*
 * Congestion control abstract interface.
 *
 * This interface is broadly based on the design described in RFC 9002. However,
 * the demarcation between the ACKM and the congestion controller does not
 * exactly match that delineated in the RFC 9002 psuedocode. Where aspects of
 * the demarcation involve the congestion controller accessing internal state of
 * the ACKM, the interface has been revised where possible to provide the
 * information needed by the congestion controller and avoid needing to give the
 * congestion controller access to the ACKM's internal data structures.
 *
 * Particular changes include:
 *
 *   - In our implementation, it is the responsibility of the ACKM to determine
 *     if a loss event constitutes persistent congestion.
 *
 *   - In our implementation, it is the responsibility of the ACKM to determine
 *     if the ECN-CE counter has increased. The congestion controller is simply
 *     informed when an ECN-CE event occurs.
 *
 * All of these changes are intended to avoid having a congestion controller
 * have to access ACKM internal state.
 *
 */
#define OSSL_CC_LOST_FLAG_PERSISTENT_CONGESTION     (1U << 0)

typedef struct ossl_cc_method_st {
    /*
     * Instantiation.
     */
    OSSL_CC_DATA *(*new)(OSSL_TIME (*now_cb)(void *arg),
                         void *now_cb_arg);

    void (*free)(OSSL_CC_DATA *ccdata);

    /*
     * Reset of state.
     */
    void (*reset)(OSSL_CC_DATA *ccdata);

    /*
     * Escape hatch for option configuration.
     *
     * option_id: One of OSSL_CC_OPTION_*.
     *
     * value: The option value to set.
     *
     * Returns 1 on success and 0 on failure.
     */
    int (*set_option_uint)(OSSL_CC_DATA *ccdata,
                           uint32_t option_id,
                           uint64_t value);

    /*
     * On success, returns 1 and writes the current value of the given option to
     * *value. Otherwise, returns 0.
     */
    int (*get_option_uint)(OSSL_CC_DATA *ccdata,
                           uint32_t option_id,
                           uint64_t *value);

    /*
     * Returns the amount of additional data (above and beyond the data
     * currently in flight) which can be sent in bytes. Returns 0 if no more
     * data can be sent at this time. The return value of this method
     * can vary as time passes.
     */
    uint64_t (*get_tx_allowance)(OSSL_CC_DATA *ccdata);

    /*
     * Returns the time at which the return value of get_tx_allowance might be
     * higher than its current value. This is not a guarantee and spurious
     * wakeups are allowed. Returns ossl_time_infinite() if there is no current
     * wakeup deadline.
     */
    OSSL_TIME (*get_wakeup_deadline)(OSSL_CC_DATA *ccdata);

    /*
     * The On Data Sent event. num_bytes should be the size of the packet in
     * bytes (or the aggregate size of multiple packets which have just been
     * sent).
     */
    int (*on_data_sent)(OSSL_CC_DATA *ccdata,
                        uint64_t num_bytes);

    /*
     * The On Data Acked event. See OSSL_CC_ACK_INFO structure for details
     * of the information to be passed.
     */
    int (*on_data_acked)(OSSL_CC_DATA *ccdata,
                         const OSSL_CC_ACK_INFO *info);

    /*
     * The On Data Lost event. See OSSL_CC_LOSS_INFO structure for details
     * of the information to be passed.
     *
     * Note: When the ACKM determines that a set of multiple packets has been
     * lost, it is useful for a congestion control algorithm to be able to
     * process this as a single loss event rather than multiple loss events.
     * Thus, calling this function may cause the congestion controller to defer
     * state updates under the assumption that subsequent calls to
     * on_data_lost() representing further lost packets in the same loss event
     * may be forthcoming. Always call on_data_lost_finished() after one or more
     * calls to on_data_lost().
     */
    int (*on_data_lost)(OSSL_CC_DATA *ccdata,
                        const OSSL_CC_LOSS_INFO *info);

    /*
     * To be called after a sequence of one or more on_data_lost() calls
     * representing multiple packets in a single loss detection incident.
     *
     * Flags may be 0 or OSSL_CC_LOST_FLAG_PERSISTENT_CONGESTION.
     */
    int (*on_data_lost_finished)(OSSL_CC_DATA *ccdata, uint32_t flags);

    /*
     * For use when a PN space is invalidated or a packet must otherwise be
     * 'undone' for congestion control purposes without acting as a loss signal.
     * Only the size of the packet is needed.
     */
    int (*on_data_invalidated)(OSSL_CC_DATA *ccdata,
                               uint64_t num_bytes);

    /*
     * Called from the ACKM when detecting an increased ECN-CE value in an ACK
     * frame. This indicates congestion.
     *
     * Note that this differs from the RFC's conceptual segregation of the loss
     * detection and congestion controller functions, as in our implementation
     * the ACKM is responsible for detecting increases to ECN-CE and simply
     * tells the congestion controller when ECN-triggered congestion has
     * occurred. This allows a slightly more efficient implementation and
     * narrower interface between the ACKM and CC.
     */
    int (*on_ecn)(OSSL_CC_DATA *ccdata,
                  const OSSL_CC_ECN_INFO *info);
} OSSL_CC_METHOD;

extern const OSSL_CC_METHOD ossl_cc_dummy_method;

# endif

#endif
