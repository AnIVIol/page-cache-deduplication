#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/compiler.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CS614");
MODULE_DESCRIPTION("Buddy Allocator PFN Tracker");

#define MAX_TRACK_PFNS 32

static unsigned long track_pfns[MAX_TRACK_PFNS];
static int num_track_pfns = 0;
static DEFINE_SPINLOCK(track_pfns_lock);

// Expose array to sysfs for manual injection if needed
module_param_array(track_pfns, ulong, &num_track_pfns, 0644);
MODULE_PARM_DESC(track_pfns, "Array of PFNs to watch");

/* * API for your deduplication module to call.
 * This safely inserts a newly freed PFN into the watch list.
 */
void auto_track_freed_pfn(unsigned long pfn)
{
    int i;
    unsigned long flags;

    spin_lock_irqsave(&track_pfns_lock, flags);

    for (i = 0; i < MAX_TRACK_PFNS; i++) {
        if (track_pfns[i] == 0) {
            WRITE_ONCE(track_pfns[i], pfn);
            pr_info("PFN_TRACKER: Armed trap for PFN %lu at slot %d\n", pfn, i);
            break;
        }
    }

    if (i == MAX_TRACK_PFNS) {
//        pr_warn("PFN_TRACKER: Array full! Missed PFN %lu\n", pfn);
    }

    spin_unlock_irqrestore(&track_pfns_lock, flags);
}
EXPORT_SYMBOL(auto_track_freed_pfn); // Make visible to your other modules

/*
 * The Kretprobe Handler: Runs when __alloc_pages finishes.
 */
static int alloc_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct page *page;
    unsigned long pfn;
    int i;

    // Extract the returned struct page *
    page = (struct page *)regs_return_value(regs);
    if (!page) 
        return 0;

    pfn = page_to_pfn(page);

    // Lockless scan for maximum performance on the buddy allocator hot path
    for (i = 0; i < MAX_TRACK_PFNS; i++) {
        unsigned long target = READ_ONCE(track_pfns[i]);
        
        if (unlikely(target != 0 && pfn == target)) {
    //        pr_info("PFN_TRACKER: [!] VICTORY! PFN %lu resurrected!\n", pfn);
            pr_info("PFN_TRACKER: Claimed by PID %d (%s)\n", current->pid, current->comm);
  //          dump_stack();
            
            // Disarm this slot
            WRITE_ONCE(track_pfns[i], 0);
            break; 
        }
    }

    return 0;
}

static struct kretprobe krp_alloc = {
    .handler = alloc_ret_handler,
    .kp.symbol_name = "__alloc_pages", 
};

static int __init pfn_tracker_init(void)
{
    int ret;
    
    // Initialize the array to 0
    memset(track_pfns, 0, sizeof(track_pfns));

    ret = register_kretprobe(&krp_alloc);
    if (ret < 0) {
        pr_err("PFN_TRACKER: Failed to register kretprobe: %d\n", ret);
        return ret;
    }

    pr_info("PFN_TRACKER: Module loaded and hooked into __alloc_pages.\n");
    return 0;
}

static void __exit pfn_tracker_exit(void)
{
    unregister_kretprobe(&krp_alloc);
    pr_info("PFN_TRACKER: Module unloaded.\n");
}

module_init(pfn_tracker_init);
module_exit(pfn_tracker_exit);
