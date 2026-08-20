#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/ftrace.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <net/sock.h>
#include <net/af_vsock.h>
#include <linux/virtio_vsock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("Transparent VSOCK Redirect Proxy Kernel Module");
MODULE_VERSION("1.0");

/* Configuration parameters */
static unsigned int vmx_cid = 0;
static unsigned int port_start = 0;
static unsigned int port_end = 0;
static int port_offset = 0;

module_param(vmx_cid, uint, 0644);
MODULE_PARM_DESC(vmx_cid, "CID of the target VM-X");
module_param(port_start, uint, 0644);
MODULE_PARM_DESC(port_start, "Start of redirect port range");
module_param(port_end, uint, 0644);
MODULE_PARM_DESC(port_end, "End of redirect port range");
module_param(port_offset, int, 0644);
MODULE_PARM_DESC(port_offset, "Offset for port mapping (default: 0 for 1:1)");

/* Session tracking */
struct fwd_session {
    u64 guest_cid;
    u32 guest_port;
    u32 vmx_port;
    unsigned long last_active;
    struct list_head list;
};

static LIST_HEAD(session_list);
static DEFINE_SPINLOCK(session_lock);

static void add_session(u64 guest_cid, u32 guest_port, u32 vmx_port)
{
    struct fwd_session *sess;
    unsigned long flags;

    spin_lock_irqsave(&session_lock, flags);
    list_for_each_entry(sess, &session_list, list) {
        if (sess->guest_cid == guest_cid &&
            sess->guest_port == guest_port &&
            sess->vmx_port == vmx_port) {
            sess->last_active = jiffies;
            spin_unlock_irqrestore(&session_lock, flags);
            return;
        }
    }

    sess = kmalloc(sizeof(*sess), GFP_ATOMIC);
    if (!sess) {
        spin_unlock_irqrestore(&session_lock, flags);
        return;
    }

    sess->guest_cid = guest_cid;
    sess->guest_port = guest_port;
    sess->vmx_port = vmx_port;
    sess->last_active = jiffies;
    list_add(&sess->list, &session_list);
    spin_unlock_irqrestore(&session_lock, flags);
}

static bool session_exists(u64 guest_cid, u32 guest_port, u32 vmx_port)
{
    struct fwd_session *sess;
    unsigned long flags;
    bool found = false;

    spin_lock_irqsave(&session_lock, flags);
    list_for_each_entry(sess, &session_list, list) {
        if (sess->guest_cid == guest_cid &&
            sess->guest_port == guest_port &&
            sess->vmx_port == vmx_port) {
            sess->last_active = jiffies;
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&session_lock, flags);
    return found;
}

static void remove_session(u64 guest_cid, u32 guest_port, u32 vmx_port)
{
    struct fwd_session *sess, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&session_lock, flags);
    list_for_each_entry_safe(sess, tmp, &session_list, list) {
        if (sess->guest_cid == guest_cid &&
            sess->guest_port == guest_port &&
            sess->vmx_port == vmx_port) {
            list_del(&sess->list);
            kfree(sess);
            break;
        }
    }
    spin_unlock_irqrestore(&session_lock, flags);
}

static void clean_idle_sessions(void)
{
    struct fwd_session *sess, *tmp;
    unsigned long flags;
    unsigned long timeout = jiffies - msecs_to_jiffies(3600 * 1000); /* 1 hour */

    spin_lock_irqsave(&session_lock, flags);
    list_for_each_entry_safe(sess, tmp, &session_list, list) {
        if (time_before(sess->last_active, timeout)) {
            list_del(&sess->list);
            kfree(sess);
        }
    }
    spin_unlock_irqrestore(&session_lock, flags);
}

/* Helper functions */
static inline bool is_redirect_port(u32 port)
{
    if (port_start == 0 || port_end == 0)
        return false;
    return (port >= port_start && port <= port_end);
}

static inline u32 map_port(u32 port)
{
    int mapped = (int)port + port_offset;
    if (mapped < 0)
        return port;
    return (u32)mapped;
}

/* Dynamically resolved symbols */
static struct virtio_transport *global_vhost_transport = NULL;

static void *resolve_symbol(const char *name)
{
    struct kprobe kp = { .symbol_name = name };
    void *addr;
    int ret;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("vsock_fwd: Failed to resolve symbol %s: %d\n", name, ret);
        return NULL;
    }
    addr = kp.addr;
    unregister_kprobe(&kp);
    return addr;
}

/* Ftrace Hook for virtio_transport_recv_pkt (Forward redirection) */
static void dummy_virtio_transport_recv_pkt(struct virtio_transport *t, struct sk_buff *skb)
{
    /* Dummy target to bypass the original function */
}

static void notrace hook_virtio_transport_recv_pkt(unsigned long ip, unsigned long parent_ip,
                                                  struct ftrace_ops *op, struct ftrace_regs *fregs)
{
    struct pt_regs *regs = ftrace_get_regs(fregs);
    struct virtio_transport *t;
    struct sk_buff *skb;
    struct virtio_vsock_hdr *hdr;
    u64 dst_cid;
    u32 dst_port;
    u64 src_cid;

    if (!regs)
        return;

    t = (struct virtio_transport *)regs->di;
    skb = (struct sk_buff *)regs->si;

    if (!t || !skb)
        return;

    if (unlikely(!global_vhost_transport)) {
        global_vhost_transport = t;
        pr_info("vsock_fwd: Dynamically captured vhost_transport pointer: %p\n", global_vhost_transport);
    }

    hdr = virtio_vsock_hdr(skb);
    if (!hdr)
        return;

    dst_cid = le64_to_cpu(hdr->dst_cid);
    dst_port = le32_to_cpu(hdr->dst_port);
    src_cid = le64_to_cpu(hdr->src_cid);

    /* Intercept packets from guest to host (CID 2) on redirected ports */
    if (vmx_cid != 0 && dst_cid == 2 && src_cid != vmx_cid && is_redirect_port(dst_port)) {
        u32 new_port = map_port(dst_port);

        pr_info("vsock_fwd: Forward intercept: guest %llu:%u -> host :%u. Redirecting to VM-X %u:%u (op=%u)\n",
                src_cid, le32_to_cpu(hdr->src_port), dst_port, vmx_cid, new_port, le16_to_cpu(hdr->op));

        /* Add to active sessions table */
        add_session(src_cid, le32_to_cpu(hdr->src_port), new_port);

        /* Rewrite destination */
        hdr->dst_cid = cpu_to_le64(vmx_cid);
        hdr->dst_port = cpu_to_le32(new_port);

        /* Re-inject to VM-X's virtqueue */
        t->send_pkt(skb);

        /* Bypass local delivery */
        regs->ip = (unsigned long)dummy_virtio_transport_recv_pkt;
    }
}

/* Ftrace Hook for virtio_transport_deliver_tap_pkt (Reverse redirection) */
static void notrace hook_virtio_transport_deliver_tap_pkt(unsigned long ip, unsigned long parent_ip,
                                                          struct ftrace_ops *op, struct ftrace_regs *fregs)
{
    struct pt_regs *regs = ftrace_get_regs(fregs);
    struct sk_buff *skb;
    struct virtio_vsock_hdr *hdr;
    u64 src_cid;
    u64 dst_cid;
    u32 src_port;
    u32 dst_port;

    if (!regs)
        return;

    skb = (struct sk_buff *)regs->di;
    if (!skb)
        return;

    hdr = virtio_vsock_hdr(skb);
    if (!hdr)
        return;

    src_cid = le64_to_cpu(hdr->src_cid);
    dst_cid = le64_to_cpu(hdr->dst_cid);
    src_port = le32_to_cpu(hdr->src_port);
    dst_port = le32_to_cpu(hdr->dst_port);

    /* Intercept response packets from VM-X to the guest */
    if (vmx_cid != 0 && src_cid == vmx_cid && dst_cid != 2 && dst_cid != vmx_cid) {
        if (session_exists(dst_cid, dst_port, src_port)) {
            struct sk_buff *cloned;

            pr_info("vsock_fwd: Reverse intercept: VM-X %llu:%u -> guest %llu:%u (op=%u)\n",
                    src_cid, src_port, dst_cid, dst_port, le16_to_cpu(hdr->op));

            /* Clone the skb to forward to the guest */
            cloned = skb_clone(skb, GFP_ATOMIC);
            if (cloned) {
                struct virtio_vsock_hdr *cloned_hdr = virtio_vsock_hdr(cloned);

                /* Rewrite source back to 2 (host) */
                cloned_hdr->src_cid = cpu_to_le64(2);

                /* Send via vhost_transport to guest's RX queue */
                if (global_vhost_transport) {
                    global_vhost_transport->send_pkt(cloned);
                } else {
                    pr_warn_ratelimited("vsock_fwd: global_vhost_transport not available!\n");
                    kfree_skb(cloned);
                }
            }

            /* Clean up session on RST or SHUTDOWN */
            if (hdr->op == cpu_to_le16(VIRTIO_VSOCK_OP_RST) ||
                hdr->op == cpu_to_le16(VIRTIO_VSOCK_OP_SHUTDOWN)) {
                remove_session(dst_cid, dst_port, src_port);
            }
        }
    }
}

/* Hook definitions */
static struct ftrace_ops recv_ops = {
    .func = hook_virtio_transport_recv_pkt,
    .flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY | FTRACE_OPS_FL_DYNAMIC,
};

static struct ftrace_ops tap_ops = {
    .func = hook_virtio_transport_deliver_tap_pkt,
    .flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_DYNAMIC,
};

/* ProcFS interface */
static struct proc_dir_entry *proc_dir = NULL;

static int config_show(struct seq_file *m, void *v)
{
    seq_printf(m, "vmx_cid=%u\n", vmx_cid);
    seq_printf(m, "port_start=%u\n", port_start);
    seq_printf(m, "port_end=%u\n", port_end);
    seq_printf(m, "port_offset=%d\n", port_offset);
    return 0;
}

static int config_open(struct inode *inode, struct file *file)
{
    return single_open(file, config_show, NULL);
}

static ssize_t config_write(struct file *file, const char __user *buffer,
                            size_t count, loff_t *ppos)
{
    char kbuf[128];
    int parsed;
    unsigned int cid = 0, start = 0, end = 0;
    int offset = 0;

    if (count >= sizeof(kbuf))
        return -EINVAL;

    if (copy_from_user(kbuf, buffer, count))
        return -EFAULT;

    kbuf[count] = '\0';

    parsed = sscanf(kbuf, "cid=%u port_start=%u port_end=%u port_offset=%d",
                    &cid, &start, &end, &offset);
    if (parsed >= 3) {
        vmx_cid = cid;
        port_start = start;
        port_end = end;
        if (parsed >= 4)
            port_offset = offset;
        pr_info("vsock_fwd: Updated config: vmx_cid=%u, port_start=%u, port_end=%u, port_offset=%d\n",
                vmx_cid, port_start, port_end, port_offset);
        clean_idle_sessions();
    } else {
        pr_err("vsock_fwd: Invalid format. Expected: cid=N port_start=A port_end=B [port_offset=C]\n");
        return -EINVAL;
    }

    return count;
}

static const struct proc_ops config_proc_ops = {
    .proc_open = config_open,
    .proc_read = seq_read,
    .proc_write = config_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int sessions_show(struct seq_file *m, void *v)
{
    struct fwd_session *sess;
    unsigned long flags;

    seq_puts(m, "GUEST_CID  GUEST_PORT  VMX_PORT  AGE_SEC\n");
    spin_lock_irqsave(&session_lock, flags);
    list_for_each_entry(sess, &session_list, list) {
        seq_printf(m, "%-10llu %-11u %-9u %lu\n",
                   sess->guest_cid, sess->guest_port, sess->vmx_port,
                   (jiffies - sess->last_active) / HZ);
    }
    spin_unlock_irqrestore(&session_lock, flags);
    return 0;
}

static int sessions_open(struct inode *inode, struct file *file)
{
    return single_open(file, sessions_show, NULL);
}

static const struct proc_ops sessions_proc_ops = {
    .proc_open = sessions_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

/* Module init and exit */
static int __init vsock_fwd_init(void)
{
    int ret;
    void *recv_addr;
    void *tap_addr;

    pr_info("vsock_fwd: Initializing transparent VSOCK redirect proxy...\n");



    recv_addr = resolve_symbol("virtio_transport_recv_pkt");
    tap_addr = resolve_symbol("virtio_transport_deliver_tap_pkt");
    if (!recv_addr || !tap_addr) {
        pr_err("vsock_fwd: Required symbols not resolved.\n");
        return -ENODEV;
    }

    /* Configure recv_ops ftrace hook */
    ret = ftrace_set_filter_ip(&recv_ops, (unsigned long)recv_addr, 0, 0);
    if (ret) {
        pr_err("vsock_fwd: ftrace_set_filter_ip for recv failed: %d\n", ret);
        return ret;
    }

    ret = register_ftrace_function(&recv_ops);
    if (ret) {
        pr_err("vsock_fwd: register_ftrace_function for recv failed: %d\n", ret);
        return ret;
    }

    /* Configure tap_ops ftrace hook */
    ret = ftrace_set_filter_ip(&tap_ops, (unsigned long)tap_addr, 0, 0);
    if (ret) {
        pr_err("vsock_fwd: ftrace_set_filter_ip for tap failed: %d\n", ret);
        goto err_unregister_recv;
    }

    ret = register_ftrace_function(&tap_ops);
    if (ret) {
        pr_err("vsock_fwd: register_ftrace_function for tap failed: %d\n", ret);
        goto err_unregister_recv;
    }

    /* Setup procFS directory and files */
    proc_dir = proc_mkdir("vsock_fwd", NULL);
    if (!proc_dir) {
        pr_err("vsock_fwd: Failed to create proc directory\n");
        ret = -ENOMEM;
        goto err_unregister_all;
    }

    proc_create("config", 0644, proc_dir, &config_proc_ops);
    proc_create("sessions", 0444, proc_dir, &sessions_proc_ops);

    pr_info("vsock_fwd: Module initialized successfully.\n");
    return 0;

err_unregister_all:
    unregister_ftrace_function(&tap_ops);
err_unregister_recv:
    unregister_ftrace_function(&recv_ops);
    return ret;
}

static void __exit vsock_fwd_exit(void)
{
    struct fwd_session *sess, *tmp;
    unsigned long flags;

    pr_info("vsock_fwd: Exiting transparent VSOCK redirect proxy...\n");

    /* Remove ProcFS entries */
    if (proc_dir) {
        remove_proc_entry("config", proc_dir);
        remove_proc_entry("sessions", proc_dir);
        remove_proc_entry("vsock_fwd", NULL);
    }

    /* Unregister ftrace hooks */
    unregister_ftrace_function(&recv_ops);
    unregister_ftrace_function(&tap_ops);

    /* Free sessions list */
    spin_lock_irqsave(&session_lock, flags);
    list_for_each_entry_safe(sess, tmp, &session_list, list) {
        list_del(&sess->list);
        kfree(sess);
    }
    spin_unlock_irqrestore(&session_lock, flags);

    pr_info("vsock_fwd: Module exited successfully.\n");
}

module_init(vsock_fwd_init);
module_exit(vsock_fwd_exit);
