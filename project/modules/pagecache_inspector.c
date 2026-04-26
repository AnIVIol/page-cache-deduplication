#include <linux/kern_levels.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/dcache.h>
#include <linux/mm_types.h>
#include <linux/xarray.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/kobject.h>
#include <linux/string.h>

#define MAX_PATH_LEN 256
#define MAX_PAGES 100 // userspace defined, number of files * pages per file

unsigned long all_pfn_list[MAX_PAGES];
int total_page_count; // total number of pages corresponding to paths ever sent to sysfs interface

/* --- UTILITY FUNCTIONS --- */

// Utility to print details about an entire file
void print_pages(struct xarray *i_pages) {
    struct page *page;
    unsigned long index;
    int local_page_count = 0;

    rcu_read_lock();
    xa_for_each(i_pages, index, page) {
        if (!page) continue;

        local_page_count++;
        printk(KERN_INFO "pagecache_inspector: page index : %lu\n", index);
        printk(KERN_INFO "pagecache_inspector: ---> PFN: %lu\t Refcount: %d\n",
               page_to_pfn(page), page_ref_count(page));
    }
    rcu_read_unlock();

    pr_info("pagecache_inspector: total pages of this file are: %d\n", local_page_count);
}

// Safely resolve the filepath to the kernel's XArray
struct xarray* get_ipages_from_filepath(const char *filepath) {
    struct path path;
    struct xarray *i_pages;
    struct inode *inode0;
    int err;

    printk(KERN_INFO "pagecache_inspector: Inspecting file: %s\n", filepath);

    err = kern_path(filepath, LOOKUP_FOLLOW, &path);
    if (err) {
        pr_err("pagecache_inspector: Failed to resolve path: %s\n", filepath);
        return NULL; // CRITICAL: Return NULL on failure
    }

    inode0 = d_inode(path.dentry);

    printk(KERN_INFO "pagecache_inspector: Inode number: %lu\n", inode0->i_ino);
    printk(KERN_INFO "pagecache_inspector: File size: %lld bytes\n", (long long)inode0->i_size);

    i_pages = &(inode0->i_mapping->i_pages);

    path_put(&path);
    return i_pages;
}

/* --- THE COMBINED SYSFS INTERFACE --- */

static ssize_t print_file_pages(struct kobject *kobj,
                                struct kobj_attribute *attr,
                                const char *buf,
                                size_t count)
{
    char filepath[MAX_PATH_LEN];
    long long offset = -1; // Default to -1 (print all) if no offset is provided
    struct xarray* ipages;

    // Parse the input: Extract the path, and OPTIONALLY try to extract an offset.
    if (sscanf(buf, "%255s %lld", filepath, &offset) < 1) {
        pr_err("pagecache_inspector: Invalid input format.\n");
        return -EINVAL;
    }

    ipages = get_ipages_from_filepath(filepath);
    
    // THE CRITICAL FIX: Abort if the file doesn't exist
    if (!ipages) {
        pr_err("pagecache_inspector: Aborting, ipages is NULL.\n");
        return count; 
    }

    if (offset == -1) {
        // Mode 1: Print the entire file
        printk(KERN_INFO "pagecache_inspector: Dumping all pages for %s\n", filepath);
        print_pages(ipages);
    } else {
        // Mode 2: Print a specific offset
        pgoff_t index = offset >> PAGE_SHIFT;
        struct page *page;

        rcu_read_lock();
        page = xa_load(ipages, index); 
        
        if (page) {
            printk(KERN_INFO "pagecache_inspector: File: %s | Offset: %lld -> Index: %lu\n", 
                   filepath, offset, index);
            printk(KERN_INFO "pagecache_inspector: ---> PFN: %lu\t Refcount: %d\n", 
                   page_to_pfn(page), page_ref_count(page));
        } else {
            printk(KERN_INFO "pagecache_inspector: File: %s | Offset: %lld -> Index: %lu\n", 
                   filepath, offset, index);
            printk(KERN_INFO "pagecache_inspector: ---> STATUS: Page not present in RAM.\n");
        }
        rcu_read_unlock();
    }

    return count;
}

static ssize_t print_file_pages(struct kobject *kobj,
                                struct kobj_attribute *attr,
                                const char *buf,
                                size_t count);

static ssize_t verify_files_shared(struct kobject *kobj,
                                  struct kobj_attribute *attr,
                                  const char *buf,
                                  size_t count)
{
    char path1[MAX_PATH_LEN], path2[MAX_PATH_LEN];
    struct xarray *ipages1, *ipages2;
    struct page *p1, *p2;
    unsigned long index;
    int shared_count = 0, total_pages1 = 0, total_pages2 = 0;
    bool mismatch = false;

    if (sscanf(buf, "%255s %255s", path1, path2) < 2) {
        pr_err("pagecache_inspector: verify_shared requires two paths.\n");
        return -EINVAL;
    }

    ipages1 = get_ipages_from_filepath(path1);
    ipages2 = get_ipages_from_filepath(path2);

    if (!ipages1 || !ipages2) {
        pr_err("pagecache_inspector: Failed to resolve one or both paths.\n");
        return count;
    }

    rcu_read_lock();
    // Count and verify pages in file 1 against file 2
    xa_for_each(ipages1, index, p1) {
        if (!p1) continue;
        total_pages1++;
        p2 = xa_load(ipages2, index);
        if (p2 && page_to_pfn(p1) == page_to_pfn(p2)) {
            shared_count++;
        } else {
            mismatch = true;
        }
    }
    // Just count pages in file 2 to see if there are extras
    xa_for_each(ipages2, index, p2) {
        if (p2) total_pages2++;
    }
    rcu_read_unlock();

    if (!mismatch && total_pages1 == total_pages2 && total_pages1 > 0) {
        pr_info("pagecache_inspector: VERIFY_SUCCESS: %s and %s share ALL %d pages.\n", path1, path2, total_pages1);
    } else {
        pr_info("pagecache_inspector: VERIFY_FAILURE: %s (%d pages) and %s (%d pages) share %d pages.\n", 
                path1, total_pages1, path2, total_pages2, shared_count);
    }

    return count;
}

/* --- SYSFS SETUP & TEARDOWN --- */

// We now only need the single, unified attribute
static struct kobj_attribute pfn_list_attribute =
        __ATTR(filename_print, 0644, NULL, print_file_pages);

static struct kobj_attribute verify_shared_attribute =
        __ATTR(verify_shared, 0644, NULL, verify_files_shared);

static struct attribute *module_attr[] = {
        &pfn_list_attribute.attr,
        &verify_shared_attribute.attr,
        NULL,
};

static struct attribute_group attr_group = {
        .attrs = module_attr,
        .name = "pagecache_inspector",
};

int init_module(void)
{
    int retval;
    total_page_count = 0;

    retval = sysfs_create_group(kernel_kobj, &attr_group);
    if(unlikely(retval)){
        printk(KERN_ERR "pagecache_inspector: can't create sysfs\n");
        return retval;
    }

    printk(KERN_INFO "pagecache_inspector: Module loaded successfully.\n");
    return 0;
}

void cleanup_module(void)
{
    sysfs_remove_group(kernel_kobj, &attr_group);
    printk(KERN_INFO "pagecache_inspector: Module unloaded. Goodbye kernel!\n");
}

MODULE_LICENSE("GPL");
