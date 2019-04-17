#include <assert.h>
#include <assert.h>

#include <rtems.h>
#include <rtems/bspIo.h>

#include "command.h"
#include "link.h"

size_t link_send(struct link *link, void *buf, size_t sz)
{
    link->rctx.tx_acked = false;
    return link->write(link, buf, sz);
}

bool link_is_send_acked(struct link *link)
{
    return link->rctx.tx_acked;
}

ssize_t link_request(struct link *link,
                     int wtimeout_ms, void *wbuf, size_t wsz,
                     int rtimeout_ms, void *rbuf, size_t rsz)
{
    rtems_interval ticks;
    rtems_event_set events;
    ssize_t rc;

    printk("%s: request\n", link->name);
    link->rctx.reply_sz_read = 0;
    link->rctx.reply = rbuf;
    link->rctx.reply_sz = rsz;

    rc = link_send(link, wbuf, wsz);
    if (!rc) {
        printk("%s: request: send failed\n", link->name);
        return -1;
    }
    link->rctx.tid_requester = rtems_task_self();

    printk("%s: request: waiting for ACK...\n", link->name);
    ticks = wtimeout_ms < 0 ? RTEMS_NO_TIMEOUT :
        RTEMS_MILLISECONDS_TO_TICKS(wtimeout_ms);
    events = 0;
    rtems_event_receive(RTEMS_EVENT_0, RTEMS_EVENT_ANY, ticks, &events);
    if (events & RTEMS_EVENT_0) {
        printk("%s: request: ACK received\n", link->name);
        assert(link->rctx.tx_acked);
    } else {
        printk("%s: request: timed out waiting for ACK...\n", link->name);
        rc = -1; // send timeout (considered a send failure)
        goto out;
    }

    printk("%s: request: waiting for reply...\n", link->name);
    ticks = rtimeout_ms < 0 ? RTEMS_NO_TIMEOUT :
        RTEMS_MILLISECONDS_TO_TICKS(wtimeout_ms);
    events = 0;
    rtems_event_receive(RTEMS_EVENT_1, RTEMS_EVENT_ANY, ticks, &events);
    if (events & RTEMS_EVENT_1) {
        printk("%s: request: reply received\n", link->name);
        rc = link->rctx.reply_sz_read;
        assert(rc);
    } else {
        printk("%s: request: timed out waiting for reply...\n", link->name);
        rc = 0;
    }

out:
    link->rctx.tid_requester = RTEMS_ID_NONE;
    return rc;
}

int link_disconnect(struct link *link)
{
    return link->close(link);
}

void link_init(struct link *link, const char *name, void *priv)
{
    link->rctx.tid_requester = RTEMS_ID_NONE;
    link->rctx.tx_acked = false;
    link->rctx.reply = NULL;
    link->name = name;
    link->priv = priv;
}

void link_recv_cmd(void *arg)
{
    struct link *link = arg;
    struct cmd cmd = {
        .link = link,
        .msg = { 0 }
    };
    printk("%s: recv_cmd\n", link->name);
    link->read(link, cmd.msg, sizeof(cmd.msg));
    if (cmd_enqueue(&cmd))
        rtems_panic("%s: recv_cmd: failed to enqueue command", link->name);
}

void link_recv_reply(void *arg)
{
    struct link *link = arg;
    rtems_status_code sc;
    printk("%s: recv_reply\n", link->name);
    link->rctx.reply_sz_read = link->read(link, link->rctx.reply,
                                          link->rctx.reply_sz);
    if (link->rctx.tid_requester != RTEMS_ID_NONE) {
        sc = rtems_event_send(link->rctx.tid_requester, RTEMS_EVENT_1);
        if (sc != RTEMS_SUCCESSFUL)
            // there was a race with reply timeout and clearing tid_requester
            rtems_panic("%s: recv_reply: failed to send reply to listening task",
                        link->name);
    }
}

void link_ack(void *arg)
{
    struct link *link = arg;
    rtems_status_code sc;
    printk("%s: ACK\n", link->name);
    link->rctx.tx_acked = true;
    if (link->rctx.tid_requester != RTEMS_ID_NONE) {
        sc = rtems_event_send(link->rctx.tid_requester, RTEMS_EVENT_0);
        if (sc != RTEMS_SUCCESSFUL)
            // there was a race with send timeout and clearing tid_requester
            rtems_panic("%s: ACK: failed to send ACK to listening task",
                        link->name);
    }
}