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
#include <linux/dedup_shared.h>

struct registered_file {
    struct list_head list;
    struct address_space *mapping;
    struct file *filp; 
};
static LIST_HEAD(registered_files);
static DEFINE_MUTEX(registered_files_lock);
static DECLARE_WAIT_QUEUE_HEAD(scanner_wait);

static struct task_struct *scanner_thread = NULL;
static DEFINE_MUTEX(scanner_control_lock);
static unsigned int scan_interval_ms = 500; // Default: sweep every 5 seconds

struct unstable_record {
    struct list_head list;
    u32 hash;
    struct page *page;
};
static LIST_HEAD(unstable_list);
static DEFINE_MUTEX(unstable_lock);

static struct kobject *scanner_kobj;

static u32 hash_page_data(struct page *page) {
    void *vaddr = kmap_local_page(page);
    u32 hash = jhash2((u32 *)vaddr, PAGE_SIZE / sizeof(u32), 0x614); 
    kunmap_local(vaddr);
    return hash;
}

static void flush_unstable_list(void) {
    struct unstable_record *rec, *tmp;
    
    mutex_lock(&unstable_lock);
    list_for_each_entry_safe(rec, tmp, &unstable_list, list) {
        list_del(&rec->list);
        put_page(rec->page);
        kfree(rec);
    }
    mutex_unlock(&unstable_lock);
}

static void cleanup_scanner_state(void) {
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
//TODO: removing from scanner shifted at /sys/kernel/page_dedup/flush
}

static void scan_single_mapping(struct address_space *mapping) {
    struct page *page;
    pgoff_t index;
    pgoff_t max_index = (mapping->host->i_size + PAGE_SIZE - 1) >> PAGE_SHIFT;

    unsigned long flags;
    for (index = 0; index < max_index; index++) {
        page = find_get_page(mapping, index);
        if (!page) continue; 
  	
	if (PageDirty(page) || PageWriteback(page)) {
            put_page(page);
            continue;
        }      
        if (PageSharedCache(page)) {
            put_page(page);
            continue;
        }

        u32 page_hash = hash_page_data(page);
        struct shared_cache_node *stable_node;
        struct unstable_record *unstable, *tmp_unstable;
        struct page *target_page = NULL;

        spin_lock_irqsave(&global_dedup_lock, flags);
        list_for_each_entry(stable_node, &global_dedup_list, global_list) {
    //TODO: add a hash field in the shared node
            if (pages_are_identical(stable_node->survivor_page, page)) {
                target_page = stable_node->survivor_page;
                get_page(target_page);
                break;
            }
        }
        spin_unlock_irqrestore(&global_dedup_lock, flags);

        // If we found a match in the Stable tree, merge and move to next index
        if (target_page) {
            //pr_info("dedup_scanner: Match found in STABLE list! Merging Index %lu...\n", index);
            merge_page_cache_pages(target_page, page);
            put_page(target_page);
            put_page(page);
            continue;
        }

        struct unstable_record *matched_unstable = NULL;
        
        mutex_lock(&unstable_lock);
        list_for_each_entry_safe(unstable, tmp_unstable, &unstable_list, list) {
            if (unstable->hash == page_hash && pages_are_identical(unstable->page, page)) {
                target_page = unstable->page;
                get_page(target_page);
                
                // Remove it from the Unstable list, it's about to be promoted!
                list_del(&unstable->list); 
                matched_unstable = unstable;
                break;
            }
        }

        // If we found a match in the Unstable tree, promote it
        if (target_page) {
            mutex_unlock(&unstable_lock);
            
            //pr_info("dedup_scanner: Match found in UNSTABLE list! Promoting Index %lu to Stable...\n", index);
            merge_page_cache_pages(target_page, page);
            
            put_page(target_page); // Drop our temporary pin
            put_page(target_page); // Drop the pin the unstable list originally held
            kfree(matched_unstable); // Free the unstable record
            
            put_page(page); // Drop the find_get_page pin
            continue;
        }

        struct unstable_record *new_unstable = kmalloc(sizeof(*new_unstable), GFP_KERNEL);
        if (new_unstable) {
            new_unstable->hash = page_hash;
            new_unstable->page = page;
            get_page(page);
            list_add_tail(&new_unstable->list, &unstable_list);
        }
        mutex_unlock(&unstable_lock);

        put_page(page);
    }
}

static int dedup_scanner_thread(void *data) {
    struct registered_file *rfile;

    while (!kthread_should_stop()) {
        wait_event_interruptible_timeout(scanner_wait, 
            kthread_should_stop(), 
            msecs_to_jiffies(scan_interval_ms));

        if (kthread_should_stop()) break;

        flush_unstable_list();

        mutex_lock(&registered_files_lock);
        if (list_empty(&registered_files)) {
            mutex_unlock(&registered_files_lock);
            continue; 
        }

//        pr_info("dedup_scanner: Starting periodic sweep of registered files...\n");

        list_for_each_entry(rfile, &registered_files, list) {
            struct file *filp = rfile->filp;
            get_file(filp); 
            mutex_unlock(&registered_files_lock);
            
            scan_single_mapping(filp->f_mapping);
            
            fput(filp); 
            mutex_lock(&registered_files_lock);
        }
        mutex_unlock(&registered_files_lock);
    }
    return 0;
}

static ssize_t scan_file_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    char path[256];
    struct file *filp;
    struct registered_file *rfile;

    snprintf(path, sizeof(path), "%s", buf);
    if (path[strlen(path)-1] == '\n') path[strlen(path)-1] = '\0';

    filp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(filp)) return PTR_ERR(filp);

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

    pr_info("dedup_scanner: Registered %s for continuous scanning\n", path);
    wake_up_interruptible(&scanner_wait); 
    return count;
}

static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    int run;
    if (kstrtoint(buf, 10, &run)) return -EINVAL;

    mutex_lock(&scanner_control_lock);
    if (run == 1 && !scanner_thread) {
        scanner_thread = kthread_run(dedup_scanner_thread, NULL, "dedup_scanner");
        pr_info("dedup_scanner: Daemon started.\n");
    } else if (run == 0 && scanner_thread) {
        pr_info("dedup_scanner: Stopping daemon and purging state...\n");
        kthread_stop(scanner_thread);
        scanner_thread = NULL;
        cleanup_scanner_state();
    }
    mutex_unlock(&scanner_control_lock);
    return count;
}

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    int is_running;
    mutex_lock(&scanner_control_lock);
    is_running = (scanner_thread != NULL);
    mutex_unlock(&scanner_control_lock);
    return sprintf(buf, "%d\n", is_running);
}

static ssize_t interval_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    unsigned int new_interval;
    if (kstrtouint(buf, 10, &new_interval)) return -EINVAL;
    scan_interval_ms = new_interval;
    pr_info("dedup_scanner: Scan interval updated to %u ms\n", scan_interval_ms);
    return count;
}

static ssize_t interval_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%u\n", scan_interval_ms);
}

static struct kobj_attribute scan_file_attr = __ATTR(scan_file, 0200, NULL, scan_file_store);
static struct kobj_attribute run_attr = __ATTR(run, 0600, run_show, run_store);
static struct kobj_attribute interval_attr = __ATTR(interval, 0600, interval_show, interval_store);

static int __init scanner_init(void)
{
    scanner_kobj = kobject_create_and_add("dedup_scanner", kernel_kobj);
    if (!scanner_kobj) return -ENOMEM;

    if (sysfs_create_file(scanner_kobj, &scan_file_attr.attr) ||
        sysfs_create_file(scanner_kobj, &run_attr.attr) ||
        sysfs_create_file(scanner_kobj, &interval_attr.attr)) {
        kobject_put(scanner_kobj);
        return -ENOMEM;
    }

    pr_info("dedup_scanner: Loaded. Waiting for activation.\n");
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
    pr_info("dedup_scanner: Module unloaded safely.\n");
}

module_init(scanner_init);
module_exit(scanner_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("prakhar22@iitk.ac.in");
