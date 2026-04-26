#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/dcache.h> /* For d_find_alias and dput */
#include <linux/rmap.h>   /* For rmap_walk */

/* --- CS614 Dedup Structures --- */
#define PAGE_MAPPING_SHARED_CACHE 0x4UL

struct shared_mapping_item {
    struct address_space *mapping;
    pgoff_t index;
    struct list_head list;
};
struct shared_cache_node {

        struct list_head mapping_list; /* Linked list of address_space pointers */
//TODO: will need rb_tree pointers here later
        struct list_head global_list;
        struct page *survivor_page;

        spinlock_t lock;
        atomic_t refcount;
};

/* ------------------------------------------------------------- */

static struct kobject *inspect_kobj;

/* * 1. Our Custom Callback for the Kernel's rmap_walk()
 * The kernel will call this for EVERY process that has this page mapped.
 */
static bool inspect_rmap_one(struct folio *folio, struct vm_area_struct *vma,
                             unsigned long address, void *arg)
{
    pr_info("    [RMAP] -> Mapped in Process mm: %px | VMA: %px | VirtAddr: 0x%lx\n",
            vma->vm_mm, vma, address);
    return true; /* Return true to keep walking the tree */
}

/* Helper function to safely print the filename from an inode */
static void print_human_readable_file(struct inode *inode)
{
    struct dentry *dentry;
    
    if (!inode) {
        pr_info("    Owning File: [Unknown/Deleted Inode]\n");
        return;
    }

    /* Ask the Directory Cache for a name associated with this inode */
    dentry = d_find_alias(inode);
    if (dentry) {
        /* %pd is the kernel format specifier to print a dentry's name safely */
        pr_info("    Owning File Name: %pd (Inode: %lu)\n", dentry, inode->i_ino);
        dput(dentry); /* Must release the reference! */
    } else {
        pr_info("    Owning File Name: [No dentry found] (Inode: %lu)\n", inode->i_ino);
    }
}


static ssize_t inspect_pfn_store(struct kobject *kobj, struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    unsigned long pfn;
    struct page *page;
    struct folio *folio;
    unsigned long mapping_val;
    struct rmap_walk_control rwc = {
        .rmap_one = inspect_rmap_one,
    };

    if (kstrtoul(buf, 10, &pfn)) return -EINVAL;
    if (!pfn_valid(pfn)) return -EINVAL;

    page = pfn_to_page(pfn);
    pr_info("\n========== PFN %lu INSPECTION ==========\n", pfn);
    pr_info("Refcount: %d | Mapcount: %d | Flags: 0x%lx\n", page_count(page), page_mapcount(page), page->flags);
    
    folio = page_folio(page);


    if (page_count(page) == 0) {
        pr_info("Status: FREED (Buddy Allocator owns this page)\n");
        pr_info("========================================\n");
        return count;
    }

    mapping_val = (unsigned long)page->mapping;

    if (!mapping_val) {
        pr_info("Status: Unmapped or Anonymous without swap space.\n");
    } 
    /* YOUR CS614 TRICK */
    else if (mapping_val & PAGE_MAPPING_SHARED_CACHE) {
        struct shared_cache_node *node = (void *)(mapping_val & ~0x7UL);
        struct shared_mapping_item *item;
        int file_count = 0;

        pr_info("Status: CS614 SHARED DEDUP PAGE (Survivor)\n");
        
        spin_lock(&node->lock);
        list_for_each_entry(item, &node->mapping_list, list) {
            struct inode *inode = item->mapping ? item->mapping->host : NULL;
            pr_info("  --- Reference %d ---\n", ++file_count);
            print_human_readable_file(inode);
            pr_info("    Index (Offset): %lu\n", item->index);
        }
        spin_unlock(&node->lock);
    } 
    /* Standard File / Page Cache */
    else if (!(mapping_val & PAGE_MAPPING_ANON) && !(mapping_val & PAGE_MAPPING_KSM)) {
        struct address_space *mapping = (struct address_space *)(mapping_val & ~0x3UL);
        struct inode *inode = mapping ? mapping->host : NULL;
        
        pr_info("Status: Standard Page Cache (File)\n");
        print_human_readable_file(inode);
        pr_info("    Page Index (Offset): %lu\n", page->index);
    } 
    else {
        pr_info("Status: Anonymous or KSM Page\n");
    }

    /* 3. Execute the standard kernel Reverse-Map Walk */
    if (page_mapcount(page) > 0) {
        pr_info("  --- Executing Kernel Rmap Walk ---\n");
        /* This will trigger inspect_rmap_one() for every process mapping this page */
        rmap_walk(folio, &rwc);
    } else {
        pr_info("  --- Rmap Walk: Page is in Page Cache but not currently mmap'd by any user process ---\n");
    }

    pr_info("========================================\n");
    return count;
}

static struct kobj_attribute inspect_attr = __ATTR(inspect, 0200, NULL, inspect_pfn_store);

static int __init pfn_inspect_init(void)
{
    inspect_kobj = kobject_create_and_add("pfn_inspector", kernel_kobj);
    if (!inspect_kobj) return -ENOMEM;
    sysfs_create_file(inspect_kobj, &inspect_attr.attr);
    return 0;
}

static void __exit pfn_inspect_exit(void)
{
    sysfs_remove_file(inspect_kobj, &inspect_attr.attr);
    kobject_put(inspect_kobj);
}

module_init(pfn_inspect_init);
module_exit(pfn_inspect_exit);
MODULE_LICENSE("GPL");
