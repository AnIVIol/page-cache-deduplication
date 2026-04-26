#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <linux/file.h>

/* The structure shared between user-space and kernel-space */
struct pfn_request {
    int fd;
    unsigned long *user_pfns; /* Pointer to user-space array */
    size_t max_pfns;          /* Size of the array */
    size_t count;             /* Number of PFNs returned by the kernel */
};

#define GET_FILE_PFNS _IOWR('p', 1, struct pfn_request)

static long pfn_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct pfn_request req;
    struct fd f;
    struct address_space *mapping;
    struct folio *folio;
    unsigned long *k_pfns;
    size_t count = 0;
    XA_STATE(xas, NULL, 0);

    if (cmd != GET_FILE_PFNS)
        return -ENOTTY;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    /* Safely resolve the file descriptor */
    f = fdget(req.fd);
    if (!f.file)
        return -EBADF;

    mapping = f.file->f_mapping;
    xas.xa = &mapping->i_pages;

    /* Allocate a temporary kernel buffer to hold the PFNs */
    k_pfns = kvmalloc_array(req.max_pfns, sizeof(unsigned long), GFP_KERNEL);
    if (!k_pfns) {
        fdput(f);
        return -ENOMEM;
    }

    /* Walk the file's page cache (XArray) */
    rcu_read_lock();
    xas_for_each(&xas, folio, ULONG_MAX) {
        if (xas_retry(&xas, folio)) continue;
        if (xa_is_value(folio)) continue; /* Skip shadow/swap entries */
        
        if (count < req.max_pfns) {
            k_pfns[count] = folio_pfn(folio);
            count++;
        }
    }
    rcu_read_unlock();
    fdput(f);

    /* Copy the collected PFNs back to user-space */
    if (copy_to_user(req.user_pfns, k_pfns, count * sizeof(unsigned long))) {
        kvfree(k_pfns);
        return -EFAULT;
    }

    req.count = count;
    if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
        kvfree(k_pfns);
        return -EFAULT;
    }

    kvfree(k_pfns);
    return 0;
}

static const struct file_operations pfn_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = pfn_ioctl,
};

static struct miscdevice pfn_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "pfn_getter",
    .fops = &pfn_fops,
};

static int __init pfn_getter_init(void) { return misc_register(&pfn_dev); }
static void __exit pfn_getter_exit(void) { misc_deregister(&pfn_dev); }

module_init(pfn_getter_init);
module_exit(pfn_getter_exit);
MODULE_LICENSE("GPL");
