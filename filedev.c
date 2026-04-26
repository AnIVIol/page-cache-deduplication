static unsigned long count_dirty_pages(struct address_space *mapping)
{
    unsigned long dirty_count = 0;
    pgoff_t index = 0;
    void *entry;

    rcu_read_lock();
    xa_for_each_marked(&mapping->i_pages, index, entry, PAGECACHE_TAG_DIRTY) {
        dirty_count++;
    }
    rcu_read_unlock();

    return dirty_count;
}

static struct fdevop user_op;
static struct fdevstat current_stat;

static ssize_t demo_read(struct file *filp, char __user *ubuf, size_t length, loff_t *offset)
{ 
    
    struct file *target_file;
    struct inode *target_inode;
    if (length != sizeof(struct fdevstat))
        return -EINVAL;
    
    target_file = fget(user_op.fd);
    if (!target_file)
        return -EBADF;

    target_inode = file_inode(target_file);

    memset(&current_stat, 0, sizeof(struct fdevstat));

    if (user_op.op & FDEVOP_FPOS) {
        current_stat.f_pos = target_file->f_pos;
    }
    
    if (user_op.op & FDEVOP_FSIZE) {
        current_stat.f_size = i_size_read(target_inode);
    }
    
    if (user_op.op & FDEVOP_INUM) {
        current_stat.f_inum = target_inode->i_ino;
    }

    if (user_op.op & FDEVOP_FLUSH) {
        filemap_write_and_wait(target_inode->i_mapping);
    }

    if (user_op.op & FDEVOP_PCSTAT) {
        current_stat.cached_pages = target_inode->i_mapping->nrpages;
        current_stat.dirty_cached = count_dirty_pages(target_inode->i_mapping);
    }

    fput(target_file);
    if (copy_to_user(ubuf, &current_stat, sizeof(struct fdevstat)))
        return -EFAULT;

    return length;
}

static ssize_t demo_write(struct file *filp, const char __user *buff, size_t len, loff_t *off)
{
    if (len != sizeof(struct fdevop))
        return -EINVAL;

    if (copy_from_user(&user_op, buff, sizeof(struct fdevop)))
        return -EFAULT;

    return len;
}
static unsigned long count_dirty_pages(struct inode *inode)
{
    struct address_space *mapping = inode->i_mapping;
    unsigned long dirty_count = 0;
    pgoff_t index;
    pgoff_t last_index = (i_size_read(inode) - 1) >> PAGE_SHIFT;

    for (index = 0; index <= last_index; index++) {
        struct page *page = xa_load(&mapping->i_pages, index);

        if (page && !xa_is_value(page)) {
            if (PageDirty(page)) {
                dirty_count++;
            }
        }
    }
    return dirty_count;
}
