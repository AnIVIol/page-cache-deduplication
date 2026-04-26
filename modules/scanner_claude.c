#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/jhash.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/dedup_shared.h>

struct registered_file {
    struct list_head list;
    struct address_space *mapping;
    struct file *filp;
};
static LIST_HEAD(registered_files);
static DEFINE_MUTEX(registered_files_lock);

static DECLARE_WAIT_QUEUE_HEAD(scanner_wait);
static struct task_struct *scanner_thread;
static DEFINE_MUTEX(scanner_control_lock);
static unsigned int scan_interval_ms = 500;     /* default sweep interval */

struct unstable_record {
    struct list_head list;
    u32 hash;
    struct page *page;          /* holds +1 ref while on the list */
};
static LIST_HEAD(unstable_list);
static DEFINE_MUTEX(unstable_lock);
static unsigned int unstable_count;

#define DEDUP_UNSTABLE_MAX 65536

static struct kobject *scanner_kobj;

static u32 hash_page_data(struct page *page)
{
    void *vaddr = kmap_local_page(page);
    u32 hash = jhash2((u32 *)vaddr, PAGE_SIZE / sizeof(u32), 0x614);
    kunmap_local(vaddr);
    return hash;
}

static void flush_unstable_list(void)
{
    struct unstable_record *rec, *tmp;

    mutex_lock(&unstable_lock);
    list_for_each_entry_safe(rec, tmp, &unstable_list, list) {
        list_del(&rec->list);
        put_page(rec->page);
        kfree(rec);
    }
    unstable_count = 0;
    mutex_unlock(&unstable_lock);
}

static void cleanup_scanner_state(void)
{
    struct registered_file *rfile, *tmp;

    unmerge_all_dedup_pages();

    mutex_lock(&registered_files_lock);
    list_for_each_entry_safe(rfile, tmp, &registered_files, list) {
        list_del(&rfile->list);
        fput(rfile->filp);
        kfree(rfile);
    }
    mutex_unlock(&registered_files_lock);

    flush_unstable_list();
}

static bool page_is_eligible(struct page *page)
{
    if (PageDirty(page) || PageWriteback(page))
        return false;
    if (PageSharedCache(page))
        return false;
    if (PageAnon(page) || PageSwapCache(page))
        return false;
    if (page_has_private(page))
        return false;            /* fs-private (buffer heads etc.) */
    if (page_mapcount(page))
        return false;            /* still mmap'd by some process */
    return true;
}

static struct page *find_stable_match(struct page *page)
{
    struct shared_cache_node *node;
    struct page *candidate;
    unsigned long flags;
    struct shared_cache_node *skip = NULL;

retry:
    candidate = NULL;
    spin_lock_irqsave(&global_dedup_lock, flags);
    list_for_each_entry(node, &global_dedup_list, global_list) {
        if (node == skip)
            continue;
        if (!node->survivor_page)
            continue;
        if (!get_page_unless_zero(node->survivor_page))
            continue;
        candidate = node->survivor_page;
        skip = node;          /* if candidate fails, don't retry it */
        break;
    }
    spin_unlock_irqrestore(&global_dedup_lock, flags);

    if (!candidate)
        return NULL;

    if (pages_are_identical(candidate, page))
        return candidate;

    put_page(candidate);
    goto retry;
}

/*
 * Unstable search: returns the matched page (+1 ref) and detaches the
 * record from the list. Caller becomes responsible for both refs and
 * for freeing the record (or re-adding it on merge failure).
 */
static struct page *find_unstable_match(struct page *page, u32 hash,
                                        struct unstable_record **rec_out)
{
    struct unstable_record *rec, *tmp;
    struct page *target = NULL;

    *rec_out = NULL;

    mutex_lock(&unstable_lock);
    list_for_each_entry_safe(rec, tmp, &unstable_list, list) {
        if (rec->hash != hash)
            continue;
        if (!pages_are_identical(rec->page, page))
            continue;
        target = rec->page;
        get_page(target);                /* temporary pin for caller */
        list_del(&rec->list);
        unstable_count--;
        *rec_out = rec;
        break;
    }
    mutex_unlock(&unstable_lock);

    return target;
}

static void unstable_insert(struct page *page, u32 hash)
{
    struct unstable_record *new_rec, *old;

    new_rec = kmalloc(sizeof(*new_rec), GFP_KERNEL);
    if (!new_rec)
        return;

    new_rec->hash = hash;
    new_rec->page = page;
    get_page(page);                       /* unstable list owns +1 ref */

    mutex_lock(&unstable_lock);
    if (unstable_count >= DEDUP_UNSTABLE_MAX) {
        old = list_first_entry(&unstable_list,
                               struct unstable_record, list);
        list_del(&old->list);
        unstable_count--;
        put_page(old->page);
        kfree(old);
    }
    list_add_tail(&new_rec->list, &unstable_list);
    unstable_count++;
    mutex_unlock(&unstable_lock);
}

static void scan_single_mapping(struct address_space *mapping)
{
    struct page *page;
    pgoff_t index;
    pgoff_t max_index;

    if (!mapping || !mapping->host)
        return;
    max_index = (i_size_read(mapping->host) + PAGE_SIZE - 1) >> PAGE_SHIFT;

    for (index = 0; index < max_index; index++) {
        u32 page_hash;
        struct page *target_page;
        struct unstable_record *matched_rec;
        int ret;

        if (kthread_should_stop())
            return;
        cond_resched();

        page = find_get_page(mapping, index);
        if (!page)
            continue;

        if (!page_is_eligible(page)) {
            put_page(page);
            continue;
        }

        page_hash = hash_page_data(page);

        /* ---- 1) Stable search ---- */
        target_page = find_stable_match(page);
        if (target_page) {
            ret = merge_page_cache_pages(target_page, page);
            put_page(target_page);              /* drop our temp pin */
            put_page(page);                     /* drop find_get_page pin */
            if (ret && ret != -EAGAIN && ret != -EBUSY)
                pr_debug("dedup: stable merge failed: %d\n", ret);
            continue;
        }

        /* ---- 2) Unstable search ---- */
        target_page = find_unstable_match(page, page_hash, &matched_rec);
        if (target_page) {
            ret = merge_page_cache_pages(target_page, page);
            if (ret == 0) {
                /* Promotion: target is now a stable survivor.
                 * Two refs to drop: temp pin + unstable's pin. */
                put_page(target_page);
                put_page(target_page);
                kfree(matched_rec);
            } else {
                /* Merge failed — restore the unstable record so the
                 * page isn't leaked and may match again later. */
                mutex_lock(&unstable_lock);
                list_add_tail(&matched_rec->list, &unstable_list);
                unstable_count++;
                mutex_unlock(&unstable_lock);
                put_page(target_page);          /* drop only temp pin */
            }
            put_page(page);
            continue;
        }

        /* ---- 3) Add to unstable for later pages this cycle ---- */
        unstable_insert(page, page_hash);
        put_page(page);
    }
}

static int dedup_scanner_thread(void *data)
{
    struct registered_file *rfile;
    struct file *filp;

    while (!kthread_should_stop()) {
        wait_event_interruptible_timeout(scanner_wait,
            kthread_should_stop(),
            msecs_to_jiffies(scan_interval_ms));

        if (kthread_should_stop())
            break;

        flush_unstable_list();

        mutex_lock(&registered_files_lock);
        if (list_empty(&registered_files)) {
            mutex_unlock(&registered_files_lock);
            continue;
        }

        list_for_each_entry(rfile, &registered_files, list) {
            filp = rfile->filp;
            get_file(filp);
            mutex_unlock(&registered_files_lock);

            scan_single_mapping(filp->f_mapping);

            fput(filp);

            if (kthread_should_stop())
                return 0;

            mutex_lock(&registered_files_lock);
        }
        mutex_unlock(&registered_files_lock);
    }

    return 0;
}

static ssize_t scan_file_store(struct kobject *kobj, struct kobj_attribute *attr,
                               const char *buf, size_t count)
{
    char path[PATH_MAX];
    struct file *filp;
    struct registered_file *rfile;
    size_t len;

    if (count == 0 || count >= sizeof(path))
        return -EINVAL;

    memcpy(path, buf, count);
    path[count] = '\0';
    len = strlen(path);
    if (len && path[len - 1] == '\n')
        path[len - 1] = '\0';
    if (path[0] == '\0')
        return -EINVAL;

    filp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(filp))
        return PTR_ERR(filp);

    if (!filp->f_mapping || !mapping_can_dedup(filp->f_mapping)) {
        fput(filp);
        return -EINVAL;
    }

    rfile = kmalloc(sizeof(*rfile), GFP_KERNEL);
    if (!rfile) {
        fput(filp);
        return -ENOMEM;
    }
    rfile->filp = filp;
    rfile->mapping = filp->f_mapping;

    mutex_lock(&registered_files_lock);
    list_add_tail(&rfile->list, &registered_files);
    mutex_unlock(&registered_files_lock);

    pr_info("dedup_scanner: registered %s\n", path);
    return count;
}

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr,
                        char *buf)
{
    int running;

    mutex_lock(&scanner_control_lock);
    running = (scanner_thread != NULL);
    mutex_unlock(&scanner_control_lock);

    return sprintf(buf, "%d\n", running);
}

static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
                         const char *buf, size_t count)
{
    int run;

    if (kstrtoint(buf, 10, &run))
        return -EINVAL;

    mutex_lock(&scanner_control_lock);
    if (run == 1 && !scanner_thread) {
        scanner_thread = kthread_run(dedup_scanner_thread, NULL,
                                     "dedup_scanner");
        if (IS_ERR(scanner_thread)) {
            int err = PTR_ERR(scanner_thread);
            scanner_thread = NULL;
            mutex_unlock(&scanner_control_lock);
            return err;
        }
        pr_info("dedup_scanner: daemon started\n");
    } else if (run == 0 && scanner_thread) {
        kthread_stop(scanner_thread);
        scanner_thread = NULL;
        cleanup_scanner_state();
        pr_info("dedup_scanner: daemon stopped, state purged\n");
    }
    mutex_unlock(&scanner_control_lock);

    return count;
}

static ssize_t interval_show(struct kobject *kobj, struct kobj_attribute *attr,
                             char *buf)
{
    return sprintf(buf, "%u\n", scan_interval_ms);
}

static ssize_t interval_store(struct kobject *kobj, struct kobj_attribute *attr,
                              const char *buf, size_t count)
{
    unsigned int ms;

    if (kstrtouint(buf, 10, &ms))
        return -EINVAL;
    if (ms < 50 || ms > 600000)        /* 50ms .. 10min */
        return -EINVAL;

    scan_interval_ms = ms;
    return count;
}

static struct kobj_attribute scan_file_attr =
    __ATTR(scan_file, 0200, NULL, scan_file_store);
static struct kobj_attribute run_attr =
    __ATTR(run, 0600, run_show, run_store);
static struct kobj_attribute interval_attr =
    __ATTR(interval, 0600, interval_show, interval_store);

static int __init scanner_init(void)
{
    int err;

    scanner_kobj = kobject_create_and_add("dedup_scanner", kernel_kobj);
    if (!scanner_kobj)
        return -ENOMEM;

    err = sysfs_create_file(scanner_kobj, &scan_file_attr.attr) ?:
          sysfs_create_file(scanner_kobj, &run_attr.attr)       ?:
          sysfs_create_file(scanner_kobj, &interval_attr.attr);
    if (err) {
        kobject_put(scanner_kobj);
        return err;
    }

    pr_info("dedup_scanner: loaded; awaiting activation\n");
    return 0;
}

static void __exit scanner_exit(void)
{
    mutex_lock(&scanner_control_lock);
    if (scanner_thread) {
        kthread_stop(scanner_thread);
        scanner_thread = NULL;
        cleanup_scanner_state();
    }
    mutex_unlock(&scanner_control_lock);

    kobject_put(scanner_kobj);
    pr_info("dedup_scanner: unloaded\n");
}

module_init(scanner_init);
module_exit(scanner_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Page cache deduplication scanner");
