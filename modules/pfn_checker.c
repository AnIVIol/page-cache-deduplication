#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mm.h>

/* The structure to pass arrays back and forth */
struct pfn_check_request {
    unsigned long *pfns;      /* Input: Array of PFNs to check */
    int *refcounts;           /* Output: Array to hold the resulting counts */
    size_t count;             /* Number of PFNs in the arrays */
};

#define CHECK_PFN_REFS _IOWR('c', 1, struct pfn_check_request)

static long checker_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct pfn_check_request req;
    unsigned long *k_pfns;
    int *k_refcounts;
    size_t i;

    if (cmd != CHECK_PFN_REFS)
        return -ENOTTY;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    /* Allocate kernel buffers for the arrays */
    k_pfns = kvmalloc_array(req.count, sizeof(unsigned long), GFP_KERNEL);
    k_refcounts = kvmalloc_array(req.count, sizeof(int), GFP_KERNEL);
    
    if (!k_pfns || !k_refcounts) {
        kvfree(k_pfns); kvfree(k_refcounts);
        return -ENOMEM;
    }

    if (copy_from_user(k_pfns, req.pfns, req.count * sizeof(unsigned long))) {
        kvfree(k_pfns); kvfree(k_refcounts);
        return -EFAULT;
    }

    /* Iterate through the PFNs and read their core refcounts */
    for (i = 0; i < req.count; i++) {
        unsigned long pfn = k_pfns[i];
        if (pfn_valid(pfn)) {
            struct page *page = pfn_to_page(pfn);
            k_refcounts[i] = page_count(page);
        } else {
            k_refcounts[i] = -1; /* -1 indicates an invalid physical address */
        }
    }

    /* Copy the refcounts back to the user-space array */
    if (copy_to_user(req.refcounts, k_refcounts, req.count * sizeof(int))) {
        kvfree(k_pfns); kvfree(k_refcounts);
        return -EFAULT;
    }

    kvfree(k_pfns);
    kvfree(k_refcounts);
    return 0;
}

static const struct file_operations checker_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = checker_ioctl,
};

static struct miscdevice checker_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "pfn_checker",
    .fops = &checker_fops,
};

static int __init pfn_checker_init(void) { return misc_register(&checker_dev); }
static void __exit pfn_checker_exit(void) { misc_deregister(&checker_dev); }

module_init(pfn_checker_init);
module_exit(pfn_checker_exit);
MODULE_LICENSE("GPL");
