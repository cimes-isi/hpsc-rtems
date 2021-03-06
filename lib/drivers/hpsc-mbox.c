#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include <rtems.h>
#include <rtems/bspIo.h>
#include <rtems/irq-extension.h>

#include "hpsc-mbox.h"

#ifdef HPSC_MBOX_DEBUG
#define HPSC_MBOX_DBG(...) printk(__VA_ARGS__)
#else
#define HPSC_MBOX_DBG(...)
#endif

#define REG_CONFIG__UNSECURE      0x1
#define REG_CONFIG__OWNER__SHIFT  8
#define REG_CONFIG__OWNER__MASK   0x0000ff00
#define REG_CONFIG__SRC__SHIFT    16
#define REG_CONFIG__SRC__MASK     0x00ff0000
#define REG_CONFIG__DEST__SHIFT   24
#define REG_CONFIG__DEST__MASK    0xff000000

#define HPSC_MBOX_EVENT_A 0x1
#define HPSC_MBOX_EVENT_B 0x2

#define HPSC_MBOX_INT_A(idx) (1 << (2 * (idx)))      // rcv (map event A to int 'idx')
#define HPSC_MBOX_INT_B(idx) (1 << (2 * (idx) + 1))  // ack (map event B to int 'idx')

struct hpsc_mbox_chan_base {
    uint32_t CONFIG;
    union {
        uint32_t EVENT_CAUSE;
        uint32_t EVENT_STATUS_CLEAR;
    };
    union {
        uint32_t EVENT_STATUS;
        uint32_t EVENT_STATUS_SET;
    };
    uint32_t EVENT_ENABLE;
    uint32_t DATA[HPSC_MBOX_DATA_REGS];
};

struct hpsc_mbox_chan_irq_info {
    rtems_interrupt_handler cb;
    void *arg;
};

// Locking enforces that a channel is not claimed/released during an ISR while
// its status is being determined or its callback executed (on event match).
// Users must therefore synchronize channel claim/release to avoid deadlocks.
struct hpsc_mbox_chan {
    // static fields
    struct hpsc_mbox *mbox;
    volatile struct hpsc_mbox_chan_base *base;
    unsigned instance;
    rtems_interrupt_lock lock;
    // dynamic fields
    struct hpsc_mbox_chan_irq_info int_a;
    struct hpsc_mbox_chan_irq_info int_b;
    uint8_t owner;
    uint8_t src;
    uint8_t dest;
    bool active;
};

struct hpsc_mbox_irq_info {
    struct hpsc_mbox *mbox;
    rtems_vector_number n;
    unsigned idx;
};

struct hpsc_mbox {
    struct hpsc_mbox_chan chans[HPSC_MBOX_CHANNELS];
    const char *info;
    uintptr_t base;
    struct hpsc_mbox_irq_info int_a;
    struct hpsc_mbox_irq_info int_b;
};

static void hpsc_mbox_chan_init(struct hpsc_mbox_chan *chan,
                                uint8_t owner, uint8_t src, uint8_t dest,
                                rtems_interrupt_handler cb_a,
                                rtems_interrupt_handler cb_b,
                                void *cb_arg)
{
    chan->base = (volatile struct hpsc_mbox_chan_base *)
        (chan->mbox->base + chan->instance * sizeof(struct hpsc_mbox_chan_base));
    chan->int_a.cb = cb_a;
    chan->int_a.arg = cb_arg;
    chan->int_b.cb = cb_b;
    chan->int_b.arg = cb_arg;
    chan->owner = owner;
    chan->src = src;
    chan->dest = dest;
    chan->active = true;
}

static void hpsc_mbox_chan_destroy(struct hpsc_mbox_chan *chan)
{
    chan->int_a.cb = NULL;
    chan->int_a.arg = NULL;
    chan->int_b.cb = NULL;
    chan->int_b.arg = NULL;
    chan->owner = 0;
    chan->src = 0;
    chan->dest = 0;
    chan->active = false;
}

static void hpsc_mbox_chan_config_read(
    struct hpsc_mbox *mbox,
    unsigned instance,
    uint8_t *owner,
    uint8_t *src,
    uint8_t *dest
)
{
    uint32_t val;
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);

    HPSC_MBOX_DBG("MBOX: %s: %u: read config\n", mbox->info, instance);
    val = mbox->chans[instance].base->CONFIG;
    if (owner)
        *owner = (val & REG_CONFIG__OWNER__MASK) >> REG_CONFIG__OWNER__SHIFT;
    if (src)
        *src =  (val & REG_CONFIG__SRC__MASK) >> REG_CONFIG__SRC__SHIFT;
    if (dest)
        *dest = (val & REG_CONFIG__DEST__MASK) >> REG_CONFIG__DEST__SHIFT;
}

static rtems_status_code hpsc_mbox_chan_config_write(
    struct hpsc_mbox *mbox,
    unsigned instance,
    uint8_t owner,
    uint8_t src,
    uint8_t dest
)
{
    uint32_t cfg;
    uint32_t cfg_hw;
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);

    cfg = REG_CONFIG__UNSECURE |
        ((owner << REG_CONFIG__OWNER__SHIFT) & REG_CONFIG__OWNER__MASK) |
        ((src << REG_CONFIG__SRC__SHIFT)     & REG_CONFIG__SRC__MASK) |
        ((dest  << REG_CONFIG__DEST__SHIFT)  & REG_CONFIG__DEST__MASK);
    HPSC_MBOX_DBG("MBOX: %s: %u: write config\n", mbox->info, instance);
    mbox->chans[instance].base->CONFIG = cfg;
    cfg_hw = mbox->chans[instance].base->CONFIG;
    if (cfg_hw != cfg) {
        printk("hpsc_mbox_chan_config_write: failed to write chan %u for %x: "
               "already owned by %x\n", instance, owner,
               (cfg_hw & REG_CONFIG__OWNER__MASK) >> REG_CONFIG__OWNER__SHIFT);
        return RTEMS_NOT_OWNER_OF_RESOURCE;
    }
    return RTEMS_SUCCESSFUL;
}

static void hpsc_mbox_chan_reset(struct hpsc_mbox *mbox, unsigned instance)
{
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);
    // clearing owner also clears destination (resets the instance)
    HPSC_MBOX_DBG("MBOX: %s: %u: reset config\n", mbox->info, instance);
    mbox->chans[instance].base->CONFIG = 0;
}

rtems_status_code hpsc_mbox_chan_claim(
    struct hpsc_mbox *mbox,
    unsigned instance,
    uint8_t owner,
    uint8_t src,
    uint8_t dest,
    rtems_interrupt_handler cb_a,
    rtems_interrupt_handler cb_b,
    void *cb_arg
)
{
    uint32_t val = 0;
    uint8_t src_hw;
    uint8_t dest_hw;
    struct hpsc_mbox_chan *chan;
    rtems_interrupt_lock_context lock_context;
    rtems_status_code sc;
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);

    HPSC_MBOX_DBG("MBOX: %s: %u: claim\n", mbox->info, instance);
    if (rtems_interrupt_is_in_progress())
        return RTEMS_CALLED_FROM_ISR;

    rtems_interrupt_lock_acquire(&chan->lock, &lock_context);
    chan = &mbox->chans[instance];
    if (chan->active) {
        sc = RTEMS_RESOURCE_IN_USE;
        goto cleanup;
    }

    hpsc_mbox_chan_init(chan, owner, src, dest, cb_a, cb_b, cb_arg);
    if (chan->owner) {
        sc = hpsc_mbox_chan_config_write(mbox, instance, owner, src, dest);
        if (sc != RTEMS_SUCCESSFUL)
            goto cleanup;
    } else { // not owner, just check the value in registers against the requested value
        hpsc_mbox_chan_config_read(mbox, instance, NULL, &src_hw, &dest_hw);
        if (cb_b && src && src_hw != src) {
            printk("hpsc_mbox_chan_claim: failed to claim mailbox %u: "
                   "src mismatch: %x (expected %x)\n",
                   chan->instance, src, src_hw);
            sc = RTEMS_UNSATISFIED;
            goto cleanup;
        }
        if (cb_a && dest && dest_hw != src) {
            printk("hpsc_mbox_chan_claim: failed to claim mailbox %u: "
                   "dest mismatch: %x (expected %x)\n",
                   chan->instance, dest, dest_hw);
            sc = RTEMS_UNSATISFIED;
            goto cleanup;
        }
    }
    if (chan->int_a.cb)
        val |= HPSC_MBOX_INT_A(chan->mbox->int_a.idx);
    if (chan->int_b.cb)
        val |= HPSC_MBOX_INT_B(chan->mbox->int_b.idx);
    HPSC_MBOX_DBG("MBOX: %s: %u: enable interrupts\n", mbox->info, instance);
    chan->base->EVENT_ENABLE |= val;

    rtems_interrupt_lock_release(&chan->lock, &lock_context);
    return RTEMS_SUCCESSFUL;
cleanup:
    hpsc_mbox_chan_destroy(chan);
    rtems_interrupt_lock_release(&chan->lock, &lock_context);
    return sc;
}

rtems_status_code hpsc_mbox_chan_release(
    struct hpsc_mbox *mbox,
    unsigned instance
)
{
    struct hpsc_mbox_chan *chan;
    rtems_interrupt_lock_context lock_context;
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);

    HPSC_MBOX_DBG("MBOX: %s: %u: release\n", mbox->info, instance);
    if (rtems_interrupt_is_in_progress())
        return RTEMS_CALLED_FROM_ISR;

    chan = &mbox->chans[instance];
    rtems_interrupt_lock_acquire(&chan->lock, &lock_context);
    chan->base->EVENT_ENABLE &= ~(HPSC_MBOX_INT_A(mbox->int_a.idx) |
                                  HPSC_MBOX_INT_B(mbox->int_b.idx));
    if (chan->owner)
        hpsc_mbox_chan_reset(mbox, instance);
    hpsc_mbox_chan_destroy(chan);
    rtems_interrupt_lock_release(&chan->lock, &lock_context);
    return RTEMS_SUCCESSFUL;
}

size_t hpsc_mbox_chan_write(
    struct hpsc_mbox *mbox,
    unsigned instance,
    const void *buf,
    size_t sz
)
{
    struct hpsc_mbox_chan *chan;
    const uint32_t *msg = buf;
    size_t len;
    size_t i;
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);
    assert(buf);
    assert(sz <= HPSC_MBOX_DATA_SIZE);

    HPSC_MBOX_DBG("MBOX: %s: %u: write\n", mbox->info, instance);
    chan = &mbox->chans[instance];

    len = sz / sizeof(uint32_t);
    if (sz % sizeof(uint32_t))
        len++;

    for (i = 0; i < len; i++)
        chan->base->DATA[i] = msg[i];
    // zero out any remaining registers
    for (; i < HPSC_MBOX_DATA_REGS; i++)
        chan->base->DATA[i] = 0;

    return sz;
}

size_t hpsc_mbox_chan_read(
    struct hpsc_mbox *mbox,
    unsigned instance,
    void *buf,
    size_t sz
)
{
    struct hpsc_mbox_chan *chan;
    uint32_t *msg = buf;
    size_t len;
    size_t i;
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);
    assert(buf);
    // assert(sz >= HPSC_MBOX_DATA_SIZE); // not a strict requirement

    HPSC_MBOX_DBG("MBOX: %s: %u: read\n", mbox->info, instance);
    chan = &mbox->chans[instance];

    len = sz / sizeof(uint32_t);
    if (sz % sizeof(uint32_t))
        len++;

    for (i = 0; i < len && i < HPSC_MBOX_DATA_REGS; i++)
        msg[i] = chan->base->DATA[i];

    return i * sizeof(uint32_t);
}

void hpsc_mbox_chan_event_set_rcv(struct hpsc_mbox *mbox, unsigned instance)
{
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);
    HPSC_MBOX_DBG("MBOX: %s: %u: raise int A\n", mbox->info, instance);
    mbox->chans[instance].base->EVENT_STATUS_SET = HPSC_MBOX_EVENT_A;
}

void hpsc_mbox_chan_event_set_ack(struct hpsc_mbox *mbox, unsigned instance)
{
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);
    HPSC_MBOX_DBG("MBOX: %s: %u: raise int B\n", mbox->info, instance);
    mbox->chans[instance].base->EVENT_STATUS_SET = HPSC_MBOX_EVENT_B;
}

void hpsc_mbox_chan_event_clear_rcv(struct hpsc_mbox *mbox, unsigned instance)
{
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);
    HPSC_MBOX_DBG("MBOX: %s: %u: clear int A\n", mbox->info, instance);
    mbox->chans[instance].base->EVENT_STATUS_CLEAR = HPSC_MBOX_EVENT_A;
}

void hpsc_mbox_chan_event_clear_ack(struct hpsc_mbox *mbox, unsigned instance)
{
    assert(mbox);
    assert(instance < HPSC_MBOX_CHANNELS);
    HPSC_MBOX_DBG("MBOX: %s: %u: clear int B\n", mbox->info, instance);
    mbox->chans[instance].base->EVENT_STATUS_CLEAR = HPSC_MBOX_EVENT_B;
}

static void hpsc_mbox_chan_isr_a(struct hpsc_mbox_chan *chan)
{
    assert(chan);
    if (chan->int_a.cb)
        chan->int_a.cb(chan->int_a.arg);
    else
        // just clear event, but don't ACK
        hpsc_mbox_chan_event_clear_rcv(chan->mbox, chan->instance);
}

static void hpsc_mbox_chan_isr_b(struct hpsc_mbox_chan *chan)
{
    assert(chan);
    if (chan->int_b.cb)
        chan->int_b.cb(chan->int_b.arg);
    else
        // just clear event
        hpsc_mbox_chan_event_clear_ack(chan->mbox, chan->instance);
}

static bool hpsc_mbox_chan_is_subscribed(struct hpsc_mbox_chan *chan,
                                         unsigned event,
                                         unsigned interrupt)
{
    if (!chan->active)
        return false;
    HPSC_MBOX_DBG("MBOX: %s: %u: check event subscription: %u\n",
                  chan->mbox->info, chan->instance, event);
    // Are we 'signed up' for this event (A) from this channel?
    // Two criteria: (1) Cause is set, and (2) Mapped to our IRQ
    if (!(chan->base->EVENT_CAUSE & event))
        return false; // this mailbox didn't raise the interrupt
    if (!(chan->base->EVENT_ENABLE & interrupt))
        return false; // this mailbox has an event but it's not ours
    return true;
}

static void hpsc_mbox_isr(struct hpsc_mbox *mbox, unsigned event,
                          unsigned interrupt,
                          void (*cb)(struct hpsc_mbox_chan *))
{
    struct hpsc_mbox_chan *chan;
    rtems_interrupt_lock_context lock_context;
    size_t i;
    bool handled = false;
    assert(mbox);
    assert(cb);
    for (i = 0; i < RTEMS_ARRAY_SIZE(mbox->chans); i++) {
        chan = &mbox->chans[i];
        rtems_interrupt_lock_acquire_isr(&chan->lock, &lock_context);
        if (hpsc_mbox_chan_is_subscribed(chan, event, interrupt)) {
            handled = true;
            cb(chan);
        }
        rtems_interrupt_lock_release_isr(&chan->lock, &lock_context);
    }
    if (!handled)
        printk("MBOX: %s: WARN: no matching event for interrupt", mbox->info);
    // assert(handled); // probably not worth forcing a runtime panic over
}

static void hpsc_mbox_isr_a(void *arg)
{
    struct hpsc_mbox_irq_info *info = (struct hpsc_mbox_irq_info *)arg;
    assert(info);
    HPSC_MBOX_DBG("MBOX: %s: ISR A\n", info->mbox->info);
    hpsc_mbox_isr(info->mbox, HPSC_MBOX_EVENT_A,
                  HPSC_MBOX_INT_A(info->mbox->int_a.idx), hpsc_mbox_chan_isr_a);
}
static void hpsc_mbox_isr_b(void *arg)
{
    struct hpsc_mbox_irq_info *info = (struct hpsc_mbox_irq_info *)arg;
    assert(info);
    HPSC_MBOX_DBG("MBOX: %s: ISR B\n", info->mbox->info);
    hpsc_mbox_isr(info->mbox, HPSC_MBOX_EVENT_B,
                  HPSC_MBOX_INT_B(info->mbox->int_b.idx), hpsc_mbox_chan_isr_b);
}

static void hpsc_mbox_init(
    struct hpsc_mbox *mbox,
    const char *info,
    uintptr_t base,
    rtems_vector_number int_a,
    unsigned int_idx_a,
    rtems_vector_number int_b,
    unsigned int_idx_b
)
{
    size_t i;
    mbox->info = info;
    mbox->base = base;
    mbox->int_a.mbox = mbox;
    mbox->int_a.n = int_a;
    mbox->int_a.idx = int_idx_a;
    mbox->int_b.mbox = mbox;
    mbox->int_b.n = int_b;
    mbox->int_b.idx = int_idx_b;
    for (i = 0; i < RTEMS_ARRAY_SIZE(mbox->chans); i++) {
        mbox->chans[i].mbox = mbox;
        mbox->chans[i].instance = i;
        rtems_interrupt_lock_initialize(&mbox->chans[i].lock, NULL);
        mbox->chans[i].active = false;
        // other fields set when channel is claimed, cleared on release
    }
}

rtems_status_code hpsc_mbox_probe(
    struct hpsc_mbox **mbox,
    const char *info,
    uintptr_t base,
    rtems_vector_number int_a,
    unsigned int_idx_a,
    rtems_vector_number int_b,
    unsigned int_idx_b
)
{
    rtems_status_code sc;
    assert(mbox);
    assert(info);
    assert(base);

    HPSC_MBOX_DBG("MBOX: %s: probe\n", info);
    HPSC_MBOX_DBG("\tbase: 0x%"PRIxPTR"\n", base);
    HPSC_MBOX_DBG("\tirq_a: %u\n", int_a);
    HPSC_MBOX_DBG("\tidx_a: %u\n", int_idx_a);
    HPSC_MBOX_DBG("\tirq_b: %u\n", int_b);
    HPSC_MBOX_DBG("\tidx_b: %u\n", int_idx_b);

    *mbox = malloc(sizeof(struct hpsc_mbox));
    if (!*mbox)
        return RTEMS_NO_MEMORY;

    // init struct
    hpsc_mbox_init(*mbox, info, base, int_a, int_idx_a, int_b, int_idx_b);

    // setup interrupt handlers
    sc = rtems_interrupt_handler_install(int_a, info, RTEMS_INTERRUPT_UNIQUE,
                                         hpsc_mbox_isr_a, &(*mbox)->int_a);
    if (sc != RTEMS_SUCCESSFUL) {
        printk("hpsc_mbox_probe: failed to install interrupt handler A\n");
        goto free_mbox;
    }
    sc = rtems_interrupt_handler_install(int_b, info, RTEMS_INTERRUPT_UNIQUE,
                                         hpsc_mbox_isr_b, &(*mbox)->int_b);
    if (sc != RTEMS_SUCCESSFUL) {
        printk("hpsc_mbox_probe: failed to install interrupt handler B\n");
        goto fail_isr_b;
    }

    return sc;
fail_isr_b:
    rtems_interrupt_handler_remove((*mbox)->int_a.n, hpsc_mbox_isr_a,
                                   &(*mbox)->int_a);
free_mbox:
    free(*mbox);
    return sc;
}

rtems_status_code hpsc_mbox_remove(struct hpsc_mbox *mbox)
{
    rtems_status_code sc;
    size_t i;
    assert(mbox);

    HPSC_MBOX_DBG("MBOX: %s: remove\n", mbox->info);

    for (i = 0; i < RTEMS_ARRAY_SIZE(mbox->chans); i++)
        hpsc_mbox_chan_release(mbox, i);

    // we can correctly assert handler removal since we installed them
    sc = rtems_interrupt_handler_remove(mbox->int_a.n, hpsc_mbox_isr_a,
                                        &mbox->int_a);
    assert(sc == RTEMS_SUCCESSFUL);
    sc = rtems_interrupt_handler_remove(mbox->int_b.n, hpsc_mbox_isr_b,
                                        &mbox->int_b);
    assert(sc == RTEMS_SUCCESSFUL);
    free(mbox);
    return sc;
}
