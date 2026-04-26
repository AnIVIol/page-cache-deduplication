#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/pagemap.h>
#include <linux/memcontrol.h>
#include "dedup_shared.h"

static unsigned long trace_ino = 0;
module_param(trace_ino, ulong, 0644);
MODULE_PARM_DESC(trace_ino, "Inode number to trace for ghost writes");

int split_single_page(struct address_space *mapping, pgoff_t index)
{
    struct shared_cache_node *node;
    struct shared_mapping_item *item, *target_item = NULL;
    struct page *shared_page, *new_page;
    struct folio *new_folio;

    unsigned long flags;
    rcu_read_lock();
    shared_page = xa_load(&mapping->i_pages, index);
    rcu_read_unlock();

    if (!shared_page || !PageSharedCache(shared_page))
        return 0;

    node = (struct shared_cache_node *)((unsigned long)shared_page->mapping & ~0x7UL);

    spin_lock(&node->lock);
    list_for_each_entry(item, &node->mapping_list, list) {
        if (item->mapping == mapping && item->index == index) {
            target_item = item;
            break;
        }
    }

    if (!target_item) {
        spin_unlock(&node->lock);
        return -ENOENT;
    }
    spin_unlock(&node->lock);

    new_page = alloc_page(GFP_ATOMIC | __GFP_HIGHMEM);
    if (!new_page)
        return -ENOMEM;

    new_folio = page_folio(new_page);
    
    if (mem_cgroup_charge(new_folio, NULL, GFP_ATOMIC)) {
        folio_put(new_folio);
        return -ENOMEM;
    }
   
    copy_highpage(new_page, node->survivor_page);
//TODO: check if needed: 
    detach_page_private(new_page); 
    clear_page_dirty_for_io(new_page);

    new_page->mapping = mapping;
    new_page->index = index; 
    set_page_count(new_page, 1);
    SetPageUptodate(new_page);    

    INIT_LIST_HEAD(&new_page->lru);
    //Restore Memory Accounting
    node_stat_mod_folio(new_folio, NR_FILE_PAGES, 1);

    lru_cache_add(new_page);
    
    xa_lock_irq(&mapping->i_pages);
    __xa_store(&mapping->i_pages, index, new_page, GFP_ATOMIC);
    xa_unlock_irq(&mapping->i_pages);


    spin_lock(&node->lock);
    list_del_init(&target_item->list); 
    kfree(target_item);
    atomic_dec(&node->refcount);
    
    put_page(node->survivor_page); 

    if (list_is_singular(&node->mapping_list)) {
        spin_unlock(&node->lock);

        spin_lock_irqsave(&global_dedup_lock, flags);
        if (!list_empty(&node->global_list)) {
            list_del_init(&node->global_list);
        }
        spin_unlock_irqrestore(&global_dedup_lock, flags);

        unmerge_shared_node(node); 
    } else {
        spin_unlock(&node->lock);
    }

    return 0;
}
EXPORT_SYMBOL(split_single_page);

static struct kprobe kp = {
    .symbol_name = "__filemap_get_folio",
};

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    /* * x86_64 Calling Convention:
     * %rdi = mapping (arg0)
     * %rsi = index   (arg1)
     * %rdx = fgp_flags (arg2)
     */
    struct address_space *mapping = (struct address_space *)regs->di;
    pgoff_t index = (pgoff_t)regs->si;
    int fgp_flags = (int)regs->dx;
    
    struct inode *inode;

    if (!mapping || !mapping->host) return 0;
    inode = mapping->host;

    if (trace_ino != 0 && inode->i_ino == trace_ino) {
        
        pr_info("PAGE_TRACER: PID %d (%s) requested index %lu on inode %lu (Flags: 0x%x)\n", 
                current->pid, current->comm, index, inode->i_ino, fgp_flags);

        // Dump the stack ONLY if it touches an index > 1
       // if (index > 1) {
            pr_info("PAGE_TRACER: --- STACK DUMP FOR INDEX %lu ---\n", index);
            dump_stack();
       // }
    }
    if (fgp_flags & (FGP_WRITE | FGP_CREAT | FGP_LOCK)) {
      
        rcu_read_lock();
        struct page *page = xa_load(&mapping->i_pages, index);
        rcu_read_unlock();
//	pr_info("pre handler for flags %lu mapping %lu\n", fgp_flags, (unsigned long) mapping);
        if (page && PageSharedCache(page)) {
    //       pr_info("interceptor: Write detected on shared PFN %lu. Splitting...\n",page_to_pfn(page));
            
            split_single_page(mapping, index);
        }
    }

    return 0;
}

static void handler_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
}

static int __init interceptor_init(void)
{
    kp.pre_handler = handler_pre;
    kp.post_handler = handler_post;

    int ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("interceptor: register_kprobe failed, returned %d\n", ret);
        return ret;
    }

    pr_info("interceptor: Write-trap active on __filemap_get_folio\n");
    return 0;
}

static void __exit interceptor_exit(void)
{
    unregister_kprobe(&kp);
    pr_info("interceptor: Write-trap removed\n");
}

module_init(interceptor_init);
module_exit(interceptor_exit);
MODULE_LICENSE("GPL");

